# B-Tree Page Merge — Implementation Plan

> **GSoC 2026 — B-tree Index Bloat Reduction (Page Merge)**
>
> A detailed design document for implementing leaf-level page merging in
> PostgreSQL B-tree indexes.  Incorporates mentor feedback from Kirk Wolak.
>
> The merge always moves data from a left leaf page (source) into its right
> sibling (destination) and then deletes the source page — reusing the same
> parent-pivot and sibling-link surgery that `_bt_pagedel()` already does.
> Key space always moves right, matching PostgreSQL's existing concurrency
> protocol.  Both pages must share the same parent (README line 273).
> A new user-facing command drives the operation; VACUUM only recycles the
> tombstone pages left behind.
>
> Companion document: `btree_wal_explainer.md` — explains how PostgreSQL
> WAL works and how to write WAL-logged code.

---

## Table of Contents

1.  [Problem Statement](#1-problem-statement)
2.  [Design Philosophy](#2-design-philosophy)
3.  [Merge Direction: Always L → R](#3-merge-direction-always-l--r)
4.  [Merge Strategy: Small Into Large When Possible](#4-merge-strategy-small-into-large-when-possible)
5.  [Merge Candidate Identification & GUC Thresholds](#5-merge-candidate-identification--guc-thresholds)
6.  [The Merge Algorithm](#6-the-merge-algorithm)
7.  [Locking Protocol](#7-locking-protocol)
8.  [High Key & Pivot Tuple Invariants](#8-high-key--pivot-tuple-invariants)
9.  [Source Page Deletion — Why We Must Delete Immediately](#9-source-page-deletion--why-we-must-delete-immediately)
10. [WAL Logging](#10-wal-logging)
11. [Crash Recovery (REDO)](#11-crash-recovery-redo)
12. [Concurrency: How Concurrent Scans See the Merge](#12-concurrency-how-concurrent-scans-see-the-merge)
13. [The `FIX BLOAT` Command](#13-the-fix-bloat-command)
14. [Edge Cases & Restrictions](#14-edge-cases--restrictions)
15. [Reusable Code Inventory](#15-reusable-code-inventory)
16. [Testing Strategy](#16-testing-strategy)
17. [Files to Modify](#17-files-to-modify)
18. [Implementation Phases](#18-implementation-phases)

---

## 1. Problem Statement

B-tree indexes become **bloated** after heavy UPDATE/DELETE workloads.  VACUUM
removes dead tuples and deletes completely empty pages, but it does NOT merge
*partially-filled* pages.  A table that has had 80% of its rows deleted can
have an index where every leaf page is only 10–20% full — 4–5× larger than
necessary.

```
 After heavy DELETE cycles:
 ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
 │██    │ │█     │ │███   │ │█     │ │██    │ │█     │ │██    │ │█     │
 │      │ │      │ │      │ │      │ │      │ │      │ │      │ │      │
 └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘
 Each page: 5–20% full.  Total: 8 pages.  Useful data fits in 2–3 pages.

 After iterative pairwise merges:
 ┌──────┐ ┌──────┐ ┌──────┐
 │██████│ │█████ │ │████  │   Pages freed: 5 out of 8
 │██    │ │██    │ │      │   Fill: 50–70%
 └──────┘ └──────┘ └──────┘
```

### Why This Matters

| Solution               | Lock Level                              | Downtime      | Extra Space  |
|-------------------------|-----------------------------------------|---------------|--------------|
| `REINDEX`               | `AccessExclusiveLock`                   | Minutes–hours | In-place     |
| `REINDEX CONCURRENTLY`  | `ShareUpdateExclusiveLock`              | Very long     | **2× index** |
| `pg_repack`             | External tool                           | Long          | **2× disk**  |
| **Page Merge (this)**   | Cleanup lock src+dst, `BT_WRITE` parent | **None**      | **None**     |

---

## 2. Design Philosophy

### Core Principle: Pure Extension, Zero Modification

This project is a **pure addition** to PostgreSQL's B-tree implementation.
It does not modify, replace, or alter any existing code path:

- **No existing function is changed.**  We add new functions that *call*
  existing ones (`_bt_search`, `_bt_lock_subtree_parent`,
  `_bt_unlink_halfdead_page`, `_bt_pgaddtup`, `_bt_restore_page`, etc.).
  The existing functions remain byte-for-byte identical.
- **No existing behavior is altered.**  Inserts, deletes, splits, scans,
  VACUUM, autovacuum, `REINDEX`, `CLUSTER` — all work exactly as before,
  whether or not the merge feature exists.
- **No existing WAL record is modified.**  We add one new record type
  (`XLOG_BTREE_MERGE`) and one new `case` in the replay dispatch.  All
  existing records and replay functions are untouched.
- **No existing concurrency protocol is changed.**  We rely on the same
  `P_IGNORE` / `_bt_moveright()` / cleanup-lock / safexid-drain mechanisms
  that page deletion already uses.  No new locking primitives.
- **The feature is entirely opt-in.**  Nothing happens unless the DBA
  explicitly runs `FIX BLOAT`.  There is no background worker, no
  autovacuum hook, no automatic trigger.  A PostgreSQL instance with this
  patch behaves 100% identically to one without it — until the DBA
  decides to use it.
- **Revert safety.**  If the patch were reverted, PostgreSQL would behave
  identically to how it does today.  No migration, no cleanup, no
  compatibility concerns.

This design minimizes review burden, regression risk, and community
push-back.  The patch adds a new capability without touching any existing
code path that users and other subsystems depend on.

### What We DO

1. **Detect** a bloated leaf page whose right same-parent sibling can
   absorb its data.
2. **Merge** the left page's (source) data into the right sibling
   (destination).
3. **Update the parent** — redirect the source page's downlink and remove
   the redundant pivot entry, identical to `_bt_mark_page_halfdead()`.
4. **Delete the source page** — mark it `BTP_HALF_DEAD`, then unlink it
   from the sibling chain via `_bt_unlink_halfdead_page()`.
5. **Leave the tombstone** for VACUUM to recycle via `BTPageIsRecyclable()`
   when the `safexid` ages out.

**Why a `FIX BLOAT` command and NOT part of VACUUM?**  VACUUM already has
a well-defined scope: remove dead tuples and delete empty pages.  Merging
partially-filled pages is a different concern — it reorganizes live data.
Coupling it with VACUUM would add complexity to an already critical code
path and make it harder to control.  A separate `FIX BLOAT` command lets the DBA
decide *when* and *how aggressively* to merge, without interfering with
VACUUM's autovacuum scheduling.

### What We DON'T Do

- ❌ We do NOT merge internal (non-leaf) pages — leaf only.
  **Why:** Internal pages contain downlinks (child pointers).  Moving a
  downlink means the child's parent has changed, creating cascading
  updates up and down the tree.  Leaf pages only contain data tuples (TIDs),
  so moving them has no structural side effects.

- ❌ We do NOT merge the rightmost page on a level.
  **Why:** Same restriction as `_bt_pagedel()`.  The rightmost page has an
  implicit +∞ high key and no right sibling.  All traversal algorithms
  (`_bt_moveright()`, `_bt_readnextpage()`) assume a rightmost page is
  never deleted.  Relaxing this would require changes across the entire
  B-tree concurrency protocol.

- ❌ We do NOT merge pages that belong to different parents.
  **Why:** README line 273: "the parent's key space assignment changes too,
  meaning we'd have to make bounding-key updates in its parent, and perhaps
  all the way up the tree.  Since we can't possibly do that atomically, we
  forbid this case."  See Section 3 for the full analysis.

- ❌ We do NOT invent new concurrency protocols.
  **Why:** PostgreSQL's B-tree concurrency protocol (Lehman & Yao with
  left-links) has been refined for 25+ years.  Adding a new protocol (like
  "move left" recovery) would be a research project, not a GSoC project.
  We reuse the existing `P_IGNORE` / `_bt_moveright()` / drain technique
  that page deletion already uses.

- ❌ We do NOT couple with VACUUM.
  **Why:** VACUUM only recycles the tombstone pages we leave behind.  There
  is zero dependency between `FIX BLOAT` and VACUUM's scan logic.  This
  means we don't need to worry about autovacuum timing, VACUUM lock conflicts,
  or VACUUM's dead-tuple thresholds affecting our merge decisions.

### Why We MUST Delete the Source Page Immediately

If the source page were left empty but "live" (parent downlink and high key
intact), inserts with keys in its range would descend to it via
`_bt_findinsertloc()` (nbtinsert.c:886) and re-fill it before VACUUM could
delete it.  **Net result: zero bloat reduction.**

**Why not just mark it "don't insert here"?**  There is no such flag in
PostgreSQL's B-tree.  The insert path (`_bt_search()` → `_bt_moveright()` →
`_bt_findinsertloc()`) routes inserts based purely on key range and parent
downlinks.  Adding a "no insert" flag would require modifying the entire
insert code path and would need its own concurrency protocol.

By atomically removing the source page's parent downlink and marking it
`BTP_HALF_DEAD`, inserts can never reach it again.  The parent directs all
keys in the source page's former range to the destination page.  This is
the same mechanism `_bt_pagedel()` already uses — zero new code needed.

---

## 3. Merge Direction: Always L → R

### Why Only L → R

PostgreSQL's B-tree concurrency protocol is built around a single
invariant: **key space only ever moves right**.  Every mechanism relies
on this:

- **`_bt_moveright()`** (nbtsearch.c:302): when a reader descends from a
  parent to a child page and finds it `P_IGNORE` (half-dead/deleted) or
  its key has moved past the high key (concurrent page split), it moves
  **right** via `btpo_next`.  There is no corresponding "move left"
  recovery path in `_bt_moveright()`.

- **Page deletion** (`_bt_mark_page_halfdead()`, nbtpage.c:2218):
  overwrites the deleted page's parent downlink with the **right
  sibling's** block number.  Key space transfers right.

- **The README** (line 237) explicitly acknowledges:
  > "Merging partly-full pages would allow better space reuse, but it
  > seems impractical to move existing data items left or right to make
  > this happen — **a scan moving in the opposite direction might miss
  > the items if so.**"

### The Concrete R→L Failure

If we merged R → L (key space moves left), a concurrent reader would fail:

```
 1. Reader descends from parent to R (parent had R's downlink)
 2. Between parent read and R read, the merge runs:
    - R's data moves to L (left)
    - R is marked BTP_HALF_DEAD
    - Parent's downlink for R is removed
 3. Reader arrives at R, finds P_IGNORE
 4. _bt_moveright() moves RIGHT to S (R's right sibling)
 5. Data is on L (to the LEFT) — reader moves AWAY from the data
 6. Reader misses tuples.  CORRECTNESS VIOLATION. ❌
```

**Why can't we fix this with locks?**  The reader held a lock on the parent
when it read R's downlink, but it releases the parent lock before
descending to R (standard B-tree protocol — you never hold a parent lock
while locking a child).  The merge can run in this gap.  This is NOT a bug
— it's the normal operation of Lehman & Yao trees.  The fix is
`_bt_moveright()`, which assumes data moved RIGHT.

**Why doesn't backward scan have the same problem?**  Backward scans at the
leaf level (`_bt_readnextpage()`, nbtsearch.c:1909) follow `btpo_prev` links,
which correctly handle half-dead pages.  The problem is specifically with
`_bt_search()` descent from parent to child, which uses `_bt_moveright()`.

**Why not add a "move left" recovery?**  Because it would break the
fundamental concurrency invariant.  A reader that moved left could overshoot
past a concurrent split (splits move data right too).  The entire L&Y
protocol assumes right-only recovery.

Therefore: **source is always L, destination is always R, key space
moves right.**  This is identical to what `_bt_pagedel()` does.

### Same-Parent Constraint

L and R must be children of the same parent.  We validate this after
locking the parent by checking that both downlinks exist on the same
parent page (see Section 7).

The nbtree README (line 273) is explicit:

> "To preserve consistency on the parent level, we cannot merge the key
> space of a page into its right sibling unless the right sibling is a
> child of the same parent — otherwise, the parent's key space assignment
> changes too, meaning we'd have to make bounding-key updates in its
> parent, and perhaps all the way up the tree."

**Why can't we update the grandparent too?**  Consider L as the rightmost
child of parent P1, R as the leftmost child of parent P2.  Deleting L
means P1's key space shrinks.  The grandparent's pivot between P1 and P2
would need updating.  If P1 is also the rightmost child of *its* parent,
the update cascades further.  In the worst case, you'd need write locks
from the leaf to the root — effectively locking the entire tree.
Additionally, updating P1's high key might require a physically larger
key tuple that doesn't fit in the available space.  The existing code
avoids all of this by never changing high keys during deletion (README
line 253-254).

---

## 4. Merge Strategy: Small Into Large When Possible

### The Preference

When scanning for merge candidates, we **prefer** to merge a smaller
(more bloated) left page into a larger (less bloated) right page.  This
is the natural and efficient case: fewer tuples to move, smaller WAL
record, shorter lock hold time.

```
 Preferred case — small L into large R:
 L: 30 tuples, 5% full (bloated)    →   merge 30 tuples into R
 R: 400 tuples, 60% full (healthy)

 Result: R has 430 tuples.  L freed.  Only 30 tuples moved.
```

### When the Right Page Is the Bloated One

Because direction is fixed (always L→R), when R is the bloated page and
L is the healthy one, we still merge L→R.  This means moving the larger
page's data:

```
 Reversed case — large L into small R:
 L: 400 tuples, 60% full (healthy)  →   merge 400 tuples into R
 R: 30 tuples, 5% full (bloated)

 Result: R has 430 tuples.  L freed.  400 tuples moved.
```

**Why is this acceptable despite moving more data?**

1. **It's all in-memory.**  Both pages are pinned and locked in the shared
   buffer pool.  Copying 400 tuples (~6 KB) in memory takes microseconds.
   The dominant costs are disk I/O (reading pages into buffers) and the WAL
   flush — not the `memcpy`.

2. **WAL is the only measurable difference.**  A WAL record with 400 packed
   tuples (~6 KB) vs. 30 packed tuples (~480 bytes).  But PostgreSQL
   routinely writes 8 KB full page images (FPIs) for every page modified
   after a checkpoint.  An extra few KB per merge is noise in the WAL stream.

3. **Lock hold time is marginally longer.**  Copying 400 tuples under
   cleanup lock takes slightly more CPU than copying 30.  But we're talking
   microseconds vs. microseconds — no concurrent reader will notice the
   difference.

4. **The alternative is far worse.**  Not merging at all (because the
   "wrong" page is bloated) means the bloat persists.  Moving 400 tuples
   is infinitely better than moving 0.

5. **This case is actually rare.**  The common bloat scenario is many
   adjacent pages that are ALL sparsely filled (e.g., after a bulk DELETE
   that removes 90% of rows uniformly).  The "healthy L, bloated R"
   scenario requires non-uniform deletion patterns, which are less common.

6. **The result is identical either way.**  Whether we move 30 tuples or
   400 tuples, the end state is one page with 430 tuples and one freed
   page.  The I/O savings from freeing a page far outweigh the marginal
   cost of copying more tuples in memory.

### The Scan Strategy

The scan proceeds left-to-right.  For each page P:

1. If P is below the low threshold, check if P's right same-parent
   sibling R can absorb P.  If yes → merge P into R.  **This is the
   "small into large" case** (P is small, R is typically larger or
   at least has room).

2. If P is NOT below threshold, skip it.  But P's right sibling might
   be below threshold — that sibling will be examined when the scan
   reaches it.  At that point, the sibling becomes the new L, and its
   own right sibling becomes R.

3. **What about the rightmost-child restriction?**  If P is the rightmost
   child of its parent, it cannot be deleted (it would require cross-parent
   key space movement — see Section 3).  We skip it.  However, the page
   to P's right (in the next parent) may itself be a candidate when the
   scan reaches it.

**Why not check BOTH directions?**  We cannot merge R→L (Section 3).  So
there is no point checking whether L can absorb R's data.  The only merge
operation that exists is "L is source, R is destination."  If R is bloated,
it will be merged when the scan reaches R (as the new L) with R's own right
sibling.

### Impact on Merge Opportunities

| Scenario | L below threshold? | Can merge? | Notes |
|----------|-------------------|------------|-------|
| L=5%, R=60% | Yes | Yes if L+R ≤ high_threshold | **Preferred**: small into large |
| L=60%, R=5% | No | No | R is bloated but L is not a candidate.  R will be a candidate when scanned as L of the (R, R.right) pair. |
| L=5%, R=5% | Yes | Yes if L+R ≤ high_threshold | Both small — merge L into R |
| L=5%, R=90% | Yes | No | Combined fill > high_threshold |
| L=5%, R=rightmost | Yes | Yes! | R has no right sibling, but L can still merge INTO R (R is the destination, not the source) |
| L=rightmost of parent | — | No | L can't be deleted (cross-parent constraint) |

---

## 5. Merge Candidate Identification & GUC Thresholds

### GUC Parameters (Per Kirk's Guidance)

Kirk was explicit: thresholds must be **GUCs** so we can test with various
settings and let users control aggressiveness.

**Why GUCs and not hardcoded?**  Different workloads have different bloat
profiles.  An OLTP system with continuous small deletes might want
conservative merging (low_threshold = 5%).  A data warehouse that bulk-deletes
partitions might want aggressive merging (low_threshold = 30%).  GUCs let
the DBA tune without recompiling.

```c
/* New GUC: minimum fill percentage to trigger a merge */
int  btree_merge_low_threshold = 10;    /* default: 10% fill */
/* "I would be okay only merging pages that are 1% or less" — Kirk */

/* New GUC: maximum fill percentage after merge (don't overfill) */
int  btree_merge_high_threshold = 70;   /* default: 70% fill */
/* "Not merging past 70%" — Kirk */
/* Prevents creating pages that will immediately split on insert */
```

### Why These Specific Defaults

- **Low threshold (10%):** Only pages that are severely bloated are
  candidates.  A page at 10% fill has had ~90% of its tuples deleted.
  This is conservative — it avoids unnecessary churn from merging pages
  that are only moderately sparse.

  **Why not higher (e.g., 50%)?**  Merging at 50% would aggressively
  compact the index but could cause "merge-split cycles": merge two 50%
  pages into one 100% page, next insert splits it back into two.  10%
  ensures we only merge pages where the bloat is unambiguous.

  **Why not lower (e.g., 1%)?**  Kirk suggested 1% would be acceptable
  for a minimal implementation.  We default to 10% as a pragmatic middle
  ground.  The GUC lets users go as low as they want.

- **High threshold (70%):** The default `fillfactor` for B-tree indexes is
  90%.  Merging up to 70% leaves ~20% headroom for inserts before a split
  is needed.

  **Why not 90% (pack pages full)?**  That would leave zero headroom.
  The very next insert into the page would trigger a split, undoing the
  work.  70% provides a buffer.

  **Why not 50% (very conservative)?**  That would leave too much wasted
  space after merging.  Two 10% pages merged at 50% threshold = 20% page,
  freeing only one page.  At 70%, we can merge more aggressively while
  still leaving room for growth.

### GUC Definition (in `src/backend/utils/misc/guc_tables.c`)

```c
{
    {"btree_merge_low_threshold", PGC_USERSET, INDEX_SETTINGS,
        gettext_noop("Minimum fill percentage below which a leaf page "
                     "is considered a merge candidate."),
        NULL,
        GUC_UNIT_PERCENT
    },
    &btree_merge_low_threshold,
    10, 0, 50,    /* default=10, min=0, max=50 */
    NULL, NULL, NULL
},
{
    {"btree_merge_high_threshold", PGC_USERSET, INDEX_SETTINGS,
        gettext_noop("Maximum fill percentage of the destination page "
                     "after merging."),
        NULL,
        GUC_UNIT_PERCENT
    },
    &btree_merge_high_threshold,
    70, 30, 100,  /* default=70, min=30, max=100 */
    NULL, NULL, NULL
},
```

### Candidate Detection Functions

```c
/*
 * _bt_is_merge_candidate -- check if a leaf page is below the low threshold.
 *
 * Why check fill percentage and not tuple count?  Because pages can have
 * variable-width tuples.  A page with 10 wide tuples might be 80% full,
 * while a page with 10 narrow tuples might be 5% full.  Fill percentage
 * is the only metric that correctly captures "how much data is on this page
 * relative to its capacity."
 */
static bool
_bt_is_merge_candidate(Page page)
{
    Size    freespace = PageGetFreeSpace(page);
    Size    pagesize  = PageGetPageSize(page);
    int     fill_pct  = 100 - (int)(freespace * 100 / pagesize);

    return (fill_pct <= btree_merge_low_threshold);
}

/*
 * _bt_can_merge_into -- check if destination can absorb source's data
 *                       without exceeding the high threshold.
 *
 * We check BOTH physical fit (bytes) AND threshold compliance.
 *
 * Why two checks?  Physical fit is necessary (you can't add data that
 * doesn't fit).  Threshold compliance is a policy choice (don't create
 * overfull pages that will split immediately).  A page might have room
 * for the bytes but the resulting fill might exceed the threshold.
 */
static bool
_bt_can_merge_into(Page srcpage, Page dstpage)
{
    Size    dstfree    = PageGetFreeSpace(dstpage);
    Size    dstpagesize = PageGetPageSize(dstpage);
    BTPageOpaque sopaque = BTPageGetOpaque(srcpage);
    OffsetNumber minoff = P_FIRSTDATAKEY(sopaque);
    OffsetNumber maxoff = PageGetMaxOffsetNumber(srcpage);
    Size    srcdata = 0;

    for (OffsetNumber off = minoff; off <= maxoff; off++)
    {
        ItemId iid = PageGetItemId(srcpage, off);
        srcdata += MAXALIGN(ItemIdGetLength(iid)) + sizeof(ItemIdData);
    }

    if (dstfree < srcdata)
        return false;   /* doesn't physically fit */

    Size after_free = dstfree - srcdata;
    int  after_fill = 100 - (int)(after_free * 100 / dstpagesize);

    return (after_fill <= btree_merge_high_threshold);
}
```

---

## 6. The Merge Algorithm

### Overview

```
 BEFORE (L is source, R is destination):
 ┌─────────────┐    ┌─────────────┐
 │ Page L      │ →  │ Page R      │ → ...
 │ hi_key = 20 │    │ hi_key = 40 │
 │ [5, 10, 15] │    │ [25, 30]    │
 │ fill: 8%    │    │ fill: 35%   │
 └─────────────┘    └─────────────┘

           Parent: [(-∞)→L]  [(20)→R]  [(40)→S]

 AFTER:
                    ┌──────────────────┐
                    │ Page R           │ → ...
                    │ hi_key = 40      │
                    │ [5,10,15,25,30]  │
                    │ fill: 43%        │
                    └──────────────────┘

           Parent: [(-∞)→R]  [(40)→S]

 L is BTP_HALF_DEAD → then BTP_DELETED with safexid.
 Key space moved right — matching the existing concurrency protocol.
 Tombstone stays until safexid ages out.  VACUUM recycles it.
```

### Step by Step

Source is always L (left), destination is always R (right).  Key space
moves right, identical to `_bt_pagedel()`.  Locks are acquired
left-to-right.

```
 Phase 1: Merge + Delete Source
 ═════════════════════════════

 Step 0 — Build search stack
   Build a search stack to L (the source page) via _bt_search()
   (nbtsearch.c:99).  This gives us a path from the root to L's
   parent.  Same technique _bt_pagedel() uses.  Release the leaf
   buffer returned by _bt_search() — we re-acquire it ourselves
   with a cleanup lock.

   Why a search stack?  We need to find L's downlink in the parent.
   _bt_lock_subtree_parent() walks the stack to locate and lock the
   parent.  Building the stack requires traversing from root to leaf,
   so we do it once and reuse it.

 Step 1 — Lock LEFT page with cleanup lock
   lbuf = ReadBuffer(rel, lblkno)
   LockBufferForCleanup(lbuf)        /* bufmgr.c — waits for all pins */
   Validate L:
     - P_ISLEAF(lopaque)             — leaf pages only
     - !P_IGNORE(lopaque)            — not deleted or half-dead
     - !P_INCOMPLETE_SPLIT(lopaque)  — no pending split
     - !_bt_leftsib_splitflag(rel, lopaque->btpo_prev, lblkno)
       (nbtpage.c — same check _bt_pagedel() does)
   If any check fails → _bt_relbuf(rel, lbuf), _bt_freestack(stack), return.

   Why validate all these conditions?  Between building the search stack
   and acquiring the cleanup lock, the page may have been split, deleted,
   or had other structural changes.  We must re-validate.

 Step 2 — Lock RIGHT page with cleanup lock
   rblkno = lopaque->btpo_next
   rbuf = ReadBuffer(rel, rblkno)
   LockBufferForCleanup(rbuf)        /* left-to-right ordering — no deadlock */
   Validate R:
     - P_ISLEAF(ropaque)
     - !P_IGNORE(ropaque)
     - ropaque->btpo_prev == lblkno  — still siblings
     - !_bt_rightsib_halfdeadflag(rel, ropaque->btpo_next)
       (nbtpage.c — same check _bt_pagedel() does)
   Source is always L, destination is always R.
   Validate: _bt_can_merge_into(lpage, rpage)
   If any check fails → release both, free stack, return.

   Why check btpo_prev == lblkno?  A concurrent split could have inserted
   a new page between L and R.  If so, our merge candidate pair is invalid.

 Step 3 — Build merged page in temp buffer
   Use a PGAlignedBlock (same as _bt_split() in nbtinsert.c:1542):
     PGAlignedBlock merged_buf;
     Page merged_page = merged_buf.data;
     _bt_pageinit(merged_page, BLCKSZ);

   Copy destination's opaque data (flags, level, sibling links).
   Copy HIGH KEY from R (the destination page).
   Copy L's data tuples first (lower keys).
   Copy R's data tuples after (higher keys).
   Use _bt_pgaddtup() (nbtinsert.c:2626) for each tuple.

   Why build in a temp buffer?  We can't modify R in-place incrementally
   — if we added L's tuples to R one by one, we'd need to shift existing
   tuples to make room at the beginning (L's keys are lower).  Building a
   fresh page lets us lay out tuples in sorted order naturally: L's tuples
   first, R's tuples after.

   Why L's tuples first?  Because L's key range is entirely below R's
   (sibling ordering invariant: all keys in L < L.high_key ≤ all keys in R).
   So sorted order is: [L's tuples] [R's tuples].

 Step 4 — Lock parent, validate same-parent
   Use _bt_lock_subtree_parent() (nbtpage.c:2817) via the saved search
   stack.  This finds L's downlink at offset l_poffset and returns the
   parent buffer locked with BT_WRITE.

   CRITICAL — validate L and R share the same parent:
     r_poffset = OffsetNumberNext(l_poffset)
     if (BTreeTupleGetDownLink(parent[r_poffset]) != rblkno) → ABORT
   This is the same cross-check _bt_mark_page_halfdead() performs at
   nbtpage.c:2182.

   Why abort instead of trying to find R on another parent?  Because
   cross-parent merges are forbidden (Section 3).  If R is on a different
   parent, the merge cannot proceed.

 Step 5 — Transfer predicate locks
   PredicateLockPageCombine(rel, lblkno, rblkno)
   Same call as _bt_mark_page_halfdead() at nbtpage.c:2201.

   Why?  Serializable Snapshot Isolation (SSI) uses predicate locks on
   index pages.  If a transaction holds a predicate lock on L's page,
   and we delete L, the lock must transfer to R so SSI can still detect
   conflicts.

 ═══ START_CRIT_SECTION() ═══

 Step 6 — Update parent
   Redirect L's downlink to point to R:
     BTreeTupleSetDownLink(parent[l_poffset], rblkno)
   Delete R's now-redundant pivot entry:
     PageIndexTupleDelete(parentpage, r_poffset)
   This is exactly what _bt_mark_page_halfdead() does at
   nbtpage.c:2218–2226: BTreeTupleSetDownLink() + PageIndexTupleDelete().
   Key space moves right.

 Step 7 — Replace R's page content
   memcpy(rpage, merged_page, BLCKSZ)

 Step 8 — Mark L as BTP_HALF_DEAD
   lopaque->btpo_flags |= BTP_HALF_DEAD
   Overwrite L's high key with a truncated top-parent link tuple
   (same as _bt_mark_page_halfdead() at nbtpage.c:2234–2245).

 Step 9 — Mark all three buffers dirty
   MarkBufferDirty(parentbuf)
   MarkBufferDirty(rbuf)
   MarkBufferDirty(lbuf)

 Step 10 — WAL log XLOG_BTREE_MERGE (see Section 10)

 Step 11 — Set LSN on all three pages

 ═══ END_CRIT_SECTION() ═══

 Step 12 — Release parent lock
   _bt_relbuf(rel, parentbuf)

 Step 13 — Release R lock
   _bt_relbuf(rel, rbuf)


 Phase 2: Unlink Source from Sibling Chain
 ═════════════════════════════════════════

 Step 14 — Call _bt_unlink_halfdead_page(rel, lbuf, ...)
   This existing function (nbtpage.c:2316) handles:
     - Locking neighbors in left-to-right order
     - Updating sibling links to bypass L
     - Marking L as BTP_DELETED with safexid =
       ReadNextFullTransactionId()
     - WAL logging: XLOG_BTREE_UNLINK_PAGE
     - L stays as tombstone until safexid ages out (drain technique)

   Why a separate phase?  Because this is exactly what _bt_pagedel()
   already does after marking a page half-dead.  By keeping Phase 1
   (merge + half-dead) and Phase 2 (unlink) separate, we can call the
   existing _bt_unlink_halfdead_page() with zero modifications.  A crash
   between the two phases is safe: the source is half-dead, and the next
   VACUUM finds it and finishes the unlink.

 Step 15 — Done.
   L is fully deleted.  R has the merged data.
   Parent is updated.  VACUUM will eventually recycle the source
   page via BTPageIsRecyclable().
```

### Sorted Insertion Detail

The merged page must maintain sorted order.  Since L's key range is
entirely below R's (sibling ordering invariant), we copy L's tuples
first, then R's.

**Page rebuild pattern** (matches `_bt_dedup_pass()` in nbtdedup.c:59 and
`_bt_split()` in nbtinsert.c:1542):

```c
PGAlignedBlock merged_buf;
Page merged_page = merged_buf.data;
_bt_pageinit(merged_page, BLCKSZ);

BTPageOpaque mopaque = BTPageGetOpaque(merged_page);
memcpy(mopaque, BTPageGetOpaque(dstpage), sizeof(BTPageOpaqueData));

/* High key always comes from R (the destination page).
 *
 * Why R's high key?  R's high key is the upper bound of R's key range.
 * After the merge, R owns BOTH L's and R's former key ranges.  Since
 * L's range was entirely below R's range, R's original high key is
 * still the correct upper bound for the combined range.
 *
 * L's high key (which was the separator between L and R) is no longer
 * needed — it becomes the deleted pivot in the parent.
 */
if (!P_RIGHTMOST(dopaque))
{
    ItemId   hitemid = PageGetItemId(rpage, P_HIKEY);
    Size     hitemsz = ItemIdGetLength(hitemid);
    IndexTuple hitem = (IndexTuple) PageGetItem(rpage, hitemid);
    if (PageAddItem(merged_page, (Item) hitem, hitemsz,
                    P_HIKEY, false, false) == InvalidOffsetNumber)
        elog(ERROR, "failed to add high key to merged page");
}

OffsetNumber afteroff = P_FIRSTDATAKEY(mopaque);

/* L's keys come first (lower), R's keys come second (higher). */
/* First: L's data tuples — skip LP_DEAD items (free mini-VACUUM) */
BTPageOpaque lopaque_t = BTPageGetOpaque(lpage);
for (OffsetNumber off = P_FIRSTDATAKEY(lopaque_t);
     off <= PageGetMaxOffsetNumber(lpage); off++)
{
    ItemId iid = PageGetItemId(lpage, off);
    if (ItemIdIsDead(iid))
        continue;   /* skip LP_DEAD items — don't copy garbage */
    IndexTuple itup = (IndexTuple) PageGetItem(lpage, iid);
    Size itupsz = ItemIdGetLength(iid);
    if (!_bt_pgaddtup(merged_page, itupsz, itup, afteroff, false))
        elog(ERROR, "failed to add tuple to merged page");
    afteroff = OffsetNumberNext(afteroff);
}
/* Then: R's data tuples — also skip LP_DEAD items */
BTPageOpaque ropaque_t = BTPageGetOpaque(rpage);
for (OffsetNumber off = P_FIRSTDATAKEY(ropaque_t);
     off <= PageGetMaxOffsetNumber(rpage); off++)
{
    ItemId iid = PageGetItemId(rpage, off);
    if (ItemIdIsDead(iid))
        continue;
    IndexTuple itup = (IndexTuple) PageGetItem(rpage, iid);
    Size itupsz = ItemIdGetLength(iid);
    if (!_bt_pgaddtup(merged_page, itupsz, itup, afteroff, false))
        elog(ERROR, "failed to add tuple to merged page");
    afteroff = OffsetNumberNext(afteroff);
}
```

---

## 7. Locking Protocol

### Why Three Locks

The merge modifies three pages atomically: the source page (marked
half-dead), the destination page (receives data), and the parent page
(downlink redirected, pivot entry removed).  This is the same number of
pages `_bt_mark_page_halfdead()` modifies.

**Why not two locks (skip parent)?**  The parent downlink MUST be
updated atomically with the data movement.  If we moved data but left
the parent pointing to L, inserts would still go to L (which now has
wrong/no data).  If we updated the parent but didn't move data, searches
would go to R which is missing L's tuples.

### Why Cleanup Locks on Source and Destination

**Why not regular BT_WRITE locks?**  `BT_WRITE` locks prevent concurrent
readers from acquiring NEW read locks, but they do NOT wait for readers
who already pinned the buffer and are about to read it.  A scanner that
pinned the source page, read some tuples, and is transitioning to the
destination could see duplicate data (forward scan) or miss data
(backward scan).

Cleanup locks (`LockBufferForCleanup()` in bufmgr.c:6701) are stronger:
they block until **no other backend has a pin on the buffer**.  This
guarantees:
- No scan is currently reading the source page
- No scan is currently reading the destination page
- No scan is transitioning between them (transitioning requires holding
  a pin on at least one)

**Why is LockBufferForCleanup() acceptable performance-wise?**  It's the
same mechanism VACUUM uses on every page it processes.  The wait is
typically very short — pins are held for microseconds during normal
scan operations.

### Lock Ordering

```
 Step 1: Build search stack via _bt_search()    (no leaf locks held)
 Step 2: Cleanup lock on LEFT page              (LockBufferForCleanup)
 Step 3: Cleanup lock on RIGHT page             (LockBufferForCleanup)
 Step 4: BT_WRITE on parent                     (_bt_lock_subtree_parent)

 Always left-to-right at leaf level, bottom-to-top across levels.
 This matches PostgreSQL's standard B-tree lock ordering.
```

**Why left-to-right?**  To prevent deadlocks.  If two concurrent merge
operations tried to merge adjacent pairs, they could deadlock if one
locked right-to-left.  By always locking left first, no cycle can form.

**Why leaf before parent?**  This is the same ordering `_bt_mark_page_halfdead()`
uses: hold lock on leaf child, then acquire write lock on parent.  The
reverse (parent then leaf) would deadlock with insert operations that lock
leaf then split up to parent.

### Same-Parent Validation

After locking the parent via `_bt_lock_subtree_parent()` (nbtpage.c:2817),
we verify both pages are children of the same parent:

```c
/* _bt_lock_subtree_parent() found L's downlink at poffset */
nextoff = OffsetNumberNext(poffset);
itemid  = PageGetItemId(parentpage, nextoff);
itup    = (IndexTuple) PageGetItem(parentpage, itemid);
if (BTreeTupleGetDownLink(itup) != rblkno)
{
    /* L and R have different parents — abort merge */
    _bt_relbuf(rel, parentbuf);
    _bt_relbuf(rel, rbuf);
    _bt_relbuf(rel, lbuf);
    _bt_freestack(stack);
    return false;
}
```

### Deadlock Safety

The search stack is built BEFORE locking any leaf page.  The stack
provides an approximate parent pointer.  `_bt_lock_subtree_parent()` /
`_bt_getstackbuf()` (nbtinsert.c:2335) walk right on the parent level if
the parent has split since the stack was built — same recovery as
`_bt_pagedel()`.

No deadlock because:
- Left → Right at leaf level ✅
- Leaf → Parent (bottom-to-top) ✅
- We never hold a parent lock while trying to acquire a leaf lock ✅

---

## 8. High Key & Pivot Tuple Invariants

Kirk's concern: *"I don't know how you will satisfy the internal pages
(pivot) invariant when you page small pages to large pages to not break
the high keys and the traversing of the B-tree."*

### High Key Rule

The rebuilt destination page (R) keeps its own high key, written at
offset `P_HIKEY` via `PageAddItem()` during the page rebuild.

**Why R's high key and not L's?**

R's high key is the upper bound for R's key range.  After the merge,
R covers both L's and R's former ranges.  L's entire key range is below
R's high key (sibling ordering invariant), so R's high key is the correct
upper bound for the combined range.

L's high key was the separator between L and R in the parent.  It becomes
the pivot entry that gets deleted from the parent — it no longer serves
any purpose.

```
 L.high_key = 20    L has keys: [5, 10, 15]
 R.high_key = 40    R has keys: [25, 30]

 Rebuilt R: high_key = 40, keys = [5, 10, 15, 25, 30]  (all ≤ 40 ✓)
```

**Why don't we need to change any page's high key?**  R keeps its own
high key.  L is deleted (no high key needed).  The parent's high key is
unchanged (the parent's key range doesn't change).  This is the same
property page deletion has (README line 253-254): "Neither the left nor
right sibling pages need to change their 'high key' if any; so there is
no problem with possibly not having enough space to replace a high key."

### Parent Pivot Update

The parent update uses the same two primitives as `_bt_mark_page_halfdead()`:

```
 BTreeTupleSetDownLink(parent[l_poffset], rblkno)   — redirect L's slot to R
 PageIndexTupleDelete(parent, r_poffset)            — remove R's pivot
 Parent: [(-∞)→L] [(20)→R] [(40)→S]  →  [(-∞)→R] [(40)→S]
```

**Why redirect L's downlink to R, not just delete L's downlink?**  Because
L's slot in the parent might be the first data item (minus-infinity downlink).
Internal pages always have at least one downlink, and the first item is
special — it represents "all keys below the next pivot."  We can't delete
it; we redirect it.  Then we delete the next entry (R's old pivot), which
is now redundant because R absorbed L's key space.

**Why is this identical to page deletion?**  Because it IS page deletion.
The parent update for merge is exactly the same operation as the parent
update for deleting an empty page.  The only difference is that our source
page had data (which we moved to R) while `_bt_pagedel()`'s source was empty.

---

## 9. Source Page Deletion — Why We Must Delete Immediately

### The Fatal Flaw of Leaving the Source Page Alive

```
 After merge, source page L is empty but alive:
   Parent still has L's downlink.  L still "owns" its key range.

 Next INSERT(key in L's range):
   1. _bt_search(): follows L's downlink → descends to L
   2. _bt_moveright(): key ≤ L.high_key → stays on L
   3. _bt_findinsertloc(): L has free space → INSERT ON L
   4. L is no longer empty.

 Next VACUUM:
   5. btvacuumpage(L): page has data → NOT empty → skip deletion
   Result: merge accomplished NOTHING. ❌
```

**Why is this inevitable?**  `_bt_findinsertloc()` (nbtinsert.c:886) always
inserts on the first page it finds that (a) has the right key range and
(b) has free space.  An empty page has maximum free space.  It's the
most attractive target for inserts.

### The Solution: Delete Source as Part of the Merge

By removing the source page's parent downlink and marking it
`BTP_HALF_DEAD` atomically with the data movement:

1. **No insert can reach the source** — `_bt_search()` /
   `_bt_findinsertloc()` can never descend to it because there is no
   parent downlink.
2. **Scans with stale references** see `P_IGNORE()` and move right
   via `_bt_moveright()` (nbtsearch.c:306) or `_bt_readnextpage()`
   (nbtsearch.c:1917).
3. **Tombstone** stays with intact sibling links until `safexid` ages
   out (the standard drain technique, README line 392).
4. **VACUUM recycles** the page when `BTPageIsRecyclable()` (nbtree.h)
   returns true.

### Timeline

```
 T0: Merge Phase 1 — move data, update parent, mark source half-dead
     (XLOG_BTREE_MERGE)
 T1: Merge Phase 2 — _bt_unlink_halfdead_page() unlinks source from
     sibling chain, marks BTP_DELETED with safexid
     (XLOG_BTREE_UNLINK_PAGE)
 T2: Source is a tombstone.  Sibling links still point through it.
     Concurrent scans with stale references follow btpo_next past it.
 T3: safexid ages out — all scans that could reference source are gone.
 T4: Next VACUUM encounters source, BTPageIsRecyclable() → true
     → RecordFreeIndexPage() → source recycled into FSM.
```

---

## 10. WAL Logging

> **Full WAL explanation and code walkthrough** — see companion document
> `btree_wal_explainer.md`.  This section covers only the merge-specific
> WAL design decisions.

The merge atomically modifies three pages (destination, source, parent)
in a single critical section, so we need **one WAL record** for the
merge.  The subsequent sibling unlink uses the existing
`XLOG_BTREE_UNLINK_PAGE` record generated by
`_bt_unlink_halfdead_page()`.

### WAL Record Definition

```c
#define XLOG_BTREE_MERGE    0xF0

typedef struct xl_btree_merge
{
    OffsetNumber poffset;       /* offset of L's downlink in parent */
    uint16       ntuples;       /* total tuples on rebuilt destination page */
} xl_btree_merge;

#define SizeOfBtreeMerge    (offsetof(xl_btree_merge, ntuples) + sizeof(uint16))
```

**Why so few fields?**  Block numbers (source, destination, parent) are
stored automatically by the WAL block reference mechanism — we register
them via `XLogRegisterBuffer()`.  We only need parameters that the replay
function can't derive: which offset in the parent has L's downlink
(`poffset`), and how many tuples to unpack from the destination's block
data (`ntuples`).

### Block References

| Block ID | Page | Flags | Why this flag |
|----------|------|-------|---------------|
| 0 | Destination | `REGBUF_WILL_INIT` | Page is completely rebuilt — replay doesn't need old content, just the packed tuples from WAL data |
| 1 | Source | `REGBUF_WILL_INIT` | Page is completely rebuilt as a half-dead tombstone |
| 2 | Parent | `REGBUF_STANDARD` | Incremental change (redirect one downlink, delete one pivot) — replay applies delta to existing page |

### Two-Record Pattern (same as `_bt_pagedel()`)

```
 Record 1 (new):      XLOG_BTREE_MERGE
   Data movement + parent pivot update + source marked half-dead.
   3 blocks: destination, source, parent.

 Record 2 (existing): XLOG_BTREE_UNLINK_PAGE
   Sibling surgery: left_sib.btpo_next = right_sib, etc.
   Generated by _bt_unlink_halfdead_page() — zero new code.
```

**Why not combine both records into one?**  Because
`_bt_unlink_halfdead_page()` already generates `XLOG_BTREE_UNLINK_PAGE`
and we want to call it directly without modifications.  Two records also
mirrors the existing two-phase delete pattern, which the recovery code
already handles.  A crash between them is safe — see Section 11.

---

## 11. Crash Recovery (REDO)

### Replay Function

The replay function `btree_xlog_merge()` reconstructs all three pages:

1. **Destination (block 0):** `XLogInitBufferForRedo()` → `_bt_pageinit()` →
   `_bt_restore_page()` unpacks tuples from WAL data.  Same pattern as
   `btree_xlog_split()` (nbtxlog.c:275).

2. **Source (block 1):** `XLogInitBufferForRedo()` → `_bt_pageinit()` →
   set `BTP_HALF_DEAD | BTP_LEAF` flags → restore truncated top-parent link.
   Same pattern as `btree_xlog_mark_page_halfdead()` (nbtxlog.c:760).

3. **Parent (block 2):** `XLogReadBufferForRedo()` → if `BLK_NEEDS_REDO`:
   redirect downlink via `BTreeTupleSetDownLink()`, delete pivot via
   `PageIndexTupleDelete()`.  Same pivot surgery as
   `btree_xlog_mark_page_halfdead()`.

**Why use `_bt_restore_page()` for the destination?**  It's the existing
function that unpacks a flat array of `MAXALIGN`'d `IndexTuple`s into a
page.  Already used by `btree_xlog_split()`.  Zero new code.

### Crash Scenarios

| Scenario | State After Recovery | Why It's Safe |
|----------|---------------------|---------------|
| Crash BEFORE `XLOG_BTREE_MERGE` flush | All pages unchanged — merge never happened | WAL is all-or-nothing per record |
| Crash AFTER `XLOG_BTREE_MERGE`, BEFORE `XLOG_BTREE_UNLINK_PAGE` | Destination has data, source is half-dead, parent updated.  Source still in sibling chain. | Next VACUUM finds source half-dead → calls `_bt_unlink_halfdead_page()` to finish.  **Same recovery as crash during `_bt_pagedel()`.** |
| Crash AFTER both records flush | Both replay.  Source fully deleted with safexid.  Clean state. | Complete operation replayed |

---

## 12. Concurrency: How Concurrent Scans See the Merge

### 12.1 Why This Is Harder Than Page Deletion

The nbtree README (line 235) acknowledges:

> "Merging partly-full pages would allow better space reuse, but it seems
> impractical to move existing data items left or right to make this happen
> — a scan moving in the opposite direction might miss the items if so."

Page deletion only deletes **empty** pages — there is no data to reprocess
or miss.  Our merge deletes a page with **live data** that has been moved.

### 12.2 Forward Scan Reprocessing Problem

```
 Timeline (L→R merge, forward scan):

 T1: Scanner reads L → BTScanPosData has [5, 10]
 T2: Scanner releases lock on L (_bt_drop_lock_and_maybe_pin)
 T3: Scanner returns 5, 10 to executor

 T4: Merge acquires cleanup locks on L, R
 T5: Merge moves [5, 10] to R → R now has [5, 10, 25, 30]
 T6: Merge releases all locks

 T7: Scanner steps to R (_bt_steppage → _bt_readnextpage)
 T8: Scanner reads R → gets [5, 10, 25, 30]
 T9: Scanner returns 5 AGAIN → REPROCESSED ❌
```

**Why splits don't have this problem:** during a split, data moves RIGHT
into a NEW page.  The scanner on the original page sees post-split content
(via its lock).  When it follows the right-link, it finds only HIGHER keys.

### 12.3 Backward Scan Missing-Data Problem

```
 Timeline (L→R merge, backward scan):

 T1: Scanner reads R → BTScanPosData has [30, 25]
 T2: Scanner releases lock on R

 T3: Merge moves [5, 10] from L to R → R now has [5, 10, 25, 30]
     L is now half-dead.

 T4: Scanner steps left to L
 T5: L is P_IGNORE → scanner skips to L's left sibling
 T6: Scanner never sees [5, 10] → MISSED ❌
```

### 12.4 Solution: Cleanup Lock on Both Source and Destination

The key insight: `LockBufferForCleanup()` (bufmgr.c:6701) blocks until
**no other backend has a pin on the buffer**.  By acquiring cleanup locks
on both the source and destination pages:

```
 T1: Merge acquires cleanup lock on L — waits for all L pins to drain
 T2: Merge acquires cleanup lock on R — waits for all R pins to drain
     At this point: no scan is reading L, no scan is reading R,
     and no scan is transitioning between them (because transitioning
     requires holding a pin on at least one of them)
 T3: Merge executes safely
```

**The pin gap:**  Can a scanner be between L and R holding NO pin on
either?  In `_bt_steppage()` (nbtsearch.c:1705):

```c
BTScanPosUnpinIfPinned(so->currPos)   /* step 1: release pin on L */
/* ... */
so->currPos.buf = _bt_getbuf(rel, blkno, BT_READ)  /* step 2: pin+lock R */
```

Between steps 1 and 2, the scanner holds zero pins.  This is an extremely
narrow window (a few CPU instructions).  `LockBufferForCleanup()` on R
could theoretically succeed during this gap.

**Why is this acceptable?**  This gap is the same class of risk tolerated
elsewhere in PostgreSQL — for example, `_bt_killitems()` (nbtutils.c:269)
uses an LSN comparison to handle the race where a page changed between
reads.  The gap is nanoseconds wide under normal conditions.  The cleanup
lock guarantees that any scanner that has meaningful state (pinned buffer,
read tuples) has completed before we proceed.

### 12.5 After the Merge: Scans That Arrive Later

| Scenario | What Happens | Why It's Safe |
|----------|-------------|---------------|
| Forward scan arrives at L after merge | L is `P_IGNORE`, `_bt_moveright()` follows `btpo_next` to R | R has all the data.  Scanner sees everything. |
| Backward scan arrives at L after merge | L is `P_IGNORE`, `_bt_readnextpage()` follows `btpo_prev` to L's left sibling | No data on L anymore.  Scanner correctly skips. |
| `_bt_search()` descends to L via stale parent | L is `P_IGNORE`, `_bt_moveright()` moves RIGHT to R | Data moved RIGHT (L→R), so moving right finds it.  **This is exactly why L→R is the only safe direction.** |
| Insert into L's old key range | Parent directs insert to R (L's downlink gone) | Correct — R owns L's former key range. |

---

## 13. The `FIX BLOAT` Command

The merge runs as a **user-invoked utility command** called `FIX BLOAT`,
operating on indexes the same way `REINDEX` does.  It is completely
independent of VACUUM.

```sql
FIX BLOAT INDEX index_name;
FIX BLOAT TABLE table_name;   -- all indexes on the table
```

**Why not integrate with VACUUM?**  Several reasons:
1. VACUUM has a well-defined contract: remove dead tuples, freeze old tuples,
   delete empty pages.  Merging live data is a different concern.
2. VACUUM runs automatically (autovacuum).  Merge should be under DBA control
   — the DBA knows when the workload allows the I/O overhead.
3. VACUUM holds a `ShareUpdateExclusiveLock` on the table.  Our merge only
   needs buffer-level locks.  Keeping them separate avoids lock contention.
4. If merge causes issues, the DBA can stop using the command without
   affecting VACUUM's operation.

### Scan Strategy

`FIX BLOAT` scans leaf pages left-to-right.  For each page below the
merge threshold:

1. Check if the right same-parent sibling can absorb this page's data.
2. Verify the data fits and the result stays below the high threshold.
3. Execute the merge (source = this page, destination = right sibling).

**Why left-to-right?**  Three reasons:
1. Matches the merge direction (source = left, destination = right).
2. After a successful merge, the source is deleted and the scan advances
   past it — no risk of re-examining a deleted page.
3. Matches VACUUM's scan direction (`btvacuumscan()`), so the pattern is
   familiar to PostgreSQL developers.

```
 _bt_merge_scan(rel, heaprel):
   for each leaf page P (left to right):
     if _bt_is_merge_candidate(P):
       R = P's right sibling
       if R exists and same parent and _bt_can_merge_into(P, R):
         _bt_try_merge(rel, heaprel, src=P, dst=R)
```

**What about chain merges?**  If three adjacent pages A, B, C are all
below threshold:
- Scan reaches A: merge A into B → B now has A's + B's data.
- Scan reaches B (with merged data): B might still be below threshold
  (if A and B were both very sparse).  Check if B can merge into C.
  If yes → merge B into C.
- Result: C has all three pages' data, A and B are deleted.

This happens naturally from the left-to-right scan without any special
logic.

---

## 14. Edge Cases & Restrictions

### Pages That Cannot Be Merged (Source)

| Condition | Why | Checked By |
|-----------|-----|------------|
| Page is rightmost on level | No right sibling; never deleted by `_bt_pagedel()` either | `P_RIGHTMOST(opaque)` |
| Page is root | Root is never deleted | `P_ISROOT(opaque)` |
| Page has `BTP_INCOMPLETE_SPLIT` | Must finish split first — merging a half-split page would corrupt the tree | `P_INCOMPLETE_SPLIT(opaque)` |
| Page has `BTP_HALF_DEAD` | Already being deleted by another operation | `P_ISHALFDEAD(opaque)` |
| Page has `BTP_DELETED` | Already deleted | `P_ISDELETED(opaque)` |
| Page is internal (non-leaf) | Moving downlinks would change children's parent references | `!P_ISLEAF(opaque)` |
| Left sibling has `INCOMPLETE_SPLIT` | Same check `_bt_pagedel()` does — left sibling might need to absorb the split | `_bt_leftsib_splitflag()` |
| Right sibling is half-dead | A half-dead right sibling has no parent downlink — parent surgery would be confused | `_bt_rightsib_halfdeadflag()` |
| Source and destination have different parents | Cross-parent key space movement would require cascading pivot updates (Section 3) | Same-parent cross-check (Section 7) |
| Destination can't absorb source's data | Combined fill > high threshold, or data physically doesn't fit | `_bt_can_merge_into()` |
| Source is rightmost child of parent | Deleting it would require deleting the parent too — unless it's the ONLY child, in which case `_bt_lock_subtree_parent()` handles it recursively | `_bt_lock_subtree_parent()` returns false |

### Posting List Tuples (Deduplication)

Posting list tuples (deduplicated entries with multiple TIDs for the same
key) are moved as-is — they are standard `IndexTuple`s with a larger
payload.  No special handling needed.

**Why not re-deduplicate during merge?**  Because deduplication has its own
code path (`_bt_dedup_pass()`) with its own logic for when to deduplicate.
Combining merge and dedup in one operation adds complexity without clear
benefit.  If dedup is needed, it happens on the next insert.

### LP_DEAD Items

If the source or destination page has items marked LP_DEAD (kill hints
from scans), they should be **skipped** during the merge.

**Why skip them instead of copying?**  LP_DEAD items are dead tuples
that haven't been physically removed yet.  Copying them to the destination
would transfer garbage and waste space.  Skipping them is a free mini-VACUUM.

### Source Is the Only Child of Its Parent

If the source is the sole child, deleting its downlink leaves the parent
empty.  `_bt_lock_subtree_parent()` (nbtpage.c:2817) handles this
recursively by checking the grandparent.  The "skinny subtree" deletion
(README line 285) deletes the entire chain of single-child internal pages.

**Why does this work for merge?**  Because our parent update is identical
to `_bt_mark_page_halfdead()`'s parent update.  We call the same
`_bt_lock_subtree_parent()` function, which handles the recursion.

---

## 15. Reusable Code Inventory

A central goal is to **maximize reuse** of existing nbtree infrastructure.
Every function listed here is **called directly, as-is, with zero
modifications**.  We do not copy/paste code, fork functions, or change
any existing function's signature or behavior.

**Why maximize reuse?**  Four reasons:
1. The existing code has been tested for 25+ years.  Any bug in our merge
   is more likely in NEW code than in reused code.
2. If the existing code is updated (e.g., a concurrency fix), our merge
   benefits automatically.
3. It makes the patch smaller, which makes community review faster.
4. **It proves our feature is a natural extension** — if the existing API
   already supports every primitive we need, the merge operation is
   clearly within the design scope of the B-tree subsystem.

### New vs. Reused

| Component | New Code? | Reused From | Why |
|-----------|-----------|-------------|-----|
| Tuple iteration loop | No | `_bt_split()` pattern | Same "iterate offsets, add tuples" pattern |
| Adding tuples to page | No | `_bt_pgaddtup()` | Handles alignment, posting lists, etc. |
| Page rebuild in temp buffer | No | `PGAlignedBlock` pattern | Stack-allocated page — no palloc needed |
| WAL replay page rebuild | No | `_bt_restore_page()` | Unpacks flat tuple array into page |
| Parent pivot update | No | `BTreeTupleSetDownLink()` + `PageIndexTupleDelete()` | Identical operation to page deletion |
| Parent finding/locking | No | `_bt_lock_subtree_parent()` | Handles concurrent parent splits, rightmost-child recursion |
| Mark page half-dead | No | `_bt_mark_page_halfdead()` logic | Same flags, same top-parent link |
| Sibling unlink | No | `_bt_unlink_halfdead_page()` — called directly | Complete Phase 2 — zero modifications |
| Predicate lock transfer | No | `PredicateLockPageCombine()` | Same SSI semantics |
| SafeXID stamp | No | `ReadNextFullTransactionId()` via unlink | Standard drain technique |
| Page recycling | No | `BTPageIsRecyclable()` — VACUUM's existing path | VACUUM already does this |
| Search stack | No | `_bt_search()` | Same descent from root to leaf |
| Left sibling split check | No | `_bt_leftsib_splitflag()` | Same safety check |
| Right sibling half-dead check | No | `_bt_rightsib_halfdeadflag()` | Same safety check |
| High key selection | No | `PageAddItem()` with R's high key | R is always the destination |
| **`_bt_try_merge()`** | **Yes** | — | Orchestrator function |
| **`_bt_merge_pages()`** | **Yes** | — | Data movement + parent update in critical section |
| **`XLOG_BTREE_MERGE` record** | **Yes** | — | New WAL record type |
| **`btree_xlog_merge()` replay** | **Yes** | — | Calls existing `_bt_restore_page()` |
| **GUC definitions** | **Yes** | — | Standard `guc_tables.c` entries |
| **`_bt_is_merge_candidate()`** | **Yes** | — | Fill-percentage check |
| **`_bt_can_merge_into()`** | **Yes** | — | Fit + threshold check |
| **`FIX BLOAT` command infrastructure** | **Yes** | — | Scan loop + merge invocation |

---

## 16. Testing Strategy

> Full details, code samples, and benchmark scripts are in
> `btree_page_merge_testing.md`.  This section is the high-level overview.

Two pillars:

### Pillar 1: amcheck for Index Integrity Verification

Every test — regression, isolation, crash recovery, benchmark — ends
with an amcheck call.  `bt_index_parent_check()` (contrib/amcheck/
verify_nbtree.c) verifies exactly the invariants our merge must maintain:

| amcheck Invariant | What It Catches After a Merge |
|-------------------|-------------------------------|
| Logical sort order (`bt_target_page_check`) | Tuples from L inserted in wrong order on R |
| High key bound (`bt_target_page_check`) | R's high key corrupted during page rebuild |
| Downlink lower bound (`bt_child_check`) | Wrong downlink redirected in parent |
| Child high key = parent pivot (`bt_child_highkey_check`) | Wrong pivot deleted in parent |
| No missing downlinks (`bt_downlink_missing_check`) | R's downlink accidentally removed |

Three levels of checking, used in different contexts:

```sql
-- Level 1 (fast, every test):  leaf-level structural checks
SELECT bt_index_check('idx');

-- Level 2 (mandatory, every merge test):  parent-child cross-checks
SELECT bt_index_parent_check('idx');

-- Level 3 (primary regression + crash recovery):  heap cross-check
SELECT bt_index_parent_check('idx', heapallindexed => true,
                             rootdescend => true);
```

Level 3 proves no tuples were lost — every visible heap tuple has a
matching index entry, and every index entry can be re-found by searching
from the root.

### Pillar 2: Performance Benchmarks and Bloat Reduction Measurements

Every benchmark measures BEFORE and AFTER using `pgstatindex()` and
`pg_relation_size()`:

| Metric | Tool | Expected After Merge |
|--------|------|---------------------|
| Leaf pages | `pgstatindex().leaf_pages` | **Fewer** |
| Average leaf density | `pgstatindex().avg_leaf_density` | **Higher** |
| Index size on disk | `pg_relation_size()` | **Smaller** (after VACUUM recycles) |
| Range scan buffers | `EXPLAIN (ANALYZE, BUFFERS)` | **Fewer** |
| Point lookup latency | `EXPLAIN (ANALYZE)` | Same or better |
| Concurrent query TPS | `pgbench` running during merge | ≥ 80% of baseline |

Bloat scenarios tested (1M-row table, varying delete %):

| Scenario | Delete % | Expected Size Reduction |
|----------|----------|------------------------|
| Severe uniform | 90% | 85–90% |
| Moderate uniform | 50% | 40–50% |
| Range delete | 90% of key range | 80%+ |
| Light | 20% | 15–20% |
| No bloat | 0% | 0% (no-op) |

Comparison with `REINDEX` on the same bloated index proves `FIX BLOAT`
achieves comparable reduction without exclusive locks or 2× disk space.

### Test Types

| Type | Location | What |
|------|----------|------|
| **Regression** | `src/test/regress/sql/btree_merge.sql` | Basic merge + amcheck + data verify + bloat measurement |
| **Isolation** | `src/test/isolation/specs/btree-merge.spec` | Forward/backward scans, inserts, VACUUM concurrent with merge |
| **Crash recovery** | `src/test/recovery/t/NNN_btree_merge.pl` | Crash after merge, crash between Phase 1/2, PITR to half-dead state |
| **Benchmarks** | Manual + scripted | Bloat reduction %, query performance, lock duration, REINDEX comparison |

---

## 17. Files Touched (Additions Only)

All changes are **new code added** to existing files.  No existing
function, struct, or line of code is modified — we append new functions,
new struct definitions, new `#define`s, and new `case` branches.

### Core Implementation

| File | What We Add (existing code untouched) |
|------|---------|
| `src/backend/access/nbtree/nbtpage.c` | Add `_bt_try_merge()` and `_bt_merge_pages()` |
| `src/backend/access/nbtree/nbtxlog.c` | Add `btree_xlog_merge()` replay + dispatch case in `btree_redo()` |
| `src/include/access/nbtxlog.h` | Add `XLOG_BTREE_MERGE`, `xl_btree_merge`, `SizeOfBtreeMerge` |
| `src/include/access/nbtree.h` | Add function prototypes and GUC extern declarations |
| `src/backend/access/nbtree/nbtree.c` | Add entry point for the new merge command |
| `src/backend/utils/misc/guc_tables.c` | Add `btree_merge_low_threshold` and `btree_merge_high_threshold` |
| `src/backend/access/rmgrdesc/nbtdesc.c` | Add description for `XLOG_BTREE_MERGE` in `btree_desc()` and `btree_identify()` |

### Testing

| File | Changes |
|------|---------|
| `src/test/regress/sql/btree_merge.sql` | Regression tests |
| `src/test/regress/expected/btree_merge.out` | Expected output |
| `src/test/isolation/specs/btree-merge.spec` | Concurrency tests |
| `src/test/recovery/t/NNN_btree_merge.pl` | Crash recovery TAP tests |

---

## 18. Implementation Timeline (Week-by-Week)

The work is distributed so that each week has concrete deliverables —
implementation, testing, and optimization overlap rather than being
isolated into sequential blocks.

### Milestone 1 (Weeks 1–3): Core Merge Implementation (No WAL)
- Implement `_bt_merge_pages()` in `nbtpage.c`:
  - Build merged page in temp buffer (`PGAlignedBlock`)
  - Copy tuples via `_bt_pgaddtup()` (L first, R second)
  - Skip LP_DEAD items during copy
  - Always L→R: high key from R
  - Parent pivot update via `BTreeTupleSetDownLink()` + `PageIndexTupleDelete()`
  - Mark source `BTP_HALF_DEAD`
  - Transfer predicate locks via `PredicateLockPageCombine()`
- Add same-parent check via `_bt_lock_subtree_parent()`
- Manual test: hardcode two page numbers, call merge, verify with `bt_index_parent_check('idx', true)`
- Regression test: create bloat, merge, verify bloat reduction with `pgstatindex`
- **Milestone:** First successful merge, parent update, half-dead marking, predicate lock transfer, amcheck and regression tests pass

### Milestone 2 (Weeks 4–6): WAL Logging and Crash Recovery
- Add `xl_btree_merge` struct + `XLOG_BTREE_MERGE` to `nbtxlog.h`
- Add `XLOG_BTREE_MERGE` case to `btree_desc()` + `btree_identify()`
- Implement WAL logging in `_bt_merge_pages()`
- Implement `btree_xlog_merge()` in `nbtxlog.c`:
  - Replay destination via `_bt_restore_page()`
  - Replay source as half-dead tombstone
  - Replay parent pivot surgery
- Write TAP crash recovery tests: merge → kill -9 → restart → amcheck passes
- Verify replay correctness with regression and crash tests
- **Milestone:** WAL logging and crash recovery work, replay verified, amcheck passes after recovery

### Milestone 3 (Weeks 7–9): Candidate Detection, FIX BLOAT Command, Edge Cases, Validation
- Implement `_bt_is_merge_candidate()` and `_bt_can_merge_into()`
- Add GUC parameters (`btree_merge_low_threshold`, `btree_merge_high_threshold`) in `guc_tables.c`
- Implement `_bt_try_merge()` — top-level merge orchestrator
  - Search stack via `_bt_search()`
  - L/R cleanup locks via `LockBufferForCleanup()`
  - Calls `_bt_merge_pages()` for the critical section
- Implement the `FIX BLOAT INDEX` scan loop (left-to-right leaf scan)
- Handle posting list tuples, INCLUDE columns, NULL values, incomplete splits
- Regression tests for all edge cases
- **Milestone:** Validation logic, GUCs, `FIX BLOAT INDEX` command, edge case handling, all regression tests pass with amcheck

### Milestone 4 (Weeks 10–12): Concurrency Isolation Tests, Benchmarks, Optimization, Documentation, Patch Submission
- Write isolation specs: scan/insert during merge, VACUUM on adjacent pages, concurrent `_bt_pagedel()`
- Extended TAP crash tests: crash between merge phases, PITR, standby promotion
- Benchmark suite: 90%/50%/20% delete scenarios, TPS impact with `pgbench`, REINDEX comparison
- Optimize: skip already-checked pages, batch merges, avoid redundant search stack descents
- Write sgml documentation, code comments, run `pgindent`, prepare patch for pgsql-hackers
- **Milestone:** All concurrency and crash scenarios pass, benchmark data, TPS impact minimal, code optimized, documentation complete, patch ready for pgsql-hackers

---

## Appendix A: Decision Log

Every major design decision with its rationale, including decisions that
may appear suboptimal but are made for good reasons.

| # | Decision | Alternatives Considered | Why We Chose This |
|---|----------|------------------------|-------------------|
| 1 | **Always L→R direction** | Bidirectional (merge in either direction) | `_bt_moveright()` only moves right.  R→L causes stale-downlink readers to move away from data.  L→R is the only direction compatible with PostgreSQL's concurrency protocol.  See Section 3. |
| 2 | **Sometimes merge large L into small R** | Skip merge when L > R; or merge R→L to move fewer tuples | The overhead is negligible (microseconds of extra `memcpy`, a few KB extra WAL).  Not merging means the bloat persists.  The alternative (R→L to move fewer tuples) is unsafe.  Moving 400 tuples in memory is infinitely better than moving 0.  See Section 4. |
| 3 | **Same-parent constraint** | Allow cross-parent merges | Cross-parent requires cascading bounding-key updates up the tree, potentially to the root.  Can't be done atomically without locking the entire tree.  Loses < 1% of merge opportunities (only the rightmost child of each parent).  See Section 3. |
| 4 | **Delete source immediately** | Leave empty for VACUUM to delete | An empty live page gets re-filled by inserts via `_bt_findinsertloc()` before VACUUM runs.  Result: zero bloat reduction.  Immediate deletion (remove downlink + half-dead flag) is the only way to prevent re-fill.  See Section 9. |
| 5 | **`FIX BLOAT` command, not VACUUM** | Integrate into VACUUM's scan | Different concerns (reorganize live data vs. remove dead data).  `FIX BLOAT` gives DBA control over timing and aggressiveness.  Avoids complicating VACUUM's critical code path.  Avoids autovacuum timing issues. |
| 6 | **Cleanup locks on both pages** | Regular BT_WRITE locks | BT_WRITE doesn't drain existing pins.  A scanner transitioning between L and R could see duplicates (forward) or miss data (backward).  Cleanup locks guarantee no scanner is active on either page.  See Section 12. |
| 7 | **Two WAL records** (merge + unlink) | Single combined WAL record | Lets us call `_bt_unlink_halfdead_page()` without modification.  Crash between them is safe (same as existing two-phase delete).  Smaller patch, reuses existing replay code. |
| 8 | **R keeps its own high key** | Compute new high key | R's high key is already the correct upper bound for the combined range.  No high key change means no risk of the new key not fitting.  Same property as existing page deletion (README line 253-254).  See Section 8. |
| 9 | **Leaf pages only** | Merge internal pages too | Internal pages have downlinks — moving them changes children's parent pointers, creating cascading updates.  Leaf bloat is the primary user complaint.  Internal page merge is future work. |
| 10 | **GUC thresholds** | Hardcoded values | Different workloads need different aggressiveness.  Kirk explicitly requested GUCs.  Defaults are conservative (10% low, 70% high).  See Section 5. |
| 11 | **Skip LP_DEAD items during merge** | Copy them to destination | LP_DEAD items are dead tuples.  Copying wastes space.  Skipping is a free mini-VACUUM. |
| 12 | **Left-to-right scan** | Random order, right-to-left | Matches merge direction (L→R).  Deleted source is behind scan cursor.  Enables natural chain merging (A→B, then B→C). |

---

## Appendix B: Comparison — Merge vs. Existing Operations

| Aspect | Page Split (`_bt_split`) | Page Delete (`_bt_pagedel`) | **Page Merge (new)** |
|--------|--------------------------|-----------------------------|-----------------------|
| Direction | → (right half created) | Key space → right | Key space → right (L into R) |
| Pages locked | 2–3 (L, R, parent) | 3+ (left, target, right, parent) | **3 (src, dst, parent)** |
| Sibling links changed | Yes | Yes | **Yes** (via `_bt_unlink_halfdead_page()`) |
| Parent changed | Yes (new downlink added) | Yes (downlink removed) | **Yes** (downlink redirected, pivot removed) |
| High key changed | Yes | No | **No** (R keeps its own high key) |
| WAL records | `XLOG_BTREE_SPLIT_L/R` | `MARK_HALFDEAD` + `UNLINK` | **`XLOG_BTREE_MERGE`** + `UNLINK` |
| Source page outcome | N/A | Immediate delete | **Immediate delete** |
| VACUUM coupling | None | N/A (is VACUUM) | **None** |

---

*This document incorporates feedback from Kirk Wolak (GSoC 2026 mentor)
and analysis of the PostgreSQL nbtree source code.  It will evolve as
implementation progresses.*

*Companion document: `btree_wal_explainer.md` — full explanation of
PostgreSQL WAL mechanics and how to write WAL-logged code.*
