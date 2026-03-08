# GSoC 2026 Proposal: B-tree Index Bloat Reduction (Page Merge)

**Candidate:** Ali Abdelrahman Sayed  
**Email:**   aliabdelrahman2005@gmail.com    
**GitHub:** https://github.com/Alielsalek1  
**Organization:** PostgreSQL  

---

## 1. Synopsis

PostgreSQL B-tree indexes bloat after heavy deletion. VACUUM recycles empty pages but not partially-filled ones, leaving indexes 4-5x larger than necessary. This proposal introduces callable functions for online page merging that reclaim space with no table locks and no disk overhead. The design reuses existing B-tree page-deletion and `BTP_HALF_DEAD` semantics, with targeted scanner-logic updates to handle merge/scan races safely.

## 2. Candidate Profile and Motivation

Final-year Computer Science student at Cairo University (Information Systems), focused on C, PostgreSQL internals, indexing, and concurrency. Relevant background includes systems-focused software engineering work and competitive programming (18th place in CPC Regional Finals, 200+ teams). After studying PostgreSQL B-tree internals and discussing design constraints with mentors, I am confident in delivering a correct merge implementation with targeted scanner changes.

**Availability:** ~30 hours/week. Brief interruptions for final exams (two weeks, mid-June) and graduation presentation (4 days, mid-July).

## 3. Benefits to Community

DBAs currently face two poor choices for fixing index bloat: `REINDEX` (heavy locks and I/O) or external tools like `pg_repack` (requires disk space). This project provides a native, online alternative that reclaims space and improves cache hit ratios, while keeping core modifications narrow and focused on scanner correctness during concurrent merges.

## 4. Related Work

**REINDEX:** `AccessExclusiveLock`, blocks all access.  
**REINDEX CONCURRENTLY:** Long duration, 2x disk space.  
**pg_repack:** External tool, complex setup, double space.  

## 5. Detailed Description & Approach

The core idea: merge a sparse "source" leaf page into its rightward "destination" sibling, reusing PostgreSQL's existing B-tree deletion infrastructure and extending scanner stepping logic for concurrent merge safety.

### Configuration Parameters

Two storage parameters (RELOPTIONS) control default merge behavior per-index:

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `btree_merge_low_threshold` | 10 | Minimum fill percentage (%) to trigger merge candidacy |
| `btree_merge_high_threshold` | 70 | Maximum fill percentage (%) result may not exceed |

Set via `CREATE INDEX ... WITH (btree_merge_low_threshold=10)` or `ALTER INDEX ... SET (...)`. Function parameters override RELOPTIONS defaults.

### Design Rationale

**1. Directional Constraint: Strict Left-to-Right Merging**

Merges are strictly left-to-right (L -> R). This preserves the rightward key-space movement expected by `_bt_moveright()` and avoids stale-pointer anomalies.

**2. Selection Strategy: Greedy Small-to-Large Merging**

Leaf scan is left-to-right with local pairwise merges: source <= `btree_merge_low_threshold`, merged page <= `btree_merge_high_threshold`. This minimizes movement, naturally allows chain merges, and avoids global planning complexity.

**3. Two-Phase Deletion: Atomic Merge with Half-Dead Marking**

Two-phase deletion is used: Phase 1 performs atomic merge and marks source as `BTP_HALF_DEAD`; Phase 2 unlinks and fully deletes the source via existing `_bt_unlink_halfdead_page()` logic. Reusing `BTP_HALF_DEAD` (already a built-in transition state in B-tree deletion) gives a reliable merge-in-progress signal without new page metadata or on-disk changes, keeping the design minimal and review risk low.

**WAL Logging Strategy:**   

- **Phase 1 WAL (`XLOG_BTREE_MERGE`)**: New WAL record logging atomic merge with full page images of parent, destination, and source (marked half-dead).
- **Phase 2 WAL (`XLOG_BTREE_UNLINK_PAGE`)**: Existing PostgreSQL WAL record for sibling chain unlink and deletion with safexid.

**Correctness:** Phase 1 atomicity via `XLOG_BTREE_MERGE`. Post-crash, VACUUM completes Phase 2 cleanup on half-dead pages.

**4. Parent Localization: Same-Parent Requirement**

Merges are limited to siblings under one parent. This avoids multi-parent lock/WAL complexity and keeps structural changes localized (one pivot removal, one downlink redirect).

**5. Minimal Core Changes: Scanner-Aware Concurrency Handling**

The implementation keeps core changes minimal and targeted. Merge and unlink operations reuse existing PostgreSQL B-tree deletion infrastructure, while scanner stepping logic is updated to detect merge transitions using existing `BTP_HALF_DEAD` signals and handle two race conditions: forward scans detecting duplicates and backward scans detecting missing data. Single-page reprocessing prevents both without introducing new on-disk metadata or changing B-tree invariants.
Stretch goal: if hackers review and testing show a safe path, reduce scanner touch points further while preserving the same correctness guarantees.

### Merge Algorithm: Operational Sequence

The operation runs in three phases.

**Phase 0: Tree Traversal and Bloat Detection**  

Traverse root -> leftmost leaf, then scan rightward. Source page eligibility is `fill <= low_threshold`; destination eligibility is `merged_fill <= high_threshold` (default: index `fillfactor`). Scanning continues rightward, enabling chain merges.

**Phase 1: Merge and Half-Dead Marking**  

For each eligible pair (L, R), validate parent/thresholds, then in one critical section: redirect parent downlink to R, remove separator, move tuples to R, mark L `BTP_HALF_DEAD`, and emit `XLOG_BTREE_MERGE`.

**Phase 2: Sibling Chain Unlink**  

Existing deletion code (`_bt_unlink_halfdead_page()`) unlinks L and logs `XLOG_BTREE_UNLINK_PAGE`. L becomes fully deleted with safexid; VACUUM recycles later when safe.

### User-Facing Interface: Callable Function

DBAs invoke merge operations through a per-index function with passable threshold parameters:

```sql
SELECT pg_btree_merge_pages('index_name', low_threshold := 10, high_threshold := 70);
```

The function acquires `ShareUpdateExclusiveLock` on the index (same lock level as VACUUM), allowing concurrent reads and writes while preventing concurrent merges or REINDEX on the same index. A table-level wrapper can be added later once the core function is proven.

**Concurrency Control:** Scanner-side detection and single-page reprocessing handle merge races. When a scanner crosses a merge boundary, it detects the transition via `BTP_HALF_DEAD` on the source page, discards its cached read data (`items[]`/page-position state) for that step, and re-reads the target page once. MVCC snapshot visibility rules remain unchanged: re-read pages are filtered by the scanner's original snapshot.

**Scanner Concurrency Handling (L1 / L2 / L3, three leaf siblings under the same parent):**

**Case 1: Forward Scan (Duplicate Prevention)**  
Scanner reads L1 and caches its tuples in `items[]`, then steps right to L2. Concurrently, a merge moves L1's tuples into L2 and marks L1 as `BTP_HALF_DEAD`. The scanner's cached L1 data is now stale, and L2 contains tuples already returned from L1.

*Detection:* After stepping L1 -> L2, re-check L1's page flags for `BTP_HALF_DEAD`.  
*Resolution:* Discard stale `items[]` and page-position state from the L1 read, re-read L2 fresh. The MVCC snapshot filters L2's contents, returning only unseen tuples. Scanning continues to L3 normally. Concurrent inserts/updates between the merge and re-read are handled by normal MVCC visibility; HOT chain redirects are unaffected because merge operates on index tuples, not heap tuples.

**Case 2: Backward Scan (Missing Data Prevention)**  
Scanner reads L2 and caches its tuples, then steps left to L1. Concurrently, a merge moves L1's tuples into L2 and marks L1 as `BTP_HALF_DEAD`. The scanner lands on a half-dead page with no valid tuples and would miss data now stored in L2.

*Detection:* After stepping L2 -> L1, check current page (L1) for `BTP_HALF_DEAD`.  
*Resolution:* Discard stale cached data for the L1 step, step back to L2, re-read L2 once. The MVCC snapshot ensures only unseen merged tuples are returned. Scanning continues leftward past L1 (skipping the half-dead page) normally.

**Pre-lock re-validation:** Between eligibility check and lock acquisition, concurrent activity may modify pages. After acquiring exclusive buffer locks, the merge re-validates fill thresholds and parent relationships. If conditions no longer hold (e.g., L1 now exceeds `low_threshold`, or L2 split), the merge skips this pair and advances.

These checks reuse existing `BTP_HALF_DEAD` semantics and add targeted logic around `_bt_steppage()` transitions, complementing existing reverse-scan split handling in `_bt_walk_left()`.

### VACUUM Interaction and Coordination

The merge is designed to work cooperatively with VACUUM:

- **Shared cleanup path:** If a merge crashes after Phase 1, VACUUM discovers the half-dead source page during its normal index scan and completes the unlink via `_bt_unlink_halfdead_page()`. The source page is recycled only after `safexid` is no longer visible to any transaction.
- **Lock serialization:** Both merge and VACUUM acquire `ShareUpdateExclusiveLock`, so they cannot run simultaneously on the same index. This avoids conflicts over concurrent structural changes.
- **Dead tuple handling:** Merge moves all index tuples (including dead ones) from source to destination. Dead tuples remain invisible via MVCC and are cleaned by VACUUM on the destination page during its next pass.

## 6. Implementation Deliverables and Timeline

| Weeks | Milestone | Deliverables |
|-------|-----------|-------------|
| 1 | Planning & Design | Finalize design, risks, test plan |
| 2-5 | Core Implementation | Merge execution, scanner reprocessing, callable function, structural integrity checks |
| 6-9 | Durability & Recovery | WAL + recovery support, crash tests, RELOPTIONS defaults, hackers discussion draft |
| 10-12 | Testing & Release | Isolation/performance tests, patchset polish |

**Final Artifacts:** Core merge logic, `XLOG_BTREE_MERGE` WAL record, `pg_btree_merge_pages()` function with RELOPTIONS, SGML documentation, and comprehensive test suite (regression, crash recovery, concurrency, VACUUM interaction). 

## 7. Quality Assurance and Verification

All tests use `amcheck` for structural validation after every merge and `isolationtester` for concurrency scenarios.

**Structural Integrity:** Verify parent/child downlinks, sibling chain continuity, key ordering, and tuple re-findability after single-pair and chain merges. Boundary tests: skip when destination is near `high_threshold`; reject cross-parent merges.

**Concurrency Safety (isolation tests with concurrent sessions):**

- Forward scan + merge: Session A scans L1->L2->L3 while Session B merges L1 into L2. Verify no duplicates and no omissions.
- Backward scan + merge: Session A scans L3->L2->L1 while Session B merges L1 into L2. Verify A detects half-dead L1, re-reads L2, returns all visible tuples.
- Concurrent inserts/deletes/updates during merge: Verify MVCC visibility correctness, HOT chain integrity, and that merge either completes or skips after re-validation.
- Pre-lock race: Rapidly modify eligible pages between eligibility check and lock acquisition. Verify merge re-validates and skips stale candidates.
- MVCC snapshot: Run merge inside a long-running transaction. Verify scanner snapshot determines visibility, not merge timing.
- Chain merges with concurrent splits: Handle splits in destination page during chained merge operations; verify merge re-validates after split detection.

**VACUUM Interaction:**

- Kill merge after Phase 1. Run VACUUM and verify it completes half-dead cleanup via `_bt_unlink_halfdead_page()`.
- Start merge and VACUUM concurrently. Verify `ShareUpdateExclusiveLock` serializes them.
- Merge pages with dead tuples. Verify VACUUM cleans destination and recycles source.

**Recovery:** Crash tests at three points: before WAL flush (both pages unchanged), after Phase 1 WAL (VACUUM completes Phase 2), and during Phase 2 unlink (recovery replays `XLOG_BTREE_UNLINK_PAGE`).

**Evaluation Metrics:** Index size reduction (target: proportional to merged pages), scan time improvement, merge throughput (pages/sec), query latency impact during concurrent merges, and merge overhead on VACUUM (verify VACUUM completion time does not degrade significantly post-merge).
