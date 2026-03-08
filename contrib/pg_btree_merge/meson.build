# pg_btree_merge extension

pg_btree_merge_sources = files(
  'pg_btree_merge.c',
)

if host_system == 'windows'
  pg_btree_merge_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_btree_merge',
    '--FILEDESC', 'pg_btree_merge - function for online B-tree page merging',])
endif

pg_btree_merge = shared_module('pg_btree_merge',
  pg_btree_merge_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_btree_merge

install_data(
  'pg_btree_merge.control',
  'pg_btree_merge--1.0.sql',
  kwargs: contrib_data_args,
)
