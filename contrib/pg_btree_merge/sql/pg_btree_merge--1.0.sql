/* contrib/pg_btree_merge/pg_btree_merge--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_btree_merge" to load this file. \quit

--
-- pg_btree_merge_pages()
--
CREATE FUNCTION pg_btree_merge_pages(index regclass)
RETURNS VOID
AS 'MODULE_PATHNAME', 'pg_btree_merge_pages'
LANGUAGE C STRICT PARALLEL RESTRICTED;

GRANT EXECUTE ON FUNCTION pg_btree_merge_pages(regclass) TO PUBLIC;
