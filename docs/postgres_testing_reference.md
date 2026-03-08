# PostgreSQL Testing Infrastructure — A Reference

> How PostgreSQL's test suites work and which ones we need for the
> B-tree page merge project.  Covers regression tests, TAP tests,
> isolation tests, and how to use amcheck + pgstatindex for
> correctness and performance verification.

---

## Table of Contents

1. [Overview of Test Suites](#1-overview-of-test-suites)
2. [Regression Tests (SQL Tests)](#2-regression-tests-sql-tests)
3. [TAP Tests (Perl-Based)](#3-tap-tests-perl-based)
4. [Isolation Tests](#4-isolation-tests)
5. [amcheck — Our Primary Correctness Verifier](#5-amcheck--our-primary-correctness-verifier)
6. [pgstatindex — Our Primary Bloat Metric](#6-pgstatindex--our-primary-bloat-metric)
7. [pgbench — Concurrent Workload Generator](#7-pgbench--concurrent-workload-generator)
8. [Running Tests](#8-running-tests)
9. [Test File Locations](#9-test-file-locations)
10. [What We Need to Write](#10-what-we-need-to-write)

---

## 1. Overview of Test Suites

PostgreSQL has several test frameworks, each for a different purpose:

| Framework | Language | Purpose | Runs Against |
|-----------|----------|---------|-------------|
| Regression tests | SQL + expected output | Correctness of SQL features | Running server |
| TAP tests | Perl (Test::More) | Server lifecycle, crash recovery, replication | Manages its own server instances |
| Isolation tests | Spec files (custom DSL) | Concurrency, lock interactions | Running server with special isolation tester |
| `pg_regress` | C harness | Runs regression tests | Utility program |
| `pg_isolation_regress` | C harness | Runs isolation tests | Utility program |

For our page merge:
- **Regression tests** — basic correctness (merge works, produces right results)
- **TAP tests** — crash recovery (crash after merge, PITR, promotion)
- **Isolation tests** — concurrency (scan during merge, insert during merge)

---

## 2. Regression Tests (SQL Tests)

### Structure

Each test has two files:
- `sql/test_name.sql` — the SQL commands to run
- `expected/test_name.out` — the expected output (stdout)

The test passes if the actual output matches the expected output exactly
(after some normalization).

### Example

```sql
-- sql/btree_merge.sql
CREATE TABLE t (id int, val text);
INSERT INTO t SELECT g, 'data_' || g FROM generate_series(1, 10000) g;
CREATE INDEX idx ON t(id);

-- Delete 90% to create bloat
DELETE FROM t WHERE id % 10 != 0;
VACUUM t;  -- clean up heap, leave index bloated

-- Check bloat before
SELECT leaf_pages, avg_leaf_density FROM pgstatindex('idx');

-- Merge
FIX BLOAT INDEX idx;

-- Verify correctness
SELECT bt_index_parent_check('idx', true);

-- Check bloat after
SELECT leaf_pages, avg_leaf_density FROM pgstatindex('idx');

-- Verify data is still correct
SELECT count(*) FROM t WHERE id % 10 = 0;
```

```
-- expected/btree_merge.out
...expected output here...
```

### How to Generate Expected Output

```bash
# Run the test, capturing actual output:
cd src/test/regress
./pg_regress --schedule=parallel_schedule btree_merge

# If the test doesn't have expected output yet:
# The actual output goes to results/btree_merge.out
# Copy it to expected/ after verifying it's correct:
cp results/btree_merge.out expected/btree_merge.out
```

---

## 3. TAP Tests (Perl-Based)

### Purpose

TAP tests manage their own PostgreSQL server instances.  They can:
- Start/stop servers
- Crash servers (kill -9) and verify recovery
- Set up replication
- Test PITR (Point-In-Time Recovery)

### Structure

TAP tests live in `t/` directories and use Perl's `Test::More` framework
with PostgreSQL-specific helpers from `PostgreSQL::Test::Cluster`.

### Key APIs

```perl
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

# Create and start a test server
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->start;

# Run SQL commands
my $result = $node->safe_psql('dbname', 'SELECT 1');

# Run SQL that might fail
my ($ret, $stdout, $stderr) = $node->psql('dbname', 'SELECT 1/0');

# Crash the server (kill -9)
$node->stop('immediate');

# Restart (triggers recovery)
$node->start;

# Wait for recovery to complete
$node->poll_query_until('dbname', "SELECT pg_is_in_recovery() = false");

# Check WAL replay
$node->safe_psql('dbname', "SELECT bt_index_parent_check('idx', true)");

# Assertions
is($result, '1', 'query returns 1');
ok($ret == 0, 'query succeeded');
```

### Crash Recovery Test Pattern

```perl
# 1. Create index, fill with data, create bloat
$node->safe_psql('test', q{
    CREATE TABLE t (id int);
    INSERT INTO t SELECT g FROM generate_series(1, 10000) g;
    CREATE INDEX idx ON t(id);
    DELETE FROM t WHERE id % 10 != 0;
    VACUUM t;
});

# 2. Perform merge
$node->safe_psql('test', "FIX BLOAT INDEX idx");

# 3. Crash the server
$node->stop('immediate');

# 4. Restart (WAL replay happens automatically)
$node->start;

# 5. Verify index integrity after recovery
my $check = $node->safe_psql('test',
    "SELECT bt_index_parent_check('idx', true)");
is($check, '', 'amcheck passes after crash recovery');

# 6. Verify data correctness
my $count = $node->safe_psql('test',
    "SELECT count(*) FROM t WHERE id % 10 = 0");
is($count, '1000', 'data survived crash recovery');
```

---

## 4. Isolation Tests

### Purpose

Isolation tests verify that concurrent operations interact correctly.
They define multiple "sessions" that execute steps in a controlled
interleaved order.

### Spec File Format

```
# isolation/specs/btree_merge.spec

setup
{
    CREATE TABLE t (id int PRIMARY KEY);
    INSERT INTO t SELECT g FROM generate_series(1, 1000) g;
    CREATE INDEX idx ON t(id);
    DELETE FROM t WHERE id > 100;
    VACUUM t;
}

teardown
{
    DROP TABLE t;
}

session s1     # the merger
setup           { /* optional per-session setup */ }
step merge      { FIX BLOAT INDEX idx; }
step check      { SELECT bt_index_parent_check('idx', true); }

session s2     # concurrent scanner
step scan       { SELECT count(*) FROM t WHERE id BETWEEN 50 AND 150; }

session s3     # concurrent inserter
step insert     { INSERT INTO t SELECT g FROM generate_series(101, 200) g; }

# Define the interleaving:
permutation merge scan check       # scan during merge
permutation merge insert check     # insert during merge
permutation scan merge check       # merge during scan
permutation insert merge check     # merge during insert
```

### How Isolation Tests Work

The isolation tester:
1. Starts all sessions.
2. Executes steps in the order specified by the `permutation` line.
3. A step blocks if it would wait for a lock held by another session.
4. The tester detects the block and proceeds to the next session's step.
5. Output includes which steps blocked and when.

### Expected Output

```
# expected/btree_merge.out

starting permutation: merge scan check
step merge: FIX BLOAT INDEX idx;
step scan: SELECT count(*) FROM t WHERE id BETWEEN 50 AND 150;
count
-----
  100
step check: SELECT bt_index_parent_check('idx', true);
bt_index_parent_check
-----
(0 rows)
...
```

---

## 5. amcheck — Our Primary Correctness Verifier

### What It Checks

`amcheck` is an extension (`contrib/amcheck`) that performs structural
integrity checks on B-tree indexes.

| Function | What It Checks |
|----------|---------------|
| `bt_index_check(index)` | Page structure, item order within pages |
| `bt_index_parent_check(index, heapallindexed)` | All of `bt_index_check` PLUS parent-child key consistency, cross-page ordering |

### Level 1: `bt_index_check`

```sql
SELECT bt_index_check('idx');
```

Checks:
- ✅ Items on each page are in key order
- ✅ Page headers are valid
- ✅ No corrupted tuples
- ✅ High keys are correct
- ❌ Does NOT check parent-child consistency

### Level 2: `bt_index_parent_check`

```sql
SELECT bt_index_parent_check('idx', true);
```

Checks everything in Level 1 PLUS:
- ✅ Every leaf page is reachable from the root via downlinks
- ✅ Parent pivot keys correctly bound child page key ranges
- ✅ Sibling links are consistent
- ✅ `heapallindexed=true`: every heap tuple that should be in the
  index IS in the index (no missing entries)

**This is our must-pass test.** After every merge, we run:

```sql
SELECT bt_index_parent_check('idx', true);
```

If this returns without error, the index structure is correct.

### How amcheck Interacts with Merge

After a merge:
- L is half-dead (no downlink from parent) — amcheck skips half-dead pages
- R has L's old tuples prepended — amcheck verifies R's items are in order
  and R's high key correctly bounds them
- Parent has updated pivot — amcheck verifies the pivot correctly bounds
  R's key range

If ANY of these invariants are broken, `bt_index_parent_check` will
raise an ERROR with a specific message telling you what's wrong.

---

## 6. pgstatindex — Our Primary Bloat Metric

### What It Reports

`pgstatindex` (from `contrib/pgstattuple`) provides physical statistics
about a B-tree index:

```sql
SELECT * FROM pgstatindex('idx');
```

| Column | Meaning | Relevance |
|--------|---------|-----------|
| `version` | B-tree version (4) | Not relevant |
| `tree_level` | Depth of tree | Should decrease or stay same after merge |
| `index_size` | Total bytes | Should decrease after merge |
| `root_block_no` | Root page number | Should not change |
| `internal_pages` | Number of internal pages | May decrease |
| `leaf_pages` | Number of leaf pages | **Should decrease** — our primary metric |
| `empty_pages` | Pages with no items | Should increase (freed pages) |
| `deleted_pages` | Pages marked deleted | Should increase during merge, decrease after VACUUM recycles |
| `avg_leaf_density` | Average % of leaf pages used | **Should increase** — our secondary metric |
| `leaf_fragmentation` | % of pages out of logical order | Should decrease |

### Example Measurement

```sql
-- Before merge:
SELECT leaf_pages, avg_leaf_density FROM pgstatindex('idx');
-- leaf_pages: 1000, avg_leaf_density: 10.5

-- After merge:
SELECT leaf_pages, avg_leaf_density FROM pgstatindex('idx');
-- leaf_pages: 120, avg_leaf_density: 85.0
-- → merged 880 pages, density increased 8x
```

### Interpreting Results

| Scenario | leaf_pages | avg_leaf_density | Assessment |
|----------|-----------|-----------------|------------|
| Before: 90% deleted | 1000 | 10% | Very bloated |
| After merge | 120 | 85% | Bloat resolved |
| Before: 50% deleted | 1000 | 50% | Moderately bloated |
| After merge | 550 | 90% | Good improvement |
| Before: uniform 80% | 1000 | 80% | Not bloated |
| After merge | 1000 | 80% | Nothing to merge (correct) |

---

## 7. pgbench — Concurrent Workload Generator

### Purpose

`pgbench` generates concurrent transactional workloads.  We use it to
measure TPS (transactions per second) during merge operations.

### Usage Pattern

```bash
# Initialize pgbench tables
pgbench -i -s 10 testdb

# Run workload in background while FIX BLOAT runs
pgbench -c 10 -j 4 -T 60 testdb &
psql testdb -c "FIX BLOAT INDEX pgbench_accounts_pkey"
wait

# Check TPS from pgbench output
```

### Custom Scripts

```sql
-- pgbench_script.sql — mix of reads and writes
\set aid random(1, 100000 * :scale)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :aid;
```

```bash
pgbench -c 10 -j 4 -T 60 -f pgbench_script.sql testdb
```

---

## 8. Running Tests

### Regression Tests

```bash
# Run all regression tests:
make check  # from top-level, initializes a temp server

# Run specific test:
cd src/test/regress
./pg_regress btree_merge

# Run against an existing server:
make installcheck
```

### TAP Tests

```bash
# Run all TAP tests for a module:
cd src/test/recovery
make check

# Run a specific TAP test:
cd src/test/recovery
prove -v t/042_btree_merge.pl
```

### Isolation Tests

```bash
# Run all isolation tests:
cd src/test/isolation
make check

# Run a specific test:
./pg_isolation_regress btree_merge
```

### Using Meson (alternative build system)

```bash
# Run all tests:
meson test -C build

# Run specific test suite:
meson test -C build recovery

# Run with verbose output:
meson test -C build -v
```

---

## 9. Test File Locations

### Where Existing Tests Live

| Type | Location | Example |
|------|----------|---------|
| Regression | `src/test/regress/sql/` | `create_index.sql` |
| Expected | `src/test/regress/expected/` | `create_index.out` |
| TAP (recovery) | `src/test/recovery/t/` | `027_stream_regress.pl` |
| TAP (nbtree) | `src/backend/access/nbtree/t/` | (none currently) |
| Isolation | `src/test/isolation/specs/` | `delete-abort-savept.spec` |
| amcheck tests | `contrib/amcheck/sql/` | `check_btree.sql` |
| pgstattuple tests | `contrib/pgstattuple/sql/` | `pgstattuple.sql` |

### Where Our Tests Will Go

| Test | Location | File |
|------|----------|------|
| Basic merge regression | `src/test/regress/sql/` | `btree_merge.sql` |
| Crash recovery | `src/test/recovery/t/` | `NNN_btree_merge.pl` |
| Concurrency | `src/test/isolation/specs/` | `btree-merge.spec` |
| amcheck integration | (inline in all tests) | Every test ends with `bt_index_parent_check` |

---

## 10. What We Need to Write

### Test 1: Basic Regression (SQL)

**File:** `src/test/regress/sql/btree_merge.sql`

Tests:
- Create table + index
- Delete 90% of rows → create bloat
- VACUUM (clean heap, leave index bloated)
- `pgstatindex` → baseline leaf_pages and avg_leaf_density
- `FIX BLOAT INDEX idx`
- `bt_index_parent_check('idx', true)` → passes
- `pgstatindex` → fewer leaf_pages, higher avg_leaf_density
- Sequential scan matches index scan (data correctness)

### Test 2: Edge Cases (SQL)

**File:** `src/test/regress/sql/btree_merge_edge.sql`

Tests:
- Merge with posting lists (duplicates)
- Merge with INCLUDE columns
- Merge with NULL values
- Already-full destination page → skip (cannot merge)
- Empty source page → skip (nothing to merge)
- Source and destination have different parents → skip (same-parent constraint)
- Rightmost leaf page → different high key handling

### Test 3: Crash Recovery (TAP)

**File:** `src/test/recovery/t/NNN_btree_merge.pl`

Tests:
- Merge → crash → recovery → amcheck passes
- Merge Phase 1 complete (half-dead) → crash → recovery → VACUUM finishes unlink
- PITR to timestamp during merge → consistent state

### Test 4: Concurrency (Isolation)

**File:** `src/test/isolation/specs/btree-merge.spec`

Tests:
- Forward scan during merge (scan on page being merged)
- Backward scan during merge
- INSERT into merge range during merge
- VACUUM on adjacent pages during merge
- Two FIX BLOAT commands on same index (should be serialized)

### The Pillar Pattern

Every test, regardless of type, ends with:

```sql
SELECT bt_index_parent_check('idx', true);
```

If this passes, the index is structurally correct.  This is the
invariant we never violate.

---

*For the complete testing strategy, see `btree_page_merge_testing.md`.
For implementation details, see `btree_page_merge_plan.md`.
For locking details relevant to isolation tests, see `postgres_locking_reference.md`.*
