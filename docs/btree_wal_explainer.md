# PostgreSQL B-Tree WAL — How It Works and How to Write WAL Code

> A companion document to `btree_page_merge_plan.md`.
> Explains how PostgreSQL's Write-Ahead Logging works for B-tree operations,
> how to write WAL-logged code, and how the merge operation's WAL fits in.
>
> **Design principle:** Our merge adds one new WAL record type and one new
> replay function.  No existing WAL record, replay function, or WAL
> infrastructure code is modified.  We use the same `XLogRegisterBuffer`,
> `XLogInsert`, `_bt_restore_page`, and critical-section patterns that
> every other B-tree operation already uses.

---

## Table of Contents

1. [What Is WAL and Why It Exists](#1-what-is-wal-and-why-it-exists)
2. [The WAL Contract](#2-the-wal-contract)
3. [How PostgreSQL B-Tree Operations Use WAL](#3-how-postgresql-b-tree-operations-use-wal)
4. [Anatomy of a WAL Record](#4-anatomy-of-a-wal-record)
5. [Writing WAL Code — Step by Step](#5-writing-wal-code--step-by-step)
6. [Buffer Registration Flags](#6-buffer-registration-flags)
7. [The Critical Section Pattern](#7-the-critical-section-pattern)
8. [WAL Replay (REDO)](#8-wal-replay-redo)
9. [Full Page Images (FPI)](#9-full-page-images-fpi)
10. [Existing B-Tree WAL Records](#10-existing-b-tree-wal-records)
11. [How Our Merge WAL Record Works](#11-how-our-merge-wal-record-works)
12. [Common Pitfalls](#12-common-pitfalls)

---

## 1. What Is WAL and Why It Exists

PostgreSQL does not write data pages directly to their final on-disk
location after every change.  Instead, it writes a sequential **log** of
changes first — the Write-Ahead Log.  Only after this log is safely on
disk does the actual data page get written (lazily, by the background
writer or checkpointer).

**The guarantee:** if the system crashes, PostgreSQL can replay the WAL
from the last checkpoint to reconstruct every change that was committed.
This is called **crash recovery** or **REDO**.

```
  Normal operation:

  1. Modify page in shared buffer pool (in memory)
  2. Write WAL record describing the change → WAL buffer → disk
  3. Mark buffer dirty
  4. Eventually, bgwriter/checkpointer writes dirty page to disk
  5. Checkpoint advances → old WAL can be recycled

  After crash:

  1. Start from last checkpoint
  2. Read WAL records sequentially
  3. For each record: if the page's LSN < record's LSN → apply (REDO)
  4. When done: database is consistent, as if no crash happened
```

### Why Not Just Write Pages Directly?

- **Random I/O is slow.** Writing 100 scattered 8KB pages means 100
  random disk seeks.  Writing 100 WAL records is one sequential append.
- **Atomicity.** A single WAL record can describe changes to multiple
  pages.  Either all changes are replayed or none.  Direct page writes
  can tear (half-written pages).
- **Durability without latency.**  `fsync()` on a sequential log is
  fast.  `fsync()` on 100 random pages is not.

---

## 2. The WAL Contract

Every piece of code that modifies a shared buffer page must follow this
contract:

1. **Log before you modify** (conceptually).  In practice, you modify
   the page first, then write the WAL record, but it all happens inside
   a **critical section** (`START_CRIT_SECTION()` / `END_CRIT_SECTION()`)
   that prevents `ereport(ERROR)` from interrupting you mid-change.

2. **The WAL record must contain enough information to reconstruct the
   change from scratch**, assuming only the WAL record and the page's
   previous on-disk state (or a full page image).

3. **Set the page's LSN** to the WAL record's LSN after writing.  This
   lets replay know whether the page has already been updated past this
   record.

4. **Mark the buffer dirty** (`MarkBufferDirty()`).  This tells the
   bgwriter/checkpointer to eventually write the page to disk.

```c
/* The canonical pattern: */
START_CRIT_SECTION();

    /* Modify pages in shared buffers */
    modify_page_A(...);
    modify_page_B(...);
    modify_page_C(...);

    MarkBufferDirty(bufA);
    MarkBufferDirty(bufB);
    MarkBufferDirty(bufC);

    /* Construct and write WAL record */
    if (RelationNeedsWAL(rel))
    {
        XLogBeginInsert();
        XLogRegisterBuffer(0, bufA, flags_A);
        XLogRegisterBuffer(1, bufB, flags_B);
        XLogRegisterBuffer(2, bufC, flags_C);
        XLogRegisterData((char *) &xlrec, sizeof(xlrec));
        XLogRecPtr recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_SOMETHING);

        PageSetLSN(pageA, recptr);
        PageSetLSN(pageB, recptr);
        PageSetLSN(pageC, recptr);
    }

END_CRIT_SECTION();
```

---

## 3. How PostgreSQL B-Tree Operations Use WAL

Each B-tree operation that modifies pages emits one or more WAL records.

| Operation | WAL Record(s) | Pages Modified |
|-----------|---------------|----------------|
| Insert a tuple | `XLOG_BTREE_INSERT_LEAF` | 1 (leaf page) |
| Split a page | `XLOG_BTREE_SPLIT_L` or `_R` | 2-3 (left, right, maybe parent) |
| Mark page half-dead | `XLOG_BTREE_MARK_PAGE_HALFDEAD` | 2 (leaf + parent) |
| Unlink deleted page | `XLOG_BTREE_UNLINK_PAGE` | 3 (left sib, target, right sib) + maybe parent |
| Deduplication | `XLOG_BTREE_DEDUP` | 1 (leaf page) |
| Vacuum cleanup | `XLOG_BTREE_VACUUM` | 1 (leaf page) |
| **Our merge** | `XLOG_BTREE_MERGE` (new) | 3 (source, destination, parent) |

### The Two-Phase Delete Pattern

Page deletion in PostgreSQL B-tree is **two WAL records**:

```
  Record 1: XLOG_BTREE_MARK_PAGE_HALFDEAD
    - Remove downlink from parent
    - Mark leaf as BTP_HALF_DEAD
    - Pages: leaf + parent

  Record 2: XLOG_BTREE_UNLINK_PAGE
    - Remove page from sibling chain
    - Mark as BTP_DELETED with safexid
    - Pages: left_sib + target + right_sib (+ maybe parent)
```

Our merge follows the same pattern:
- Record 1: `XLOG_BTREE_MERGE` (move data + parent update + half-dead)
- Record 2: `XLOG_BTREE_UNLINK_PAGE` (sibling unlink — existing code)

A crash between the two records is safe: the source is half-dead with
intact sibling links.  The next VACUUM detects the half-dead page and
finishes the unlink.

---

## 4. Anatomy of a WAL Record

A WAL record has these components:

```
  ┌─────────────────────────────────────────────────┐
  │  XLogRecord header (fixed)                      │
  │    - xl_tot_len:  total record length            │
  │    - xl_xid:      transaction ID                 │
  │    - xl_info:     operation type (e.g. MERGE)    │
  │    - xl_rmid:     resource manager (RM_BTREE_ID) │
  │    - xl_prev:     LSN of previous record         │
  ├─────────────────────────────────────────────────┤
  │  Main data (XLogRegisterData)                   │
  │    - Your custom struct (e.g. xl_btree_merge)   │
  │    - Contains operation-specific parameters      │
  ├─────────────────────────────────────────────────┤
  │  Block reference 0                               │
  │    - RelFileLocator + fork + block number        │
  │    - Flags (WILL_INIT, STANDARD, etc.)           │
  │    - Optional: full page image (FPI)             │
  │    - Optional: per-block data (XLogRegisterBufData) │
  ├─────────────────────────────────────────────────┤
  │  Block reference 1                               │
  │    ...                                           │
  ├─────────────────────────────────────────────────┤
  │  Block reference 2                               │
  │    ...                                           │
  └─────────────────────────────────────────────────┘
```

### Key Concepts

- **Resource Manager (rmid):** Each subsystem (heap, btree, hash, etc.)
  has its own resource manager that knows how to replay its WAL records.
  B-tree is `RM_BTREE_ID`.  The replay dispatch is in
  `src/backend/access/nbtree/nbtxlog.c` → `btree_redo()`.

- **xl_info:** Identifies the specific operation within the resource
  manager.  For B-tree: `XLOG_BTREE_INSERT_LEAF`, `XLOG_BTREE_SPLIT_L`,
  etc.  We add `XLOG_BTREE_MERGE`.

- **Block references:** Each WAL record references 0+ disk blocks.
  The WAL infrastructure automatically tracks which relation/fork/block
  each reference points to.  During replay, `XLogReadBufferForRedo()`
  uses this to find and lock the right buffer.

- **Main data vs. per-block data:**
  - Main data (`XLogRegisterData`): operation parameters shared across
    all blocks (e.g., parent offset, tuple count).
  - Per-block data (`XLogRegisterBufData`): data specific to one block
    (e.g., the packed tuple bytes for the destination page).

---

## 5. Writing WAL Code — Step by Step

Here is exactly how to write WAL-logged code for a new B-tree operation,
using our merge as the running example.

### Step 1: Define the WAL Record Struct

In `src/include/access/nbtxlog.h`:

```c
/*
 * xl_btree_merge -- WAL record for XLOG_BTREE_MERGE.
 *
 * Describes a leaf page merge: source (L) merged into destination (R),
 * source marked half-dead, parent pivot updated.
 *
 * Block references:
 *   0 = destination page (rebuilt with REGBUF_WILL_INIT)
 *   1 = source page      (rebuilt as half-dead with REGBUF_WILL_INIT)
 *   2 = parent page      (pivot surgery with REGBUF_STANDARD)
 */
typedef struct xl_btree_merge
{
    OffsetNumber poffset;       /* L's downlink offset in parent */
    uint16       ntuples;       /* total tuples on rebuilt destination */
} xl_btree_merge;

#define SizeOfBtreeMerge  (offsetof(xl_btree_merge, ntuples) + sizeof(uint16))
```

**Why these fields?**

- `poffset`: replay needs to know WHERE in the parent page to do the
  pivot surgery (redirect downlink, delete next pivot).
- `ntuples`: replay needs to know how many tuples to unpack from the
  per-block data into the destination page.

**Why so few fields?**  Because block numbers are stored automatically
by the block reference mechanism — we don't need `lblkno`, `rblkno`,
or `parentblkno` in the struct.  The WAL infrastructure handles that.

### Step 2: Assign a WAL Record Type

In `src/include/access/nbtxlog.h`:

```c
#define XLOG_BTREE_MERGE    0xF0    /* leaf page merge */
```

The value must not collide with existing `XLOG_BTREE_*` values.
Convention: check existing values in `nbtxlog.h` and pick an unused one.

### Step 3: Write the WAL Record (in the critical section)

In the merge function, after modifying all three pages:

```c
START_CRIT_SECTION();

/* === Modify pages (see merge algorithm) === */

/* 1. Replace destination page content with merged page */
memcpy(dstpage, merged_page, BLCKSZ);

/* 2. Mark source as half-dead */
lopaque->btpo_flags |= BTP_HALF_DEAD;
/* ... set top-parent link in source's high key ... */

/* 3. Update parent: redirect downlink, delete pivot */
BTreeTupleSetDownLink(parent_itup, rblkno);
PageIndexTupleDelete(parentpage, OffsetNumberNext(poffset));

/* === Mark all buffers dirty === */
MarkBufferDirty(dstbuf);
MarkBufferDirty(srcbuf);
MarkBufferDirty(parentbuf);

/* === Construct WAL record === */
if (RelationNeedsWAL(rel))
{
    xl_btree_merge xlrec;
    xlrec.poffset = poffset;
    xlrec.ntuples = ntuples;

    XLogBeginInsert();

    /*
     * Block 0: destination — completely rebuilt.
     * REGBUF_WILL_INIT tells replay to initialize the page from scratch
     * rather than reading the old version.  We provide the full tuple
     * data as per-block data.
     */
    XLogRegisterBuffer(0, dstbuf, REGBUF_WILL_INIT);
    XLogRegisterBufData(0, (char *) packed_tuples, packed_len);

    /*
     * Block 1: source — becomes a half-dead tombstone.
     * Also REGBUF_WILL_INIT because we rebuild it completely.
     * The per-block data is the truncated top-parent link tuple
     * (just like _bt_mark_page_halfdead stores).
     */
    XLogRegisterBuffer(1, srcbuf, REGBUF_WILL_INIT);
    XLogRegisterBufData(1, (char *) halfdead_data, halfdead_len);

    /*
     * Block 2: parent — incremental change (redirect + delete).
     * REGBUF_STANDARD means the WAL infrastructure may take a full
     * page image if needed (first modification since checkpoint),
     * but normally just logs the delta.
     */
    XLogRegisterBuffer(2, parentbuf, REGBUF_STANDARD);

    /* Main data: the operation parameters */
    XLogRegisterData((char *) &xlrec, SizeOfBtreeMerge);

    /* Emit the record — returns the LSN */
    XLogRecPtr recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MERGE);

    /* Stamp all three pages with the record's LSN */
    PageSetLSN(BufferGetPage(dstbuf), recptr);
    PageSetLSN(BufferGetPage(srcbuf), recptr);
    PageSetLSN(BufferGetPage(parentbuf), recptr);
}

END_CRIT_SECTION();
```

### Step 4: Pack Tuples for WAL

The destination page's tuples need to be serialized into a flat byte
array for the WAL record.  PostgreSQL already has a pattern for this
in `_bt_split()` (nbtinsert.c).  The packed format is simply concatenated
`IndexTuple`s with sizes rounded up to `MAXALIGN`.

```c
/*
 * Pack all tuples from the merged page into a flat buffer.
 * This is the same format _bt_restore_page() expects during replay.
 */
static char *
_bt_pack_tuples(Page page, BTPageOpaque opaque, Size *packlen)
{
    OffsetNumber minoff = P_FIRSTDATAKEY(opaque);
    OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
    Size         total = 0;

    /* First pass: compute total size */
    for (OffsetNumber off = minoff; off <= maxoff; off++)
    {
        ItemId iid = PageGetItemId(page, off);
        total += MAXALIGN(ItemIdGetLength(iid));
    }

    char *buf = palloc(total);
    char *ptr = buf;

    /* Second pass: copy tuples */
    for (OffsetNumber off = minoff; off <= maxoff; off++)
    {
        ItemId     iid = PageGetItemId(page, off);
        Size       itupsz = ItemIdGetLength(iid);
        IndexTuple itup = (IndexTuple) PageGetItem(page, iid);

        memcpy(ptr, itup, itupsz);
        memset(ptr + itupsz, 0, MAXALIGN(itupsz) - itupsz);  /* zero padding */
        ptr += MAXALIGN(itupsz);
    }

    *packlen = total;
    return buf;
}
```

### Step 5: Write the Replay Function

In `src/backend/access/nbtree/nbtxlog.c`:

```c
static void
btree_xlog_merge(XLogReaderState *record)
{
    xl_btree_merge *xlrec =
        (xl_btree_merge *) XLogRecGetData(record);
    XLogRecPtr lsn = record->EndRecPtr;

    /* --- Block 0: Destination page (full rebuild) --- */
    {
        Buffer buf = XLogInitBufferForRedo(record, 0);
        Page   page = BufferGetPage(buf);
        char  *data;
        Size   datalen;

        _bt_pageinit(page, BLCKSZ);

        /* Unpack tuples using the existing _bt_restore_page() */
        data = XLogRecGetBlockData(record, 0, &datalen);
        _bt_restore_page(page, data, datalen);

        PageSetLSN(page, lsn);
        MarkBufferDirty(buf);
        UnlockReleaseBuffer(buf);
    }

    /* --- Block 1: Source page (rebuild as half-dead) --- */
    {
        Buffer         buf = XLogInitBufferForRedo(record, 1);
        Page           page = BufferGetPage(buf);
        BTPageOpaque   opaque;
        char          *data;
        Size           datalen;

        _bt_pageinit(page, BLCKSZ);
        opaque = BTPageGetOpaque(page);
        opaque->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;

        /* Restore the truncated top-parent link tuple */
        data = XLogRecGetBlockData(record, 1, &datalen);
        if (datalen > 0)
            _bt_restore_page(page, data, datalen);

        PageSetLSN(page, lsn);
        MarkBufferDirty(buf);
        UnlockReleaseBuffer(buf);
    }

    /* --- Block 2: Parent page (incremental pivot surgery) --- */
    {
        Buffer buf;

        if (XLogReadBufferForRedo(record, 2, &buf) == BLK_NEEDS_REDO)
        {
            Page        page = BufferGetPage(buf);
            BlockNumber rblkno;

            /* Get destination block number from block ref 0 */
            XLogRecGetBlockTag(record, 0, NULL, NULL, &rblkno);

            /* Redirect L's downlink → R */
            ItemId     iid = PageGetItemId(page, xlrec->poffset);
            IndexTuple itup = (IndexTuple) PageGetItem(page, iid);
            BTreeTupleSetDownLink(itup, rblkno);

            /* Delete R's now-redundant pivot entry */
            PageIndexTupleDelete(page, OffsetNumberNext(xlrec->poffset));

            PageSetLSN(page, lsn);
            MarkBufferDirty(buf);
        }
        if (BufferIsValid(buf))
            UnlockReleaseBuffer(buf);
    }
}
```

### Step 6: Register the Replay Function

In `nbtxlog.c`, find the `btree_redo()` dispatch function and add:

```c
void
btree_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    switch (info)
    {
        case XLOG_BTREE_INSERT_LEAF:
            btree_xlog_insert(true, false, false, record);
            break;
        /* ... existing cases ... */
        case XLOG_BTREE_MERGE:           /* NEW */
            btree_xlog_merge(record);
            break;
        default:
            elog(PANIC, "btree_redo: unknown op code %u", info);
    }
}
```

### Step 7: Add Human-Readable Description

In `src/backend/access/rmgrdesc/nbtdesc.c`, add a case to `btree_desc()`:

```c
case XLOG_BTREE_MERGE:
{
    xl_btree_merge *xlrec = (xl_btree_merge *) rec;
    appendStringInfo(buf, "poffset %u, ntuples %u",
                     xlrec->poffset, xlrec->ntuples);
    break;
}
```

And in `btree_identify()`:

```c
case XLOG_BTREE_MERGE:
    id = "MERGE";
    break;
```

This makes `pg_waldump` output human-readable:

```
rmgr: Btree  len: 8234 tx: 12345 lsn: 0/1234 desc: MERGE poffset 3, ntuples 47
```

---

## 6. Buffer Registration Flags

### `REGBUF_WILL_INIT`

Use when the page is **completely rebuilt** from scratch during replay.
The replay function calls `XLogInitBufferForRedo()` which allocates a
fresh buffer and the code reinitializes it from the WAL data.

**Effect:** the WAL infrastructure does NOT need to save a full page
image (FPI) of the old page content, because replay doesn't need it —
it rebuilds from scratch.  This saves WAL space.

We use this for **both source and destination**:
- Destination: completely rebuilt with merged tuple data.
- Source: completely rebuilt as a half-dead tombstone.

### `REGBUF_STANDARD`

Use when the page is **incrementally modified** — replay applies a delta
to the existing page content.  The replay function calls
`XLogReadBufferForRedo()` which reads the existing page (or restores it
from a full page image if one was included).

We use this for the **parent page**: we only redirect one downlink and
delete one pivot entry.  The rest of the parent page is unchanged.

### `REGBUF_NO_IMAGE`

Use when you explicitly don't want a full page image.  Rarely used.

### When Does a Full Page Image (FPI) Get Written?

For `REGBUF_STANDARD` blocks: the WAL infrastructure automatically
includes an FPI if this is the **first modification to the page since
the last checkpoint**.  This protects against torn pages (partial writes).

For `REGBUF_WILL_INIT` blocks: no FPI is ever written, because replay
doesn't need the old page content.

---

## 7. The Critical Section Pattern

### Why Critical Sections Exist

Between modifying a page and writing its WAL record, the system is in
an inconsistent state: the in-memory page has been changed, but the WAL
record hasn't been written yet.  If an `ereport(ERROR)` fires in this
window, the page would be modified without a WAL record → corruption
after crash.

`START_CRIT_SECTION()` increments a counter that makes `ereport(ERROR)`
escalate to `PANIC` (immediate crash) — which is actually safer, because
then the old WAL is replayed and the half-done change is undone.

### The Rule

**Between `START_CRIT_SECTION()` and `END_CRIT_SECTION()`, no `ereport(ERROR)`
can fire.**  This means:

1. All memory allocation (`palloc()`) must happen BEFORE the critical section.
2. All validation checks must happen BEFORE the critical section.
3. All buffer reads must happen BEFORE the critical section.
4. Inside the critical section: only `memcpy`, `MarkBufferDirty`,
   `XLog*` calls, and `PageSetLSN`.

```c
/* === BEFORE critical section: allocate, validate, prepare === */
PGAlignedBlock merged_buf;
Page merged_page = merged_buf.data;
/* ... build merged page, run all checks, pack tuples ... */
char *packed = _bt_pack_tuples(merged_page, mopaque, &packed_len);

/* === CRITICAL SECTION: modify pages + write WAL === */
START_CRIT_SECTION();
    memcpy(dstpage, merged_page, BLCKSZ);          /* can't fail */
    lopaque->btpo_flags |= BTP_HALF_DEAD;           /* can't fail */
    BTreeTupleSetDownLink(parent_itup, rblkno);      /* can't fail */
    PageIndexTupleDelete(parentpage, nextoff);        /* can't fail */
    MarkBufferDirty(dstbuf);                         /* can't fail */
    MarkBufferDirty(srcbuf);                         /* can't fail */
    MarkBufferDirty(parentbuf);                      /* can't fail */
    /* WAL calls — can't fail */
    XLogBeginInsert();
    /* ... register blocks and data ... */
    recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MERGE);
    PageSetLSN(..., recptr);                         /* can't fail */
END_CRIT_SECTION();
```

---

## 8. WAL Replay (REDO)

### How Replay Works

During crash recovery, `StartupXLOG()` reads WAL records sequentially.
For each record, it calls the appropriate resource manager's `redo`
function.  For B-tree records, that's `btree_redo()` in `nbtxlog.c`.

### The LSN Check

Before replaying a record on a page, PostgreSQL checks:

```c
if (PageGetLSN(page) >= record->EndRecPtr)
    /* Page is already up-to-date — skip */
```

This makes replay **idempotent**.  If the page was already written to disk
after the WAL record was generated, replaying the record again is a no-op.

### XLogInitBufferForRedo vs XLogReadBufferForRedo

- **`XLogInitBufferForRedo(record, block_id)`**: For `REGBUF_WILL_INIT`
  blocks.  Allocates a buffer, does NOT read old content.  Always
  returns the buffer (no LSN check needed — the page is rebuilt from
  scratch).

- **`XLogReadBufferForRedo(record, block_id, &buf)`**: For
  `REGBUF_STANDARD` blocks.  Reads the existing page (or restores from
  FPI).  Returns `BLK_NEEDS_REDO` if replay is needed, `BLK_RESTORED`
  if an FPI was applied (already up-to-date), or `BLK_DONE` if the page
  LSN shows it's already past this record.

---

## 9. Full Page Images (FPI)

### The Torn Page Problem

A disk write of an 8KB page is not guaranteed to be atomic.  If the
system crashes mid-write, the page on disk might be half old content,
half new content — a **torn page**.

### How FPI Solves It

The first time a page is modified after a checkpoint, the WAL record
includes a complete image of the page (an FPI).  During replay, if the
page on disk is torn, the FPI restores it to a known-good state, and
then subsequent WAL records apply on top of it.

### Impact on Our Merge

- **Destination and source** use `REGBUF_WILL_INIT` → no FPI ever.
  Replay rebuilds them from scratch.  This is efficient.
- **Parent** uses `REGBUF_STANDARD` → FPI on first modification since
  checkpoint.  An FPI adds ~8KB to the WAL record.  This is the same
  cost as any other B-tree operation that modifies the parent.

---

## 10. Existing B-Tree WAL Records

For reference, here are the existing B-tree WAL records defined in
`src/include/access/nbtxlog.h`:

```c
#define XLOG_BTREE_INSERT_LEAF        0x00  /* insert tuple on leaf */
#define XLOG_BTREE_INSERT_UPPER       0x10  /* insert tuple on internal */
#define XLOG_BTREE_INSERT_META        0x20  /* insert + metapage update */
#define XLOG_BTREE_SPLIT_L            0x30  /* split: orig is left */
#define XLOG_BTREE_SPLIT_R            0x40  /* split: orig is right */
#define XLOG_BTREE_INSERT_POST        0x50  /* insert posting tuple */
#define XLOG_BTREE_DEDUP              0x60  /* deduplicate page */
#define XLOG_BTREE_VACUUM             0x70  /* vacuum cleanup */
#define XLOG_BTREE_DELETE             0x80  /* delete tuples */
#define XLOG_BTREE_MARK_PAGE_HALFDEAD 0x90  /* phase 1 of delete */
#define XLOG_BTREE_UNLINK_PAGE        0xA0  /* phase 2 of delete */
#define XLOG_BTREE_UNLINK_PAGE_META   0xB0  /* phase 2 + meta update */
#define XLOG_BTREE_NEWROOT            0xC0  /* new root page */
#define XLOG_BTREE_META_CLEANUP       0xD0  /* metapage cleanup */

/* Our new record: */
#define XLOG_BTREE_MERGE              0xF0  /* leaf page merge */
```

### The Split Record as a Model

Our merge WAL record is structured similarly to `XLOG_BTREE_SPLIT_L/R`:

| Aspect | Split | Merge |
|--------|-------|-------|
| Pages modified | 2-3 (left, right, maybe parent) | 3 (source, destination, parent) |
| Page rebuilt from scratch | Right page (`REGBUF_WILL_INIT`) | Source + destination (`REGBUF_WILL_INIT`) |
| Tuple data in WAL | Right page tuples (packed) | Destination tuples (packed) |
| Replay function | `btree_xlog_split()` | `btree_xlog_merge()` |
| Uses `_bt_restore_page()` | Yes (right page) | Yes (destination page) |
| Parent update | Separate `INSERT_UPPER` record | Included in same record |

---

## 11. How Our Merge WAL Record Works

### Record Structure

```
  XLOG_BTREE_MERGE record:
  ┌──────────────────────────────────────────────┐
  │  XLogRecord header                           │
  │    rmid = RM_BTREE_ID                        │
  │    info = XLOG_BTREE_MERGE                   │
  ├──────────────────────────────────────────────┤
  │  Main data: xl_btree_merge                   │
  │    poffset = 3                               │
  │    ntuples = 47                              │
  ├──────────────────────────────────────────────┤
  │  Block 0: destination page                   │
  │    flags = REGBUF_WILL_INIT                  │
  │    data  = 47 packed IndexTuples (~6 KB)     │
  ├──────────────────────────────────────────────┤
  │  Block 1: source page                        │
  │    flags = REGBUF_WILL_INIT                  │
  │    data  = 1 truncated top-parent tuple      │
  ├──────────────────────────────────────────────┤
  │  Block 2: parent page                        │
  │    flags = REGBUF_STANDARD                   │
  │    data  = (none — replay uses poffset)      │
  │    maybe = FPI if first mod since checkpoint  │
  └──────────────────────────────────────────────┘
```

### Size Estimate

| Component | Size |
|-----------|------|
| XLogRecord header | ~24 bytes |
| xl_btree_merge | 4 bytes |
| Block ref headers (3×) | ~60 bytes |
| Destination tuple data | ~2-6 KB (depends on merged tuples) |
| Source half-dead data | ~16 bytes |
| Parent FPI (if needed) | 0 or ~8 KB |
| **Total (no parent FPI)** | **~2-6 KB** |
| **Total (with parent FPI)** | **~10-14 KB** |

This is comparable to a page split record (~4-8 KB).

### Crash Safety Analysis

| Crash Point | WAL State | Recovery Result |
|-------------|-----------|-----------------|
| Before `XLogInsert()` returns | Record not flushed | Merge never happened.  All pages revert to pre-merge state. |
| After `XLOG_BTREE_MERGE`, before `XLOG_BTREE_UNLINK_PAGE` | Merge record flushed, unlink not | `btree_xlog_merge()` replays: destination has data, source is half-dead, parent updated.  Source still in sibling chain.  Next VACUUM finds it half-dead and calls `_bt_unlink_halfdead_page()` to finish. |
| After both records flushed | Both present | Both replay.  Source fully deleted with safexid.  Clean state. |

---

## 12. Common Pitfalls

### Pitfall 1: Allocating Memory in the Critical Section

```c
/* WRONG — palloc can fail with ERROR */
START_CRIT_SECTION();
    char *buf = palloc(1024);   /* ← PANIC if out of memory */
END_CRIT_SECTION();

/* RIGHT — allocate before */
char *buf = palloc(1024);
START_CRIT_SECTION();
    memcpy(buf, data, len);     /* ← can't fail */
END_CRIT_SECTION();
```

### Pitfall 2: Forgetting to Set LSN

```c
/* WRONG — page has no LSN, replay will re-apply record */
XLogRecPtr recptr = XLogInsert(...);
/* oops, forgot PageSetLSN */

/* RIGHT */
XLogRecPtr recptr = XLogInsert(...);
PageSetLSN(page, recptr);
```

### Pitfall 3: Wrong Buffer Registration Flag

```c
/* WRONG — using REGBUF_STANDARD for a page you're rebuilding */
XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
/* Replay will try to read old page content — but you've overwritten it */

/* RIGHT — use REGBUF_WILL_INIT for completely rebuilt pages */
XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);
/* Replay initializes a fresh page and rebuilds from WAL data */
```

### Pitfall 4: Not Handling BLK_NEEDS_REDO

```c
/* WRONG — modifying page without checking if redo is needed */
buf = XLogReadBufferForRedo(record, 2, &buf);
/* just modify it... */

/* RIGHT — check return value */
if (XLogReadBufferForRedo(record, 2, &buf) == BLK_NEEDS_REDO)
{
    /* Apply changes */
    PageSetLSN(page, lsn);
    MarkBufferDirty(buf);
}
if (BufferIsValid(buf))
    UnlockReleaseBuffer(buf);
```

### Pitfall 5: Forgetting MarkBufferDirty

If you modify a page but don't call `MarkBufferDirty()`, the
bgwriter/checkpointer won't know to write it to disk.  The change
exists only in the WAL.  After a clean shutdown (no replay), the
change is lost.

---

*This document is a companion to `btree_page_merge_plan.md`.
It explains the WAL mechanics; the plan document covers the
algorithm, concurrency, and design decisions.*
