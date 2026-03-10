#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lockdefs.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_btree_merge_pages);

/*
 * Walk from metapage root down to the leftmost leaf by always following the
 * first downlink on each internal page.
 */
static BlockNumber
find_leftmost_leaf(Relation rel)
{
	Buffer		metabuf;
	Page		metapage;
	BTMetaPageData *metad;
	BlockNumber curblk;
	int			level = 0;

	metabuf = ReadBuffer(rel, BTREE_METAPAGE);
	metapage = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapage);
	curblk = metad->btm_root;

	ereport(NOTICE,
			(errmsg("LOG: Metapage - root block: %u, level: %u",
					(unsigned int) metad->btm_root,
					(unsigned int) metad->btm_level)));
	ereport(NOTICE, (errmsg("LOG: Descending to leftmost leaf...")));

	ReleaseBuffer(metabuf);

	for (;;)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;

		buf = ReadBuffer(rel, curblk);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);

		ereport(NOTICE,
				(errmsg("  Level %d, block %u, is_leaf=%d",
						level,
						(unsigned int) curblk,
						P_ISLEAF(opaque) ? 1 : 0)));

		if (P_ISLEAF(opaque))
		{
			ereport(NOTICE,
					(errmsg("LOG: Reached leftmost leaf at block %u",
							(unsigned int) curblk)));
			ReleaseBuffer(buf);
			return curblk;
		}
		else
		{
			OffsetNumber offnum = P_FIRSTDATAKEY(opaque);
			ItemId		itemid = PageGetItemId(page, offnum);
			IndexTuple itup = (IndexTuple) PageGetItem(page, itemid);
			BlockNumber child = BTreeTupleGetDownLink(itup);

			ereport(NOTICE,
					(errmsg("  Level %d downlink: parent %u offset %u -> child %u",
							level,
							(unsigned int) curblk,
							(unsigned int) offnum,
							(unsigned int) child)));

			curblk = child;
			level++;
			ReleaseBuffer(buf);
		}
	}
}

static uint64
log_leaf_chain_state(Relation rel, BlockNumber startblk)
{
	BlockNumber blkno = startblk;
	uint64		page_count = 0;

	while (blkno != P_NONE)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber maxoff;
		Size		free_space;
		BlockNumber nextblk;

		buf = ReadBuffer(rel, blkno);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);

		maxoff = PageGetMaxOffsetNumber(page);
		free_space = PageGetExactFreeSpace(page);
		page_count++;

		ereport(NOTICE,
				(errmsg("  Page %llu: level=%u, items=%u, free=%zu bytes, flags=0x%x",
						(unsigned long long) page_count,
						(unsigned int) opaque->btpo_level,
						(unsigned int) maxoff,
						(size_t) free_space,
						(unsigned int) opaque->btpo_flags)));

		nextblk = opaque->btpo_next;
		ReleaseBuffer(buf);
		blkno = nextblk;
	}

	ereport(NOTICE,
			(errmsg("  Total leaf pages: %llu", (unsigned long long) page_count)));

	return page_count;
}

/*
 * Check if data items from lpage can fit into rpage's free space.
 */
static bool
can_merge_pages(Page lpage, Page rpage)
{
	BTPageOpaque lopaque = BTPageGetOpaque(lpage);
	OffsetNumber lmaxoff = PageGetMaxOffsetNumber(lpage);
	Size rfree = PageGetExactFreeSpace(rpage);
	Size needed = 0;
	OffsetNumber i;
	OffsetNumber lstart;

	/* Only data items are copied */
	if (!P_RIGHTMOST(lopaque))
		lstart = OffsetNumberNext(FirstOffsetNumber);
	else
		lstart = FirstOffsetNumber;

	for (i = lstart; i <= lmaxoff; i = OffsetNumberNext(i))
	{
		ItemId itemid = PageGetItemId(lpage, i);
		needed += MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);
	}

	return (needed <= rfree);
}

/*
 * Copy all items (excluding high key) from lpage to rpage.
 *
 * NOTE: When merging Leaf A -> Leaf B, the combined range's high-key 
 * is ALREADY in B (the right page). The items in A are all smaller 
 * than any item in B and smaller than B's high-key.
 * 
 * We must keep B's high-key as the high-key for the combined page.
 * Items from A are prepended AFTER B's high-key to maintain global sort order.
 */
static void
merge_pages_items(Page lpage, Page rpage)
{
	BTPageOpaque lopaque = BTPageGetOpaque(lpage);
	OffsetNumber lmaxoff = PageGetMaxOffsetNumber(lpage);
	OffsetNumber i;
	OffsetNumber lstart;

	/* 
	 * Left Page high key remains valid for the "combined" new range 
	 * only if we were replacing B's high key. But we are KEEPING 
	 * the right sibling B, so we MUST keep B's high key.
	 * 
	 * Thus, we skip L's high-key and only copy L's data items.
	 */
	if (!P_RIGHTMOST(lopaque))
		lstart = OffsetNumberNext(FirstOffsetNumber);
	else
		lstart = FirstOffsetNumber;

	/* Prepend L's data items to R, just after R's high-key */
	for (i = lmaxoff; i >= lstart; i = OffsetNumberPrev(i))
	{
		ItemId itemid = PageGetItemId(lpage, i);
		IndexTuple itup = (IndexTuple) PageGetItem(lpage, itemid);
		Size itemsz = ItemIdGetLength(itemid);

		/* Insert at offset 2 (after R's High Key at offset 1) */
		if (PageAddItem(rpage, (const void *) itup, itemsz, 
						OffsetNumberNext(FirstOffsetNumber),
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to prepend data item from left page during merge");
	}
}

/*
 * Mark a page as half-dead by setting the BTP_HALF_DEAD flag.
 */
static void
mark_page_halfdead(Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	opaque->btpo_flags |= BTP_HALF_DEAD;
}

/*
 * Finalize page deletion once side links/downlinks are already updated.
 */
static void
mark_page_deleted(Page page)
{
	FullTransactionId safexid = GetCurrentFullTransactionId();
	BTPageSetDeleted(page, safexid);
}

/* 
 * Get the block number from a parent's item (either downlink or high-key)
 */
static BlockNumber
get_blkno_from_tuple(IndexTuple itup)
{
	return BTreeTupleGetDownLink(itup);
}

static uint64
run_merge_scan(Relation rel, BlockNumber startblk)
{
	BlockNumber lblkno = startblk;
	uint64		merged_count = 0;
	uint64		visited = 0;

	while (lblkno != P_NONE)
	{
		Buffer		lbuf, rbuf;
		Page		lpage, rpage;
		BTPageOpaque lopaque, ropaque;
		BlockNumber rblkno;

		/* Read left page */
		lbuf = ReadBuffer(rel, lblkno);
		lpage = BufferGetPage(lbuf);
		lopaque = BTPageGetOpaque(lpage);
		visited++;

		/* Get right sibling block number */
		rblkno = lopaque->btpo_next;

		/* If no right sibling, we're done with this page */
		if (rblkno == P_NONE)
		{
			ReleaseBuffer(lbuf);
			break;
		}

		/* Read right page */
		rbuf = ReadBuffer(rel, rblkno);
		rpage = BufferGetPage(rbuf);
		ropaque = BTPageGetOpaque(rpage);

		/* Try to merge if pages can fit together */
		if (can_merge_pages(lpage, rpage))
		{
			BlockNumber lprev = lopaque->btpo_prev;
			BlockNumber parentblk = P_NONE;
			OffsetNumber parentoff = InvalidOffsetNumber;
			Buffer		pbuf = InvalidBuffer;
			BlockNumber nblocks;
			BlockNumber curblk;
			bool		found_parent = false;

			/*
			 * Find parent downlink to L before we start the critical section.
			 */
			nblocks = RelationGetNumberOfBlocks(rel);
			for (curblk = 1; curblk < nblocks; curblk++)
			{
				Buffer		buf = ReadBuffer(rel, curblk);
				Page		page = BufferGetPage(buf);
				BTPageOpaque opaque;
				OffsetNumber i, maxoff;

				if (PageIsNew(page))
				{
					ReleaseBuffer(buf);
					continue;
				}

				opaque = BTPageGetOpaque(page);
				if (P_ISLEAF(opaque) || P_ISDELETED(opaque) || P_ISHALFDEAD(opaque))
				{
					ReleaseBuffer(buf);
					continue;
				}

				maxoff = PageGetMaxOffsetNumber(page);
				for (i = P_FIRSTDATAKEY(opaque); i <= maxoff; i = OffsetNumberNext(i))
				{
					ItemId		itemid = PageGetItemId(page, i);
					IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
					BlockNumber downlink = BTreeTupleGetDownLink(itup);

					if (downlink == lblkno)
					{
						parentblk = curblk;
						parentoff = i;
						pbuf = buf;
						found_parent = true;
						break;
					}
				}

				if (found_parent)
					break;

				ReleaseBuffer(buf);
			}

			if (!found_parent)
				elog(ERROR, "could not find parent downlink for block %u", lblkno);

			ereport(NOTICE,
					(errmsg("Merging pages: L=%u (%u items) -> R=%u (%u items)",
							(unsigned int) lblkno,
							(unsigned int) PageGetMaxOffsetNumber(lpage),
							(unsigned int) rblkno,
							(unsigned int) PageGetMaxOffsetNumber(rpage))));

			START_CRIT_SECTION();

			/* 1. Copy all items from left to right */
			merge_pages_items(lpage, rpage);

			/* 1.5 Mark source page half-dead while unlink is in progress */
			mark_page_halfdead(lpage);

			/* 2. Update R's previous link to L's previous link */
			ropaque->btpo_prev = lprev;

			/* 3. Update Left Sibling's next link to point to R (if it exists) */
			if (lprev != P_NONE)
			{
				Buffer		prev_buf = ReadBuffer(rel, lprev);
				Page		prev_page = BufferGetPage(prev_buf);
				BTPageOpaque prev_opaque = BTPageGetOpaque(prev_page);

				prev_opaque->btpo_next = rblkno;
				MarkBufferDirty(prev_buf);
				ReleaseBuffer(prev_buf);
			}

			/* 
			 * 3.5 Update the parent downlink target to point to R instead of L.
			 * This maintains the B-tree structure and high-key invariants.
			 */
			{
				Page		ppage = BufferGetPage(pbuf);
				BTPageOpaque popaque = BTPageGetOpaque(ppage);
				OffsetNumber maxoff = PageGetMaxOffsetNumber(ppage);

				/* 
				 * Redirection: Update L's downlink to R, then delete R's original
				 * downlink if it's not the same one. This preserves the 
				 * key range separator that actually describes R's contents.
				 */
				BTreeTupleSetDownLink((IndexTuple) PageGetItem(ppage, PageGetItemId(ppage, parentoff)), rblkno);
				
				/* Find and delete R's old downlink in the same parent */
				for (OffsetNumber i = P_FIRSTDATAKEY(popaque); i <= maxoff; i = OffsetNumberNext(i))
				{
					IndexTuple itup = (IndexTuple) PageGetItem(ppage, PageGetItemId(ppage, i));
					if (BTreeTupleGetDownLink(itup) == rblkno && i != parentoff)
					{
						PageIndexTupleDelete(ppage, i);
						ereport(NOTICE, (errmsg("LOG: Deleted redundant downlink to %u at parent %u offset %u",
												rblkno, parentblk, (unsigned int) i)));
						break;
					}
				}
				
				MarkBufferDirty(pbuf);
				ereport(NOTICE, (errmsg("LOG: Updated parent %u offset %u downlink: %u -> %u",
										parentblk, (unsigned int) parentoff, lblkno, rblkno)));
			}

			/* 4. Update left page's next pointer to point to R */
			lopaque->btpo_next = rblkno;

			/* 5. Final state after unlink work is complete: DELETED */
			mark_page_deleted(lpage);

			/* Mark both buffers dirty */
			MarkBufferDirty(lbuf);
			MarkBufferDirty(rbuf);

			END_CRIT_SECTION();

			ReleaseBuffer(pbuf);
			merged_count++;

			/* Move to the right page (which now contains merged data) */
			ReleaseBuffer(lbuf);
			ReleaseBuffer(rbuf);
			lblkno = rblkno;
		}
		else
		{
			/* Can't merge, move to right page */
			ReleaseBuffer(rbuf);
			ReleaseBuffer(lbuf);
			lblkno = rblkno;
		}
	}

	ereport(NOTICE,
			(errmsg("LOG: Merge scan completed. visited=%llu merged=%llu",
					(unsigned long long) visited,
					(unsigned long long) merged_count)));

	return merged_count;
}

Datum
pg_btree_merge_pages(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	Relation	idxrel;
	BlockNumber leftmost;
	uint64		merged_count;

	idxrel = relation_open(index_oid, AccessExclusiveLock);

	if (idxrel->rd_rel->relkind != RELKIND_INDEX)
		elog(ERROR, "relation \"%s\" is not an index", RelationGetRelationName(idxrel));

	if (idxrel->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "index \"%s\" is not a btree index", RelationGetRelationName(idxrel));

	ereport(NOTICE, (errmsg("=== pg_btree_merge_pages: START ===")));

    ereport(NOTICE, (errmsg("=== DURING MERGE ===")));
	leftmost = find_leftmost_leaf(idxrel);
	merged_count = run_merge_scan(idxrel, leftmost);

	ereport(NOTICE,
			(errmsg("=== pg_btree_merge_pages: COMPLETE - merged %llu page pairs ===",
					(unsigned long long) merged_count)));

	relation_close(idxrel, AccessExclusiveLock);

	PG_RETURN_VOID();
}
