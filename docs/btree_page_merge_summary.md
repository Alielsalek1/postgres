# B-Tree Page Merge — Summary

> **GSoC 2026 — B-tree Index Bloat Reduction (Page Merge)**
>
> This is a condensed overview.  For full design rationale, tradeoff
> analysis, code samples, and concurrency proofs see
> `btree_page_merge_plan.md`.  For WAL internals see
> `btree_wal_explainer.md`.

---

## Design Principle: Pure Extension, Zero Modification

This project is designed as a **pure addition** to PostgreSQL.  It does
not modify, replace, or alter any existing code path:

- **No existing function is changed.**  Every existing B-tree function
  (`_bt_search`, `_bt_moveright`, `_bt_pagedel`, `_bt_split`,
  `_bt_findinsertloc`, VACUUM's index scan, etc.) remains untouched.
- **No existing behavior is altered.**  Inserts, deletes, splits, scans,
  VACUUM, autovacuum, `REINDEX` — all work exactly as before.
- **No existing WAL record is modified.**  We add one new record type;
  all existing record types and their replay functions are unchanged.
- **No existing concurrency protocol is changed.**  We rely on the same
  `P_IGNORE` / `_bt_moveright()` / cleanup-lock / drain mechanisms that
  page deletion already uses.
- **The feature is opt-in.**  Nothing happens unless the DBA explicitly
  runs `FIX BLOAT`.  There is no background worker, no autovacuum
  hook, no automatic trigger.

The entire patch is **new code that calls existing functions**.  If the
patch were reverted, PostgreSQL would behave identically to how it does
today.  This minimizes review burden, regression risk, and community
push-back.

---

## The Problem

VACUUM deletes empty B-tree pages but ignores partially-filled ones.
After heavy DELETE workloads, indexes can be 4–5× larger than necessary
with every leaf page only 10–20% full.  Existing fixes (`REINDEX`,
`pg_repack`) require downtime or 2× disk space.

## The Solution

A new utility command — `FIX BLOAT` — that **merges adjacent leaf pages**
pairwise and **frees the emptied page** — online, with no table-level
locks, no extra disk space.  It operates on indexes the same way `REINDEX`
does:

```sql
FIX BLOAT INDEX index_name;
FIX BLOAT TABLE table_name;   -- all indexes on the table
```

---

## Core Design Decisions

| Decision | Choice |
|----------|--------|
| **Direction** | Always L → R (source = left, destination = right) |
| **Strategy** | Prefer small L into large R; accept large L into small R |
| **Scope** | Leaf pages only |
| **Parent constraint** | Both pages must share the same parent |
| **Source page** | Deleted immediately (half-dead → unlinked) |
| **Trigger** | `FIX BLOAT` command (like `REINDEX`), not VACUUM |
| **Thresholds** | GUC-controlled: `low_threshold` = 10%, `high_threshold` = 70% |
| **Locking** | Cleanup lock on both leaves, `BT_WRITE` on parent |
| **WAL** | New `XLOG_BTREE_MERGE` + existing `XLOG_BTREE_UNLINK_PAGE` |
| **High key** | R keeps its own high key (no change needed) |
| **LP_DEAD items** | Skipped during merge (free mini-VACUUM) |

---

## How It Works

```
 BEFORE:
 ┌─────────────┐    ┌─────────────┐
 │ Page L (8%) │ →  │ Page R (35%)│ → ...
 │ [5, 10, 15] │    │ [25, 30]    │
 └─────────────┘    └─────────────┘
       Parent: [(-∞)→L]  [(20)→R]  [(40)→S]

 AFTER:
                    ┌──────────────────┐
                    │ Page R (43%)     │ → ...
                    │ [5,10,15,25,30]  │
                    └──────────────────┘
       Parent: [(-∞)→R]  [(40)→S]
       L is deleted.
```

### Two Phases

1. **Merge + Mark Half-Dead** (single critical section, one WAL record)
   - Build merged page in temp buffer: L's tuples first, then R's
   - Redirect L's parent downlink → R
   - Delete R's redundant pivot from parent
   - Replace R's content with merged page
   - Mark L as `BTP_HALF_DEAD`

2. **Unlink Source** (calls existing `_bt_unlink_halfdead_page()`)
   - Bypass L in sibling chain
   - Stamp `safexid` for the drain technique
   - VACUUM recycles the page later

Crash between phases is safe — VACUUM finishes the unlink (same as
existing page deletion recovery).

---

## Locking & Concurrency

```
 Lock order:  L (cleanup) → R (cleanup) → Parent (BT_WRITE)
              left-to-right at leaf, bottom-to-top across levels
```

- **Cleanup locks** drain all pins — no scanner can be reading either
  page or transitioning between them during the merge.
- **After merge:** scans arriving at deleted L see `P_IGNORE` and move
  right via `_bt_moveright()` — data is on R.  Safe because key space
  moved right (L→R), matching PostgreSQL's concurrency protocol.

---

## Scan Strategy

```
 for each leaf page P (left to right):
   if fill(P) ≤ low_threshold:
     R = P.btpo_next
     if same_parent(P, R) and fits(P, R, high_threshold):
       merge(P → R)
```

- Naturally handles **chain merges**: A→B, then B→C if still sparse.
- Pages that can't be source (rightmost, different parent) are skipped.

---

## GUC Parameters

| GUC | Default | Purpose |
|-----|---------|---------|
| `btree_merge_low_threshold` | 10% | Source must be below this fill % |
| `btree_merge_high_threshold` | 70% | Result must not exceed this fill % |

---

## What We Reuse vs. Write New

**Called as-is, never modified:**
`_bt_search()`, `_bt_lock_subtree_parent()`, `_bt_unlink_halfdead_page()`,
`_bt_pgaddtup()`, `_bt_restore_page()`, `BTreeTupleSetDownLink()`,
`PageIndexTupleDelete()`, `PredicateLockPageCombine()`,
`_bt_leftsib_splitflag()`, `_bt_rightsib_halfdeadflag()`,
`BTPageIsRecyclable()` (VACUUM path).  **None of these are touched by
our patch — we call them, exactly as they are.**

**New code (100% additive):**
`_bt_try_merge()`, `_bt_merge_pages()`, `_bt_is_merge_candidate()`,
`_bt_can_merge_into()`, `btree_xlog_merge()`, `xl_btree_merge` WAL
struct, GUC definitions, `FIX BLOAT` command entry point

---

## Files Touched (Additions Only)

All changes are **new code added** to existing files — no existing
function or line of code is modified.

| File | What We Add (existing code untouched) |
|------|------|
| `nbtpage.c` | New functions: `_bt_try_merge()`, `_bt_merge_pages()` |
| `nbtxlog.c` | New function: `btree_xlog_merge()` replay + one new `case` in dispatch |
| `nbtxlog.h` | New `#define` + new struct (appended) |
| `nbtree.h` | New prototypes + externs (appended) |
| `nbtree.c` | `FIX BLOAT` command entry point function |
| `guc_tables.c` | New GUC entries (appended to array) |
| `nbtdesc.c` | New `case` in description switch |

---

## Testing

> Full details in `btree_page_merge_testing.md`.

Two pillars:

**1. amcheck for index integrity verification** — every test ends with
`bt_index_parent_check()`.  It verifies sorted order, high key bounds,
parent-child downlink consistency, and no missing downlinks.  The primary
regression test uses Level 3 (`heapallindexed => true, rootdescend => true`)
to prove no tuples were lost and every entry is re-findable from root.

**2. Performance benchmarks and bloat reduction measurements** — every
benchmark measures BEFORE/AFTER with `pgstatindex()` (leaf pages,
avg_leaf_density) and `pg_relation_size()`.  Bloat scenarios from 20%
to 90% delete.  Query performance compared with `EXPLAIN (ANALYZE, BUFFERS)`.
Concurrent TPS measured with `pgbench` during merge.  Compared against
`REINDEX` on the same bloated index.

| Type | What |
|------|------|
| **Regression** | Insert → bulk delete → VACUUM → merge → amcheck (Level 3) → data verify → pgstatindex before/after |
| **Isolation** | Forward + backward scans, inserts, VACUUM concurrent with merge |
| **Crash recovery** | Crash after merge, crash between Phase 1/2, PITR to half-dead state → amcheck |
| **Benchmarks** | Bloat reduction (pgstatindex), query perf (EXPLAIN ANALYZE BUFFERS), lock duration, TPS impact (pgbench), REINDEX comparison |

---

## Implementation Timeline

| Milestone | Focus | Deliverable |
|----------|-------|------------|
| 1 (Week 1) | Planning, requirements, initial design, test plan | Kickoff, requirements, design doc, risk analysis, initial test plan |
| 2 (Weeks 2–5) | Core merge implementation (in-memory), FIX BLOAT command, unit tests | Merge logic, parent update, half-dead marking, predicate lock transfer, GUCs, `FIX BLOAT INDEX` command, amcheck/unit tests pass |
| 3 (Weeks 6–9) | WAL logging, redo, crash recovery, integration tests | Add XLOG_BTREE_MERGE, implement redo, crash recovery TAP tests, verify replay correctness, regression/integration tests, peer review |
| 4 (Weeks 10–12) | Candidate detection, edge cases, concurrency isolation tests, benchmarks, optimization, documentation, final review, patch submission | Validation logic, edge case handling, all concurrency and crash scenarios pass, benchmark data, TPS impact minimal, code optimized, documentation complete, final review, patch ready for pgsql-hackers |
