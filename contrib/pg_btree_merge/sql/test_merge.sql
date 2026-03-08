-- Test script for pg_btree_merge prototype

-- Create the extension
CREATE EXTENSION IF NOT EXISTS pg_btree_merge;
CREATE EXTENSION IF NOT EXISTS amcheck;

-- Create a test table
CREATE TABLE test_bloat (
    id SERIAL PRIMARY KEY,
    data TEXT
);

-- Create an index we'll target
CREATE INDEX idx_test_bloat ON test_bloat(id);

-- Insert test data
INSERT INTO test_bloat (data) SELECT 'row ' || i::text FROM generate_series(1, 10000) i;

-- Create some sparsity by deleting 70% of rows
DELETE FROM test_bloat WHERE id % 10 <= 6;

-- Show initial index size
\d+ idx_test_bloat

-- Check structural integrity before merge
SELECT bt_index_check('idx_test_bloat'::regclass);

-- Run the merge (this is what we're testing)
SELECT pg_btree_merge_pages('idx_test_bloat'::regclass);

-- Check structural integrity after merge  
SELECT bt_index_check('idx_test_bloat'::regclass);

-- Verify data integrity - run some queries
SELECT COUNT(*) FROM test_bloat;
SELECT MIN(id), MAX(id) FROM test_bloat;
SELECT COUNT(*) FROM test_bloat WHERE id % 10 > 6;

-- Show final index size (should be smaller)
\d+ idx_test_bloat

-- Cleanup
DROP TABLE test_bloat CASCADE;
DROP EXTENSION pg_btree_merge;
