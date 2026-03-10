-- Test script for pg_btree_merge prototype

-- Create the extension
CREATE EXTENSION IF NOT EXISTS pg_btree_merge;
CREATE EXTENSION IF NOT EXISTS amcheck;

-- Create a test table
CREATE TABLE test_bloat (
    id int
) WITH (fillfactor=10);

-- Create an index we'll target
CREATE INDEX idx_test_bloat ON test_bloat(id) WITH (fillfactor=10);

-- Insert test data
INSERT INTO test_bloat (id) SELECT i FROM generate_series(1, 10000) i;

-- Create some sparsity by deleting 90% of rows
DELETE FROM test_bloat WHERE id % 10 < 9;

-- Show initial index size
SELECT pg_size_pretty(pg_relation_size('idx_test_bloat')) as size_before_merge;

-- 1. MVCC PROOF: Create a reference table to store the 'before' state
-- This captures exactly what SHOULD be visible.
BEGIN;
CREATE TEMP TABLE visible_before AS 
SELECT id, ctid as target_ctid FROM test_bloat;

-- Check structural integrity before merge
SELECT bt_index_check('idx_test_bloat'::regclass);

-- Run the merge (this is what we're testing)
SELECT pg_btree_merge_pages('idx_test_bloat'::regclass);
SELECT pg_size_pretty(pg_relation_size('idx_test_bloat')) as size_after_merge;

-- Check structural integrity after merge (Deep check with heap verification)
SELECT bt_index_check('idx_test_bloat'::regclass, true);

-- 2. MVCC PROOF: Compare 'before' snapshot with 'after' index scan
-- This query returns 0 if every record that was visible is still visible.
-- We force an Index Scan to ensure we are testing the index paths.
SET enable_seqscan = off;
SELECT count(*) as hidden_or_corrupted_records
FROM (
    SELECT id, target_ctid FROM visible_before
    EXCEPT
    SELECT id, ctid FROM test_bloat
) validation;
RESET enable_seqscan;

-- 3. MVCC PROOF: Bidirectional Scan Integrity
-- Merges can break the 'prev' or 'next' pointers. 
-- We verify both forward and backward scans return the same count.
SET enable_seqscan = off;
WITH forward AS (
    SELECT id FROM test_bloat ORDER BY id ASC
),
backward AS (
    SELECT id FROM test_bloat ORDER BY id DESC
)
SELECT 
    (SELECT count(*) FROM forward) as fwd_count,
    (SELECT count(*) FROM backward) as bwd_count,
    ((SELECT count(*) FROM forward) = (SELECT count(*) FROM backward)) as scans_match;
RESET enable_seqscan;
COMMIT;

-- Verify data integrity - run some queries
SELECT COUNT(*) FROM test_bloat;
SELECT MIN(id), MAX(id) FROM test_bloat;
SELECT COUNT(*) FROM test_bloat WHERE id % 10 > 7;

-- Now run VACUUM to reclaim those BTP_DELETED pages!
-- We must vacuum the TABLE to affect its indexes.
VACUUM test_bloat;
SELECT pg_size_pretty(pg_relation_size('idx_test_bloat')) as size_after_vacuum;

-- Re-check structural integrity after VACUUM.
-- Vacuumed pages are no longer linked to anything.
SELECT bt_index_parent_check('idx_test_bloat'::regclass, true);

-- Force index scans and verify forward/backward ordered traversal.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;

-- Planner proof: should use index scan directionally.
EXPLAIN (ANALYZE, COSTS OFF) SELECT id FROM test_bloat ORDER BY id ASC;
EXPLAIN (ANALYZE, COSTS OFF) SELECT id FROM test_bloat ORDER BY id DESC;

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;

-- Insert new rows. This should reuse the space from the deleted pages
-- instead of extending the file further.
INSERT INTO test_bloat (id) SELECT i FROM generate_series(2000, 5000) i;

-- Final size check.
SELECT pg_size_pretty(pg_relation_size('idx_test_bloat')) as final_size;

-- Cleanup
DROP TABLE test_bloat CASCADE;
DROP EXTENSION pg_btree_merge;
