# B-Tree Page Merge — Testing Plan

> How every component is tested, what tools we use, and what we measure.
>
> Two pillars:
> 1. **amcheck for index integrity verification** — proves correctness
> 2. **Performance benchmarks and bloat reduction measurements** — proves value

---

## Table of Contents

1.  [Testing Philosophy](#1-testing-philosophy)
2.  [amcheck — The Correctness Oracle](#2-amcheck--the-correctness-oracle)
3.  [Component-Level Tests](#3-component-level-tests)
4.  [Regression Tests](#4-regression-tests)
5.  [Isolation Tests (Concurrency)](#5-isolation-tests-concurrency)
6.  [Crash Recovery TAP Tests](#6-crash-recovery-tap-tests)
7.  [Performance Benchmarks](#7-performance-benchmarks)
8.  [Bloat Reduction Measurements](#8-bloat-reduction-measurements)
9.  [GUC Threshold Tests](#9-guc-threshold-tests)
10. [Edge Case Tests](#10-edge-case-tests)
11. [Test Files and Locations](#11-test-files-and-locations)
12. [Test Matrix Summary](#12-test-matrix-summary)

---

## 1. Testing Philosophy

Every test answers one of two questions:

1. **Is the index still correct?** → amcheck answers this.
2. **Did we actually reduce bloat?** → `pgstatindex()` and `pg_relation_size()` answer this.

We never trust "it didn't crash" as evidence of correctness.  A merge
can silently corrupt the tree — wrong high key, missing downlink, broken
sibling chain, orphaned tuples — and queries may still "work" by
coincidence.  Only amcheck catches these structural violations.

Similarly, we never trust "pages were merged" as evidence of value.  The
merge must result in measurably fewer pages, lower index size on disk,
and equivalent or better query performance.  Only benchmarks prove this.

### The Pattern

Every test follows this skeleton:

```
1. Setup: create table, index, insert data
2. Create bloat: DELETE + VACUUM (sparse pages, no empty pages)
3. Measure BEFORE: pgstatindex(), pg_relation_size(), query plan
4. Run FIX BLOAT
5. Verify correctness: bt_index_check(), bt_index_parent_check()
6. Verify data: count(*), range scans forward + backward
7. Measure AFTER: pgstatindex(), pg_relation_size(), query plan
8. Compare BEFORE vs AFTER
```

---

## 2. amcheck — The Correctness Oracle

### What amcheck Is

`amcheck` (contrib/amcheck) is PostgreSQL's built-in B-tree integrity
verifier.  It implements `bt_index_check()` and `bt_index_parent_check()`
in `contrib/amcheck/verify_nbtree.c`.  These functions do a full
structural traversal of the index and report any invariant violation as
an ERROR.

### Why amcheck Is the Right Tool

Our merge modifies three pages in a single critical section:
- **Destination (R):** tuples are added, content is rebuilt
- **Parent:** downlink is redirected, pivot is deleted
- **Source (L):** marked `BTP_HALF_DEAD`, later unlinked

Each of these changes can introduce a structural bug.  amcheck verifies
exactly the invariants we need to maintain:

### The Five Invariants amcheck Checks

#### Invariant 1: Logical Order (`bt_target_page_check`)

Every page's items must be in sorted order according to the index's
comparison function.  After a merge, R's tuples include L's former
tuples prepended before R's existing tuples.  If the merge rebuilt the
page incorrectly (wrong order, duplicate keys, missing tuples), this
check catches it.

```
What it checks: For every consecutive pair (item[i], item[i+1]) on
a page, the comparison function must return item[i] ≤ item[i+1].
```

**Merge-specific failure mode:** Tuples from L are inserted before R's
tuples (lower key space).  If the merge code puts them after R's tuples,
or interleaves them wrong, sorted order breaks.

#### Invariant 2: High Key Bound (`bt_target_page_check`)

Every tuple on a non-rightmost page must be ≤ the page's high key.  Our
merge doesn't change R's high key (R is the destination, its high key
is already the correct upper bound for the combined range).  But if
the code has a bug and R's high key is wrong, L's tuples (which have
lower keys) could exceed the high key of a different page.

```
What it checks: For every tuple T on page P, T ≤ P.high_key.
```

**Merge-specific failure mode:** If we accidentally use L's high key
instead of R's, or if the page rebuild corrupts the high key, this
check catches it.

#### Invariant 3: Parent-Child Consistency (`bt_child_check`)

Every downlink in a parent page must point to a child whose first key
is ≥ the downlink's lower bound.  After a merge, L's downlink is
redirected to point to R, and the redundant pivot (L's former separator)
is deleted.  If the parent surgery is wrong, the downlink-to-child
mapping breaks.

```
What it checks: For parent downlink D pointing to child C,
first_key(C) ≥ lower_bound(D).
```

**Merge-specific failure mode:** If we redirect the wrong downlink, or
delete the wrong pivot, or fail to update `BTreeTupleSetDownLink()`
correctly, this check catches it.

#### Invariant 4: Child High Key = Parent Pivot (`bt_child_highkey_check`)

A child's high key must match the corresponding separator key in its
parent.  Since R keeps its own high key and R's pivot in the parent is
unchanged, this should hold.  But the parent *does* change (L's pivot
is deleted, L's downlink is redirected), so the parent's key structure
could be corrupted.

```
What it checks: For child page C with parent P,
C.high_key = P.pivot_key[next_downlink_after(C)].
```

**Merge-specific failure mode:** If `PageIndexTupleDelete()` removes the
wrong offset in the parent, the pivot-to-highkey correspondence breaks.

#### Invariant 5: No Missing Downlinks (`bt_downlink_missing_check`)

Every non-ignored leaf page must have a parent downlink pointing to it.
After a merge, L is marked `BTP_HALF_DEAD` (and later unlinked), so it
becomes `P_IGNORE` — amcheck correctly skips it.  R must still have a
downlink.  If the parent surgery accidentally removes R's downlink
instead of L's, this check catches it.

```
What it checks: Walk the child level left-to-right.  Every non-P_IGNORE
page must be the target of exactly one parent downlink.
```

**Merge-specific failure mode:** Deleting R's pivot instead of L's pivot
leaves R without a parent downlink → immediate ERROR.

### The Three Levels of amcheck

We use all three in our tests, from cheapest to most expensive:

```sql
-- Level 1: Leaf-level structural checks (fast, AccessShareLock)
SELECT bt_index_check('idx');

-- Level 2: Parent-child cross-checks (slower, ShareLock)
-- This is the critical one for merge correctness.
SELECT bt_index_parent_check('idx');

-- Level 3: Heap-index cross-check (slowest, proves no lost tuples)
-- heapallindexed=true: every visible heap tuple has a matching index entry
-- rootdescend=true: re-search from root for every tuple to verify findability
SELECT bt_index_parent_check('idx', heapallindexed => true,
                             rootdescend => true);
```

**For our merge, Level 2 is mandatory in every test.**  Level 3 is used
in the primary regression test and after crash recovery to prove that
no tuples were lost.

### What amcheck Does NOT Check

amcheck verifies *structural* invariants.  It does not check:
- Performance (did the merge actually reduce bloat?)
- Lock behavior (did we hold cleanup locks correctly?)
- WAL correctness (does replay produce the same state?)
- Concurrency safety (do concurrent scans see correct results?)

These require separate tests (Sections 5–8).

---

## 3. Component-Level Tests

Each component of the merge is tested individually before integration.

### 3a. `_bt_merge_pages()` — The Critical Section

This is the function that rebuilds the destination page, updates the
parent, and marks the source half-dead.

**What to test:**

| Test | How | Verify |
|------|-----|--------|
| Two adjacent pages, L sparse, R has room | Manually pick two adjacent pages, call `_bt_merge_pages()` | `bt_index_parent_check()` passes, all tuples on R, L is half-dead |
| LP_DEAD items on L are skipped | Insert data, mark some tuples dead, merge | Dead tuples don't appear on R; R has fewer tuples than L+R original |
| L has posting list tuples (dedup) | Create index with many duplicates (triggers deduplication), then delete some, merge | `bt_index_check()` passes, posting lists on R are intact |
| Parent has exactly 2 children (L and R) | Create tiny index where a parent has only 2 leaf children | After merge, parent has 1 child, amcheck passes, no empty parent |
| Parent has many children, L is leftmost | L is the first downlink in the parent | After merge, parent's first downlink now points to R, negative infinity item intact |
| Merge fills R to exactly `high_threshold` | Choose L and R sizes that sum to exactly the threshold | Merge succeeds, `bt_index_check()` passes |
| Merge would exceed `high_threshold` | Choose L and R sizes that sum above threshold | Merge is NOT attempted (function returns without merging) |

**How to run:**

```sql
-- Create controlled scenario
CREATE TABLE unit_test (id int, pad text);
CREATE INDEX unit_idx ON unit_test (id);
ALTER TABLE unit_test SET (autovacuum_enabled = false);

-- Fill enough pages to create a known structure
INSERT INTO unit_test SELECT i, repeat('x', 200)
  FROM generate_series(1, 50000) i;

-- Delete specific ranges to create sparse pages at known positions
DELETE FROM unit_test WHERE id BETWEEN 100 AND 450;  -- sparse page at L
VACUUM unit_test;

-- Before: measure the specific pages
SELECT * FROM pgstatindex('unit_idx');

-- Merge
FIX BLOAT INDEX unit_idx;

-- After: structural verification
SELECT bt_index_parent_check('unit_idx', heapallindexed => true);

-- After: data verification
SELECT count(*) FROM unit_test;
SET enable_seqscan = off;
SELECT count(*) FROM unit_test WHERE id BETWEEN 1 AND 500;
```

### 3b. `btree_xlog_merge()` — WAL Replay

The replay function must reconstruct the exact same page state that
the original merge produced.  We test this via crash recovery (Section 6),
but we can also verify replay indirectly:

**Test:** Run merge on primary, check standby via streaming replication.

```sql
-- On primary:
FIX BLOAT INDEX idx;
SELECT bt_index_parent_check('idx', heapallindexed => true);

-- On standby (after WAL replay):
SELECT bt_index_parent_check('idx', heapallindexed => true);

-- Both must pass.  If replay is wrong, the standby's amcheck fails.
```

### 3c. `_bt_try_merge()` — The Orchestrator

This function acquires locks, validates preconditions, and calls
`_bt_merge_pages()`.

**What to test:**

| Test | How | Verify |
|------|-----|--------|
| Lock ordering: L → R → Parent | Instrument with `elog(DEBUG)` or trace | Lock acquisition order matches spec |
| Cleanup lock failure (page pinned) | Start a long-running scan on L, attempt merge | Merge blocks until scan releases pin, OR merge skips the page |
| Same-parent validation rejects cross-parent pair | Manually pick two pages with different parents | Merge is NOT attempted |
| Right sibling is half-dead | Delete R via VACUUM, then attempt merge on L | Merge skips (R is `P_IGNORE`) |
| Left page was split since candidate check | Concurrent insert splits L between candidate check and lock acquisition | Merge aborts and moves on (page content changed) |

### 3d. `_bt_is_merge_candidate()` and `_bt_can_merge_into()`

**What to test:**

| Test | How | Verify |
|------|-----|--------|
| Page at 5% fill → candidate (default threshold 10%) | Check with `low_threshold = 10` | Returns true |
| Page at 15% fill → NOT candidate (default threshold 10%) | Check with `low_threshold = 10` | Returns false |
| Page at 0% fill (empty but not deleted) → candidate | Rare edge case | Returns true, merge proceeds |
| Combined fill = 65% and `high_threshold = 70` → fits | Compute precisely | Returns true |
| Combined fill = 75% and `high_threshold = 70` → doesn't fit | Compute precisely | Returns false |

### 3e. `FIX BLOAT` Scan Loop

The command scans all leaf pages left-to-right and applies merges.

**What to test:**

| Test | How | Verify |
|------|-----|--------|
| Full scan with no merge candidates | All pages > threshold | Command completes, 0 merges, amcheck passes |
| Full scan with many candidates | Delete 90% of data | Multiple merges, amcheck passes, size reduced |
| Chain merge: A→B, then B→C | Three adjacent sparse pages | After command, two pages deleted, one has all data |
| Rightmost page is sparse but skipped | Delete data from rightmost page | No merge (no right sibling), amcheck passes |
| Index has only 1 leaf page | Very small table | No merge possible, command completes cleanly |
| Index has 2 leaf pages, both sparse | Both below threshold | One merge, amcheck passes |

---

## 4. Regression Tests

Location: `src/test/regress/sql/btree_merge.sql`

These are the SQL-level tests that run as part of `make check`.

### Test 1: Basic Merge + amcheck

```sql
-- Setup
CREATE EXTENSION IF NOT EXISTS amcheck;
CREATE EXTENSION IF NOT EXISTS pgstattuple;

CREATE TABLE merge_basic (id int, val text);
CREATE INDEX merge_basic_idx ON merge_basic (id);
ALTER TABLE merge_basic SET (autovacuum_enabled = false);

-- Fill many pages
INSERT INTO merge_basic SELECT i, repeat('x', 100)
  FROM generate_series(1, 100000) i;

-- Create severe bloat (95% delete)
DELETE FROM merge_basic WHERE id % 20 != 0;
VACUUM merge_basic;

-- Record before state
SELECT leaf_pages AS before_leaf_pages,
       avg_leaf_density AS before_density
  FROM pgstatindex('merge_basic_idx');

SELECT pg_relation_size('merge_basic_idx') AS before_size;

-- Merge with aggressive thresholds
SET btree_merge_low_threshold = 50;
SET btree_merge_high_threshold = 90;
FIX BLOAT INDEX merge_basic_idx;

-- CORRECTNESS: amcheck (Level 2 + heapallindexed)
SELECT bt_index_parent_check('merge_basic_idx', heapallindexed => true);

-- CORRECTNESS: all data accessible
SELECT count(*) FROM merge_basic;  -- must be 5000

-- CORRECTNESS: forward scan
SET enable_seqscan = off;
SELECT count(*) FROM merge_basic WHERE id BETWEEN 1000 AND 5000;

-- CORRECTNESS: backward scan
SELECT count(*) FROM merge_basic WHERE id BETWEEN 1000 AND 5000
  ORDER BY id DESC;

-- CORRECTNESS: index-only scan
VACUUM merge_basic;  -- update visibility map
EXPLAIN (COSTS OFF) SELECT id FROM merge_basic WHERE id = 2000;
SELECT id FROM merge_basic WHERE id = 2000;

-- BLOAT REDUCTION: fewer pages, higher density
SELECT leaf_pages AS after_leaf_pages,
       avg_leaf_density AS after_density
  FROM pgstatindex('merge_basic_idx');

SELECT pg_relation_size('merge_basic_idx') AS after_size;

-- Cleanup
RESET btree_merge_low_threshold;
RESET btree_merge_high_threshold;
RESET enable_seqscan;
DROP TABLE merge_basic;
```

### Test 2: No Bloat — FIX BLOAT Is a No-Op

```sql
CREATE TABLE merge_noop (id int);
CREATE INDEX merge_noop_idx ON merge_noop (id);
INSERT INTO merge_noop SELECT generate_series(1, 10000);

SELECT pg_relation_size('merge_noop_idx') AS before_size;
FIX BLOAT INDEX merge_noop_idx;
SELECT pg_relation_size('merge_noop_idx') AS after_size;

-- Size should be identical (no sparse pages to merge)
SELECT bt_index_parent_check('merge_noop_idx');

DROP TABLE merge_noop;
```

### Test 3: Data Type Coverage

```sql
-- Text keys
CREATE TABLE merge_text (val text);
CREATE INDEX merge_text_idx ON merge_text (val);
INSERT INTO merge_text SELECT md5(i::text) FROM generate_series(1, 50000) i;
DELETE FROM merge_text WHERE val < '3';
VACUUM merge_text;
FIX BLOAT INDEX merge_text_idx;
SELECT bt_index_parent_check('merge_text_idx', heapallindexed => true);
DROP TABLE merge_text;

-- Composite keys
CREATE TABLE merge_composite (a int, b text, c timestamptz);
CREATE INDEX merge_comp_idx ON merge_composite (a, b, c);
INSERT INTO merge_composite
  SELECT i, 'val' || i, now() - (i || ' seconds')::interval
  FROM generate_series(1, 50000) i;
DELETE FROM merge_composite WHERE a % 10 != 0;
VACUUM merge_composite;
FIX BLOAT INDEX merge_comp_idx;
SELECT bt_index_parent_check('merge_comp_idx', heapallindexed => true);
DROP TABLE merge_composite;

-- INCLUDE columns (covering index)
CREATE TABLE merge_include (id int, data text);
CREATE INDEX merge_incl_idx ON merge_include (id) INCLUDE (data);
INSERT INTO merge_include SELECT i, repeat('x', 50) FROM generate_series(1, 50000) i;
DELETE FROM merge_include WHERE id % 15 != 0;
VACUUM merge_include;
FIX BLOAT INDEX merge_incl_idx;
SELECT bt_index_parent_check('merge_incl_idx', heapallindexed => true);
DROP TABLE merge_include;
```

### Test 4: Unique Index

```sql
CREATE TABLE merge_unique (id int PRIMARY KEY);
INSERT INTO merge_unique SELECT generate_series(1, 100000);
DELETE FROM merge_unique WHERE id % 20 != 0;
VACUUM merge_unique;
FIX BLOAT INDEX merge_unique_pkey;
-- Must not create duplicate keys
SELECT bt_index_parent_check('merge_unique_pkey',
                             heapallindexed => true,
                             checkunique => true);
DROP TABLE merge_unique;
```

### Test 5: Merge After Concurrent Inserts

```sql
CREATE TABLE merge_insert (id int);
CREATE INDEX merge_insert_idx ON merge_insert (id);
INSERT INTO merge_insert SELECT generate_series(1, 100000);
DELETE FROM merge_insert WHERE id % 20 != 0;
VACUUM merge_insert;

-- Insert new rows into the sparse range BEFORE merge
INSERT INTO merge_insert SELECT generate_series(1, 500);

FIX BLOAT INDEX merge_insert_idx;
SELECT bt_index_parent_check('merge_insert_idx', heapallindexed => true);

-- All rows must be findable
SET enable_seqscan = off;
SELECT count(*) FROM merge_insert;
DROP TABLE merge_insert;
```

---

## 5. Isolation Tests (Concurrency)

Location: `src/test/isolation/specs/btree-merge.spec`

These use PostgreSQL's isolation test framework to interleave merge
operations with concurrent scans and DML.

### Test 5a: Forward Scan During Merge

```
setup {
  CREATE EXTENSION IF NOT EXISTS amcheck;
  CREATE TABLE iso_merge (id int);
  CREATE INDEX iso_merge_idx ON iso_merge (id);
  ALTER TABLE iso_merge SET (autovacuum_enabled = false);
  INSERT INTO iso_merge SELECT generate_series(1, 10000);
  DELETE FROM iso_merge WHERE id % 10 != 0;
  VACUUM iso_merge;
}

teardown {
  DROP TABLE iso_merge;
}

session scanner
setup           { BEGIN; SET enable_seqscan = off; }
step scan1      { SELECT count(*) FROM iso_merge WHERE id BETWEEN 100 AND 500; }
step scan2      { SELECT count(*) FROM iso_merge WHERE id BETWEEN 100 AND 500; }
step end        { COMMIT; }

session merger
step merge      { SET btree_merge_low_threshold = 50;
                  SET btree_merge_high_threshold = 90;
                  FIX BLOAT INDEX iso_merge_idx; }
step verify     { SELECT bt_index_parent_check('iso_merge_idx'); }

# scan1 and scan2 must return the same count
permutation scan1 merge scan2 verify end
```

**What this proves:** A forward scan that starts before the merge and
continues after sees the same data — no duplicates, no missing rows.

### Test 5b: Backward Scan During Merge

```
session scanner
setup           { BEGIN; SET enable_seqscan = off; }
step scan_bwd1  { SELECT count(*) FROM iso_merge
                  WHERE id BETWEEN 100 AND 500 ORDER BY id DESC; }
step scan_bwd2  { SELECT count(*) FROM iso_merge
                  WHERE id BETWEEN 100 AND 500 ORDER BY id DESC; }
step end        { COMMIT; }

session merger
step merge      { SET btree_merge_low_threshold = 50;
                  SET btree_merge_high_threshold = 90;
                  FIX BLOAT INDEX iso_merge_idx; }

permutation scan_bwd1 merge scan_bwd2 end
```

### Test 5c: Insert Into Merge Range During Merge

```
session inserter
step insert     { INSERT INTO iso_merge SELECT generate_series(101, 110); }

session merger
step merge      { SET btree_merge_low_threshold = 50;
                  SET btree_merge_high_threshold = 90;
                  FIX BLOAT INDEX iso_merge_idx; }
step verify     { SELECT bt_index_parent_check('iso_merge_idx',
                         heapallindexed => true); }

permutation merge insert verify
permutation insert merge verify
```

**What this proves:** Inserts concurrent with (or interleaved with) the
merge don't cause corruption or lost tuples.

### Test 5d: VACUUM During Merge

```
session vacuumer
step vac        { VACUUM iso_merge; }

session merger
step merge      { FIX BLOAT INDEX iso_merge_idx; }
step verify     { SELECT bt_index_parent_check('iso_merge_idx'); }

permutation merge vac verify
permutation vac merge verify
```

---

## 6. Crash Recovery TAP Tests

Location: `src/test/recovery/t/NNN_btree_merge.pl`

These use PostgreSQL's TAP test framework (`PostgreSQL::Test::Cluster`)
to test WAL replay after crashes.

### Test 6a: Crash After Complete Merge

```perl
# Setup
my $node = PostgreSQL::Test::Cluster->new('merge_recovery');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', q{
  CREATE EXTENSION amcheck;
  CREATE EXTENSION pgstattuple;
  CREATE TABLE crash_test (id int, pad text);
  CREATE INDEX crash_idx ON crash_test (id);
  INSERT INTO crash_test SELECT i, repeat('x', 100)
    FROM generate_series(1, 100000) i;
  DELETE FROM crash_test WHERE id % 20 != 0;
  VACUUM crash_test;
});

# Run merge
$node->safe_psql('postgres', q{
  SET btree_merge_low_threshold = 50;
  SET btree_merge_high_threshold = 90;
  FIX BLOAT INDEX crash_idx;
});

# Crash immediately
$node->stop('immediate');

# Restart (WAL replay happens here)
$node->start;

# Verify after recovery
my $result = $node->safe_psql('postgres',
  q{SELECT bt_index_parent_check('crash_idx', heapallindexed => true)});
is($result, '', 'amcheck passes after crash recovery');

my $count = $node->safe_psql('postgres',
  q{SELECT count(*) FROM crash_test});
is($count, '5000', 'all rows accessible after recovery');
```

### Test 6b: Crash Between Phase 1 and Phase 2

This is the most critical crash test.  After Phase 1, the source page is
half-dead but still in the sibling chain.  Recovery must handle this
correctly (VACUUM finishes the unlink on next run).

```perl
# This is harder to orchestrate — we may need to use pg_walinspect
# to find the XLOG_BTREE_MERGE record LSN and do PITR to that point,
# similar to contrib/amcheck/t/005_pitr.pl

# The approach:
# 1. Create bloated index on primary
# 2. Take backup
# 3. Run FIX BLOAT on primary
# 4. Use pg_walinspect to find the XLOG_BTREE_MERGE LSN
#    (this is Phase 1 — source is half-dead)
# 5. Create replica restoring to that LSN (before UNLINK_PAGE)
# 6. On replica: amcheck must pass (half-dead page is P_IGNORE, skipped)
# 7. On replica: VACUUM must finish the unlink
# 8. After VACUUM: amcheck must still pass
```

### Test 6c: Crash + Concurrent Activity After Restart

```perl
# After crash recovery, run concurrent inserts and scans
# to ensure the recovered index is fully functional

$node->safe_psql('postgres', q{
  INSERT INTO crash_test SELECT generate_series(100001, 100500);
  SET enable_seqscan = off;
  SELECT count(*) FROM crash_test WHERE id BETWEEN 1 AND 50000;
  SELECT bt_index_parent_check('crash_idx', heapallindexed => true);
});
```

---

## 7. Performance Benchmarks

### Why Benchmark

Proving bloat reduction without performance data is like proving a diet
works by showing before/after photos without a scale.  Reviewers will
ask: "How long does it take?  Does it block queries?  Is the lock
duration acceptable?"

### Benchmark 1: Bloat Reduction Effectiveness

**Goal:** Measure how much `FIX BLOAT` reduces index size under various
bloat patterns.

```sql
-- Setup: create a standard pgbench database
-- pgbench -i -s 100 testdb   (10M rows)

-- Create bloat scenarios
CREATE TABLE bench_bloat (id int, val text);
CREATE INDEX bench_bloat_idx ON bench_bloat (id);
ALTER TABLE bench_bloat SET (autovacuum_enabled = false);

-- Scenario A: uniform 90% delete (worst case)
INSERT INTO bench_bloat SELECT i, repeat('x', 100)
  FROM generate_series(1, 1000000) i;
DELETE FROM bench_bloat WHERE id % 10 != 0;
VACUUM bench_bloat;

-- Measure BEFORE
\timing on
SELECT * FROM pgstatindex('bench_bloat_idx');
SELECT pg_relation_size('bench_bloat_idx') AS idx_size_before;
SELECT pg_size_pretty(pg_relation_size('bench_bloat_idx'));

-- Merge
FIX BLOAT INDEX bench_bloat_idx;

-- Measure AFTER
SELECT * FROM pgstatindex('bench_bloat_idx');
SELECT pg_relation_size('bench_bloat_idx') AS idx_size_after;
SELECT pg_size_pretty(pg_relation_size('bench_bloat_idx'));
```

**Metrics to record:**

| Metric | How | Unit |
|--------|-----|------|
| Index size before | `pg_relation_size()` | bytes |
| Index size after | `pg_relation_size()` | bytes |
| Size reduction % | `(before - after) / before * 100` | % |
| Leaf pages before | `pgstatindex().leaf_pages` | count |
| Leaf pages after | `pgstatindex().leaf_pages` | count |
| Pages merged | Command output (NOTICE) | count |
| Average leaf density before | `pgstatindex().avg_leaf_density` | % |
| Average leaf density after | `pgstatindex().avg_leaf_density` | % |
| Deleted pages (for recycling) | `pgstatindex().deleted_pages` | count |
| Empty pages | `pgstatindex().empty_pages` | count |
| Wall clock time | `\timing` | ms |

**Bloat scenarios to test:**

| Scenario | Delete Pattern | Expected Reduction |
|----------|---------------|--------------------|
| A: Uniform 90% | `WHERE id % 10 != 0` | ~80–90% |
| B: Uniform 50% | `WHERE id % 2 != 0` | ~40–50% |
| C: Range delete | `WHERE id BETWEEN 1 AND 900000` | ~80% (front-heavy) |
| D: Random delete | `WHERE random() < 0.9` | ~80% (varies by page) |
| E: No bloat | No delete | 0% (no-op) |
| F: Light bloat (20%) | `WHERE id % 5 != 0` | Small (may not hit threshold) |

### Benchmark 2: Query Performance Impact

**Goal:** Prove that queries are not slower after merge (and ideally
faster, due to fewer pages to scan).

```sql
-- Before FIX BLOAT
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bench_bloat WHERE id = 50000;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bench_bloat
  WHERE id BETWEEN 10000 AND 20000;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_bloat;

-- After FIX BLOAT
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bench_bloat WHERE id = 50000;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM bench_bloat
  WHERE id BETWEEN 10000 AND 20000;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_bloat;
```

**Metrics to compare:**

| Query Type | Metric | Expected After Merge |
|------------|--------|---------------------|
| Point lookup | Buffers hit/read | Same (tree height unchanged) |
| Range scan | Buffers hit/read | **Fewer** (fewer leaf pages) |
| Count(*) | Buffers hit/read | **Fewer** (fewer leaf pages) |
| Point lookup | Execution time | Same or slightly better |
| Range scan | Execution time | **Faster** (less I/O) |

### Benchmark 3: Lock Duration and Throughput Impact

**Goal:** Prove that `FIX BLOAT` does not block concurrent queries for
unacceptable durations.

```bash
# Terminal 1: run FIX BLOAT
psql -c "FIX BLOAT INDEX bench_bloat_idx;" testdb

# Terminal 2: run pgbench concurrently
pgbench -c 10 -j 2 -T 60 -f custom_script.sql testdb
```

Where `custom_script.sql` does point lookups and range scans on the
bloated table:

```sql
\set id random(1, 1000000)
SELECT * FROM bench_bloat WHERE id = :id;
```

**Metrics to record:**

| Metric | How | Acceptable |
|--------|-----|------------|
| pgbench TPS during merge | pgbench `-T 60` output | ≥ 80% of baseline TPS |
| pgbench avg latency during merge | pgbench output | ≤ 1.5× baseline latency |
| Max lock wait time | `pg_stat_activity` / `log_lock_waits` | < 100ms per page pair |
| Total merge duration | `\timing` | < 10s for 1M-row index |

### Benchmark 4: Comparison with REINDEX

**Goal:** Show that `FIX BLOAT` achieves comparable bloat reduction to
`REINDEX` without the downsides (exclusive lock, full rebuild).

```sql
-- Same setup as Benchmark 1

-- Method 1: FIX BLOAT
SELECT pg_relation_size('bench_bloat_idx') AS before_size;
\timing on
FIX BLOAT INDEX bench_bloat_idx;
\timing off
SELECT pg_relation_size('bench_bloat_idx') AS after_fix_bloat;

-- Rebuild for comparison
REINDEX INDEX bench_bloat_idx;
SELECT pg_relation_size('bench_bloat_idx') AS after_reindex;
```

**Comparison table:**

| Metric | FIX BLOAT | REINDEX |
|--------|-----------|---------|
| Size reduction | Measure | Measure |
| Duration | Measure | Measure |
| Lock level | Cleanup locks (per page) | AccessExclusiveLock (entire index) |
| Concurrent queries blocked | No | Yes |
| Extra disk space needed | None | 2× index size |
| WAL generated | Small (per merge) | Large (full index) |

### Running the Full Benchmark Suite

```bash
#!/bin/bash
# benchmark.sh — run all performance benchmarks

DB="merge_bench"
SCALE=100  # pgbench scale factor

# Initialize
createdb $DB
pgbench -i -s $SCALE $DB
psql $DB -c "CREATE EXTENSION amcheck; CREATE EXTENSION pgstattuple;"

# Create bloat scenarios and run benchmarks
for DELETE_PCT in 90 50 20; do
  echo "=== Scenario: ${DELETE_PCT}% delete ==="

  psql $DB <<EOF
    CREATE TABLE bench_${DELETE_PCT} (LIKE pgbench_accounts INCLUDING ALL);
    INSERT INTO bench_${DELETE_PCT} SELECT * FROM pgbench_accounts;
    CREATE INDEX bench_${DELETE_PCT}_idx ON bench_${DELETE_PCT} (aid);
    ALTER TABLE bench_${DELETE_PCT} SET (autovacuum_enabled = false);

    -- Delete
    DELETE FROM bench_${DELETE_PCT}
      WHERE aid % (100 / (100 - ${DELETE_PCT})) != 0;
    VACUUM bench_${DELETE_PCT};

    -- Before
    SELECT 'BEFORE' AS phase, * FROM pgstatindex('bench_${DELETE_PCT}_idx');
    SELECT 'BEFORE' AS phase,
           pg_relation_size('bench_${DELETE_PCT}_idx') AS size;

    -- Merge
    \timing on
    FIX BLOAT INDEX bench_${DELETE_PCT}_idx;
    \timing off

    -- After
    SELECT 'AFTER' AS phase, * FROM pgstatindex('bench_${DELETE_PCT}_idx');
    SELECT 'AFTER' AS phase,
           pg_relation_size('bench_${DELETE_PCT}_idx') AS size;

    -- Integrity
    SELECT bt_index_parent_check('bench_${DELETE_PCT}_idx',
                                 heapallindexed => true);

    DROP TABLE bench_${DELETE_PCT};
EOF
done
```

---

## 8. Bloat Reduction Measurements

### The Tool: `pgstatindex()`

`pgstatindex()` (`contrib/pgstattuple`) is the standard PostgreSQL tool
for measuring B-tree index statistics.  It scans every page and reports:

```sql
SELECT * FROM pgstatindex('idx_name');
```

| Column | Meaning | Relevant to merge |
|--------|---------|-------------------|
| `version` | B-tree version | No |
| `tree_level` | Tree height | Not expected to change (leaf-only merge) |
| `index_size` | Total index size in bytes | **Primary reduction metric** |
| `root_block_no` | Root page block number | May change if tree shrinks |
| `internal_pages` | Number of internal pages | Not expected to change |
| `leaf_pages` | Number of leaf pages | **Must decrease** |
| `empty_pages` | Half-dead pages | Increases temporarily (tombstones) |
| `deleted_pages` | Fully deleted pages | Increases after VACUUM recycles |
| `avg_leaf_density` | Average fill % of leaf pages | **Must increase** |
| `leaf_fragmentation` | % of out-of-order leaf pages | May decrease |

### How to Measure Bloat

```sql
-- Step 1: Before merge
SELECT leaf_pages, avg_leaf_density,
       pg_relation_size('idx') AS index_bytes
  FROM pgstatindex('idx');

-- Step 2: Run merge
FIX BLOAT INDEX idx;

-- Step 3: After merge (before VACUUM recycles tombstones)
SELECT leaf_pages, avg_leaf_density,
       pg_relation_size('idx') AS index_bytes,
       empty_pages AS tombstones
  FROM pgstatindex('idx');

-- Step 4: After VACUUM (tombstones recycled, actual disk space freed)
VACUUM tablename;
SELECT leaf_pages, avg_leaf_density,
       pg_relation_size('idx') AS index_bytes,
       deleted_pages AS recycled
  FROM pgstatindex('idx');
```

**Important:** `pg_relation_size()` won't decrease until VACUUM recycles
the tombstone pages.  After merge, the pages are half-dead/deleted but
still allocated.  The *logical* bloat reduction (fewer live leaf pages,
higher density) is immediate.  The *physical* disk reduction requires
VACUUM + the OS to reclaim the space (or a subsequent insert to reuse
the pages).

### Bloat Reduction Formula

```
Reduction% = (leaf_pages_before - leaf_pages_after) / leaf_pages_before × 100
Density_gain = avg_leaf_density_after - avg_leaf_density_before
```

### Expected Results

| Delete % | Leaf Pages Before | Expected After | Expected Reduction |
|----------|-------------------|----------------|-------------------|
| 90% | ~1000 | ~100–150 | 85–90% |
| 80% | ~1000 | ~200–250 | 75–80% |
| 50% | ~1000 | ~500–550 | 45–50% |
| 20% | ~1000 | ~800–850 | 15–20% |

These estimates assume uniform bloat.  Clustered deletions (e.g., range
deletes) will show even better reduction because entire pages become
mergeable.

---

## 9. GUC Threshold Tests

### What to Test

| GUC Setting | Scenario | Expected |
|-------------|----------|----------|
| `low=10, high=70` (default) | Pages at 8% fill → merge | Merges happen |
| `low=10, high=70` (default) | Pages at 12% fill → no merge | No merges |
| `low=50, high=90` (aggressive) | Pages at 40% fill → merge | Merges happen |
| `low=1, high=70` (ultra-conservative) | Only nearly-empty pages merge | Few merges |
| `low=0, high=70` (disabled) | No pages qualify | Zero merges, no errors |
| `low=100, high=100` (merge everything) | All non-full pages qualify | Maximum merges |
| `low=50, high=50` (impossible) | Combined fill can never be ≤50% if source is ≤50% and dest has data | Very few merges |

### How to Test

```sql
-- Test: conservative thresholds
SET btree_merge_low_threshold = 10;
SET btree_merge_high_threshold = 70;
FIX BLOAT INDEX idx;
-- Record pages merged, verify amcheck

-- Test: aggressive thresholds
SET btree_merge_low_threshold = 50;
SET btree_merge_high_threshold = 90;
FIX BLOAT INDEX idx;
-- Record pages merged (should be more), verify amcheck

-- Test: disabled
SET btree_merge_low_threshold = 0;
FIX BLOAT INDEX idx;
-- Should do nothing
```

---

## 10. Edge Case Tests

| Edge Case | How to Create | What to Verify |
|-----------|---------------|----------------|
| **Rightmost leaf page is sparse** | Delete most data from highest keys | Not merged (no right sibling), amcheck passes |
| **Leftmost leaf page is sparse** | Delete most data from lowest keys | Can be merged into its right sibling (it's the source) |
| **Single leaf page in index** | Very small table (< 1 page of data) | No merge possible, command exits cleanly |
| **All pages below threshold** | Delete 99% uniformly | Chain merges reduce to minimal pages |
| **Index with INCLUDE columns** | Covering index | INCLUDE data moves correctly, amcheck passes |
| **Index with deduplication** | Many duplicate keys | Posting list tuples handled correctly |
| **Index on expression** | `CREATE INDEX ON t ((lower(col)))` | Expression index merged correctly |
| **Partial index** | `CREATE INDEX ON t (id) WHERE id > 0` | Only qualifying rows in index, merge correct |
| **TOAST'ed values in index** | Long text keys | Large tuples move correctly |
| **Concurrent DDL** | `ALTER INDEX ... SET (fillfactor=50)` during merge | Merge uses current fillfactor or aborts |
| **Index on partitioned table** | Each partition has its own index | FIX BLOAT on each partition's index independently |

---

## 11. Test Files and Locations

| File | Type | Contents |
|------|------|----------|
| `src/test/regress/sql/btree_merge.sql` | Regression | Tests 1–5 from Section 4 |
| `src/test/regress/expected/btree_merge.out` | Expected output | Known-good output for regression |
| `src/test/isolation/specs/btree-merge.spec` | Isolation | Tests 5a–5d from Section 5 |
| `src/test/recovery/t/NNN_btree_merge.pl` | TAP/Recovery | Tests 6a–6c from Section 6 |
| `docs/btree_page_merge_benchmarks.md` | Documentation | Benchmark results (Section 7–8) |

---

## 12. Test Matrix Summary

| Component | Correctness (amcheck) | Data Integrity | Bloat Reduction | Concurrency | Crash Recovery |
|-----------|----------------------|----------------|-----------------|-------------|----------------|
| `_bt_merge_pages()` | ✅ Level 2+3 | ✅ count(*) + scans | ✅ pgstatindex | — | ✅ TAP crash |
| `btree_xlog_merge()` | ✅ on standby | ✅ after replay | — | — | ✅ TAP crash |
| `_bt_try_merge()` | ✅ Level 2 | ✅ count(*) | — | ✅ isolation | — |
| `FIX BLOAT` scan loop | ✅ Level 2+3 | ✅ count(*) + scans | ✅ pgstatindex + pg_relation_size | ✅ isolation | ✅ TAP crash |
| GUC thresholds | ✅ Level 2 | ✅ count(*) | ✅ vary thresholds | — | — |
| Edge cases | ✅ Level 2 | ✅ count(*) | — | — | — |
| Benchmarks | ✅ Level 2 | — | ✅ full suite | ✅ pgbench concurrent | — |

### amcheck Call Summary

| When | Which Function | Options | Why |
|------|----------------|---------|-----|
| After every merge in regression tests | `bt_index_parent_check()` | `heapallindexed => true` | Prove structure + no lost tuples |
| After crash recovery | `bt_index_parent_check()` | `heapallindexed => true` | Prove replay produced correct state |
| After isolation tests | `bt_index_parent_check()` | default | Prove no concurrency corruption |
| In benchmarks | `bt_index_parent_check()` | default | Sanity check (don't want corrupt fast index) |
| Primary regression test | `bt_index_parent_check()` | `heapallindexed => true, rootdescend => true` | Maximum paranoia — full structural + tuple-level + re-search verification |

---

*This testing plan ensures that every invariant the merge must maintain
is verified by amcheck, every performance claim is backed by
measurements, and every crash/concurrency scenario is exercised.  No
hand-waving — every test produces a pass/fail result.*
