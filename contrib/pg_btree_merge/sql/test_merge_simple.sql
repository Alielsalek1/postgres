-- Simple test script for pg_btree_merge prototype (no amcheck required)

CREATE EXTENSION pg_btree_merge;

-- Create a test table 
CREATE TABLE test_bloat (
    id SERIAL,
    data TEXT
);

-- Create an index
CREATE INDEX idx_test_bloat ON test_bloat(id);

-- Insert test data
INSERT INTO test_bloat (data) SELECT 'row ' || i::text FROM generate_series(1, 10000) i;

-- Create sparsity by deleting 70% of rows
DELETE FROM test_bloat WHERE id % 10 <= 6;

-- Show initial index size
SELECT pg_relation_size('idx_test_bloat'::regclass) AS index_size_before;

--  Run the merge (this is what we're testing!)
SELECT pg_btree_merge_pages('idx_test_bloat'::regclass);

-- Show final index size
SELECT pg_relation_size('idx_test_bloat'::regclass) AS index_size_after;

-- Verify data integrity
SELECT 'Data check:' AS check_type,
       COUNT(*) AS count,
       MIN(id) AS min_id,
       MAX(id) AS max_id
FROM test_bloat;

-- Verify queries work correctly
SELECT 'Test queries:' AS test;
SELECT * FROM test_bloat WHERE id = 7 ORDER BY id;
SELECT COUNT(*) AS count_id_100_plus FROM test_bloat WHERE id >= 100;

-- Cleanup
DROP TABLE test_bloat CASCADE;
DROP EXTENSION pg_btree_merge;
