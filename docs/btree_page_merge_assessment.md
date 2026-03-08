# B-Tree Page Merge — Plan Assessment

> Honest pros and cons of the development plan.
> Use this to anticipate reviewer questions and strengthen weak areas.

---

## Pros

### 1. Pure Extension — Zero Modification
The entire patch is additive.  No existing function is changed, no
existing behavior is altered.  If reverted, PostgreSQL behaves identically
to today.  This is exactly what committers want to see — zero regression
risk to existing workloads.  This alone puts the proposal ahead of most.

### 2. Deep Reuse of Existing Infrastructure
We call 15+ existing functions as-is and write ~7 new ones.  The ratio
signals that the merge operation is a *natural* extension of the B-tree
subsystem, not a hack.  The two-phase pattern (merge+half-dead, then
unlink) mirrors `_bt_pagedel()` exactly.

### 3. Every Decision Has a Proven "Why"
L→R direction isn't an opinion — it's proven via `_bt_moveright()` code
analysis.  Same-parent constraint comes from the README itself.  Immediate
deletion is proven necessary via the `_bt_findinsertloc()` re-fill
scenario.  This level of rigor is unusual for a GSoC proposal.

### 4. Crash Safety Is Inherited, Not Invented
The crash-between-phases scenario is identical to the existing
`_bt_pagedel()` crash scenario.  VACUUM already finishes orphaned
half-dead pages.  No new recovery logic — a proven recovery path is
reused.

### 5. The Problem Is Real and Well-Known
The nbtree README itself acknowledges the gap (line 235–237).  `REINDEX`
and `pg_repack` are widely used workarounds.  This is a recognized
community pain point, increasing acceptance likelihood.

### 6. Opt-In, DBA-Controlled
No autovacuum hook, no background worker, no automatic behavior change.
A conservative committer can accept this knowing it will never fire
unless someone explicitly asks for it.

---

## Cons / Risks

### 1. The Cleanup-Lock Pin Gap Is Not Fully Resolved
There is a nanosecond window in `_bt_steppage()` where a scanner holds
zero pins between L and R.  The plan argues it's "the same class of risk
tolerated elsewhere" — but that's hand-waving.  A reviewer may push back.

**Mitigation needed:** Either a formal proof that the gap is harmless,
or a concrete check (e.g., an LSN comparison on R after acquiring the
lock, similar to `_bt_killitems()`).  This is the weakest point in the
plan and must be hardened before implementation.

### 2. Command Interface — `FIX BLOAT`
The command is an index-level utility command analogous to `REINDEX`:

```sql
FIX BLOAT INDEX index_name;
FIX BLOAT INDEX CONCURRENTLY index_name;  -- future extension
FIX BLOAT TABLE table_name;               -- all indexes on table
```

It operates on indexes the same way `REINDEX` does — the DBA specifies
which index (or table) to compact.  The name is a working title;
community review may suggest a different name (e.g., `COMPACT INDEX`,
`MERGE INDEX`, `DEFRAGMENT INDEX`).

**Open questions to resolve during implementation:**
- **Relation lock level:** Likely `ShareUpdateExclusiveLock` on the index
  (same as `REINDEX CONCURRENTLY`) to prevent concurrent DDL while
  allowing concurrent DML.  Or possibly no table-level lock at all —
  only buffer-level locks, since the merge doesn't change the index's
  logical content.
- **Permissions:** Likely requires index owner or superuser, same as
  `REINDEX`.
- **Progress reporting:** Should emit `NOTICE` messages (pages scanned,
  pages merged, space freed) and/or expose counters via `pg_stat`
  views.  Production DBAs won't adopt a tool with no observability.

### 3. Same-Parent Constraint Limits Effectiveness
The plan estimates <1% of merge opportunities lost, but this hasn't been
validated empirically.  In heavily fragmented indexes where parents have
only 2–3 children, the rightmost child of each parent is un-mergeable.

**Mitigation:** This is exactly why thresholds are GUCs.  After the core
implementation is complete, we benchmark on real bloated indexes with
various threshold combinations and measure actual merge rates.  If the
same-parent constraint proves too limiting in practice, cross-parent
merge is future work — but we expect it won't be needed for the common
case (uniform bloat across many pages).

### 4. Leaf-Only — Tree Height Doesn't Shrink
After merging many leaf pages, parent pages may become sparse (few
downlinks).  Tree height stays the same.  Traversal cost (root → leaf)
doesn't improve.

**Why this is acceptable:**
- Internal pages are for traversal only — they don't store user data.
  The real bloat cost (I/O, cache pressure, sequential scan waste) is
  in leaf pages.
- Internal page merge is extremely complex — moving a downlink means
  changing the child's parent pointer, creating cascading updates up
  and down the tree.  The complexity-to-benefit ratio is very poor.
- PostgreSQL B-trees rarely exceed 3–4 levels even for billions of rows.
  A few sparse internal pages add microseconds to traversal, not
  milliseconds.
- Leaf page merge is the concrete user complaint.  No one has ever filed
  a bug report about internal page bloat.

### 5. Forward Scan Reprocessing Risk Under Cleanup Locks
The plan claims cleanup locks prevent the forward-scan duplicate
problem.  This depends on the assumption that a scanner between L and R
*always* holds a pin on at least one page.

**Mitigation needed:** Audit every code path in `_bt_steppage()` and
`_bt_readnextpage()` to confirm no path drops both pins simultaneously
(e.g., during `CHECK_FOR_INTERRUPTS()` or snapshot visibility checks).

### 6. No Progress Reporting or Statistics (Yet)
The command currently scans and merges silently.  DBAs need to know:
how many pages merged, how many skipped, how much space freed, how long
it took.

**Plan:** Add `NOTICE` messages at minimum.  `pg_stat` counters or a
`RETURNING`-style output are stretch goals for Phase 4.

### 7. Interaction with Concurrent DDL Is Unanalyzed
What happens if `REINDEX CONCURRENTLY` or `CREATE INDEX CONCURRENTLY`
runs on the same index during a merge?  Both modify index structure
with buffer-level locks.

**Likely answer:** Safe, because cleanup locks on leaf pages serialize
against any concurrent structural modification.  But this should be
documented explicitly and tested in isolation tests.

### 8. 12-Week Timeline Is Tight
Phase 1 (weeks 1–3) includes core merge function, WAL record, replay
function, WAL description, AND a basic test.  Writing correct WAL code
with FPI handling and crash testing is notoriously tricky for a first
attempt.

**Mitigation:** Consider budgeting 4 weeks for Phase 1 by borrowing
from Phase 4 (polish).  The WAL explainer document reduces ramp-up time,
but hands-on debugging always takes longer than expected.

---

## Risk Matrix

| Risk | Severity | Likelihood | Mitigation Status |
|------|----------|------------|-------------------|
| Pin gap concurrency | High | Medium | Needs formal analysis |
| Command interface naming | Low | High | Working title, community decides |
| Same-parent limiting merge rate | Medium | Low | GUCs allow tuning; benchmark after impl |
| Internal page bloat remains | Low | Certain | Acceptable — leaf is the real problem |
| Forward scan reprocessing | High | Low | Needs code path audit |
| No progress reporting | Medium | Certain | Planned for Phase 4 |
| Concurrent DDL interaction | Medium | Low | Likely safe; needs explicit testing |
| Tight timeline | Medium | Medium | Shift 1 week from Phase 4 to Phase 1 |

---

## Bottom Line

The design is **strong**.  The extension-only principle, deep reuse, and
L→R proof are exactly what the PostgreSQL community values.  The main
risks to address before implementation:

1. **Harden the pin-gap argument** — formal proof or LSN-check mitigation
2. **Finalize command syntax** — `FIX BLOAT INDEX` as working proposal
3. **Budget an extra week for Phase 1** — WAL code is hard

Everything else is either acceptable by design (leaf-only, same-parent)
or solvable during implementation (progress reporting, DDL interaction,
threshold tuning).
