# contrib/pg_btree_merge/Makefile

MODULE_big	= pg_btree_merge
OBJS = \
	$(WIN32RES) \
	pg_btree_merge.o

EXTENSION = pg_btree_merge
DATA = sql/pg_btree_merge--1.0.sql
PGFILEDESC = "pg_btree_merge - function for online B-tree page merging"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_btree_merge
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

REGRESS = test_merge test_scans
