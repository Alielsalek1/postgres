#!/bin/bash
set -e

# Configuration
RUN_DIR="$PWD"
PG_ROOT="/home/alilililili/Projects/postgres"
PG_DATA="/tmp/pg_btree_merge_test_db"
PG_LOG="$RUN_DIR/pg_btree_merge_run.log"
TEST_LOG="$RUN_DIR/pg_btree_merge_test_output.log"
# Port should be unique to avoid conflicts with existing clusters
PG_PORT=55439
PG_INSTALL="$PG_ROOT/tmp_install/usr/local/pgsql"

# Mirror full script output to a file in the launch directory.
exec > >(tee -a "$TEST_LOG") 2>&1

echo "Test output log: $TEST_LOG"
echo "PostgreSQL server log: $PG_LOG"

echo "=== [1/6] Building and Installing Extension ==="
cd "$PG_ROOT"
# Use temporary install location to make server deployment reliable
make -C contrib/pg_btree_merge clean
make -C contrib/pg_btree_merge
echo "Skipping install" > /dev/null
make -C contrib/pg_btree_merge DESTDIR="$PG_ROOT/tmp_install" install
echo "Build and Install complete."

echo "=== [2/6] Setting up environment ==="
export PATH="$PG_INSTALL/bin:$PATH"
export LD_LIBRARY_PATH="$PG_INSTALL/lib:$LD_LIBRARY_PATH"

# Debug: verify binaries
echo "Checking binaries from $PG_INSTALL/bin..."
which psql || echo "psql NOT FOUND"
which initdb || echo "initdb NOT FOUND"
which pg_ctl || echo "pg_ctl NOT FOUND"
which postgres || echo "postgres NOT FOUND"

echo "=== [3/6] Initializing Test Server ==="
pg_ctl -D "$PG_DATA" stop -m fast 2>/dev/null || true
rm -rf "$PG_DATA"
initdb -D "$PG_DATA"
echo "Starting PostgreSQL on port $PG_PORT..."
pg_ctl -D "$PG_DATA" -l "$PG_LOG" -o "-p $PG_PORT" start
sleep 2

echo "=== [4/6] Creating Test Database ==="
psql -p "$PG_PORT" -d postgres -c "DROP DATABASE IF EXISTS test_merge_run;"
psql -p "$PG_PORT" -d postgres -c "CREATE DATABASE test_merge_run;"
echo "Database 'test_merge_run' created."

echo "=== [5/6] Running B-Tree Merge Test ==="
psql -p "$PG_PORT" -d test_merge_run -f "contrib/pg_btree_merge/sql/test_merge.sql"

echo "=== [6/6] Cleaning up ==="
pg_ctl -D "$PG_DATA" stop -m fast
echo "Test complete. Server stopped."
