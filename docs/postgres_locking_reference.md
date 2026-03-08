# PostgreSQL Locking — A Reference for B-Tree Development

> Quick reference for every type of lock in PostgreSQL, when each is used,
> and which ones our page merge uses (and why).
>
> This is a study companion — not an original document.  Everything here
> comes from the PostgreSQL source code, the nbtree README, and the
> buffer manager comments in `bufmgr.c`.

---

## Table of Contents

1. [The Three Lock Layers](#1-the-three-lock-layers)
2. [Layer 1: Relation-Level Locks](#2-layer-1-relation-level-locks)
3. [Layer 2: Buffer Content Locks (Page Locks)](#3-layer-2-buffer-content-locks-page-locks)
4. [Layer 3: Heavyweight Relation Locks vs. Lightweight Locks](#4-layer-3-heavyweight-relation-locks-vs-lightweight-locks)
5. [Pins — Not a Lock, But Critical](#5-pins--not-a-lock-but-critical)
6. [Cleanup Locks — The Special One](#6-cleanup-locks--the-special-one)
7. [Predicate Locks (SSI)](#7-predicate-locks-ssi)
8. [nbtree's Locking Rules](#8-nbtrees-locking-rules)
9. [What Our Page Merge Uses](#9-what-our-page-merge-uses)
10. [Lock Ordering Rules and Deadlock Prevention](#10-lock-ordering-rules-and-deadlock-prevention)
11. [Common Mistakes to Avoid](#11-common-mistakes-to-avoid)
12. [Quick Reference Card](#12-quick-reference-card)

---

## 1. The Three Lock Layers

PostgreSQL has three distinct locking mechanisms.  They are completely
independent systems that protect different things:

```
 ┌──────────────────────────────────────────────────────────────┐
 │ Layer 1: Relation-Level Locks (heavyweight)                  │
 │   Protects: entire tables/indexes against conflicting DDL    │
 │   Granularity: whole relation                                │
 │   Duration: held for entire transaction                      │
 │   Source: lockdefs.h, lock.h                                 │
 ├──────────────────────────────────────────────────────────────┤
 │ Layer 2: Buffer Content Locks (lightweight, per-page)        │
 │   Protects: in-memory page content against concurrent reads  │
 │             and writes                                       │
 │   Granularity: single 8KB buffer/page                        │
 │   Duration: held only while examining/modifying the page     │
 │   Source: bufmgr.h, bufmgr.c                                 │
 ├──────────────────────────────────────────────────────────────┤
 │ Layer 3: Lightweight Locks (LWLocks)                         │
 │   Protects: shared memory data structures                    │
 │   Granularity: individual shared memory structures           │
 │   Duration: very short (microseconds)                        │
 │   Source: lwlock.h                                           │
 └──────────────────────────────────────────────────────────────┘
```

For B-tree work, we mostly deal with **Layer 1** (relation locks on the
index) and **Layer 2** (buffer locks on individual pages).  LWLocks are
infrastructure we don't interact with directly.

---

## 2. Layer 1: Relation-Level Locks

Defined in `src/include/storage/lockdefs.h`.  These are the classic
PostgreSQL lock modes, numbered 1–8.  They protect entire relations
(tables, indexes) against conflicting operations.

### The 8 Lock Modes

```c
#define NoLock                    0  // not a lock — flag for "don't get a lock"
#define AccessShareLock           1  // SELECT
#define RowShareLock              2  // SELECT FOR UPDATE/FOR SHARE
#define RowExclusiveLock          3  // INSERT, UPDATE, DELETE
#define ShareUpdateExclusiveLock  4  // VACUUM (non-FULL), ANALYZE,
                                    // CREATE INDEX CONCURRENTLY
#define ShareLock                 5  // CREATE INDEX (without CONCURRENTLY)
#define ShareRowExclusiveLock     6  // like EXCLUSIVE, but allows ROW SHARE
#define ExclusiveLock             7  // blocks ROW SHARE/SELECT...FOR UPDATE
#define AccessExclusiveLock       8  // ALTER TABLE, DROP TABLE, VACUUM FULL,
                                    // unqualified LOCK TABLE
```

### Conflict Matrix

Two operations conflict if their lock modes have an "X" at the
intersection:

```
              AS   RS   RE   SUE   S   SRE   E   AE
 AS (1)        .    .    .    .    .    .    .    X
 RS (2)        .    .    .    .    .    .    X    X
 RE (3)        .    .    .    .    X    X    X    X
 SUE (4)       .    .    .    X    X    X    X    X
 S (5)         .    .    X    X    .    X    X    X
 SRE (6)       .    .    X    X    X    X    X    X
 E (7)         .    X    X    X    X    X    X    X
 AE (8)        X    X    X    X    X    X    X    X
```

Key relationships:
- `AccessShareLock` (SELECT) conflicts only with `AccessExclusiveLock`
  (DROP TABLE).  Selects and DML coexist freely.
- `ShareUpdateExclusiveLock` (VACUUM) conflicts with itself — two
  VACUUMs on the same table can't run concurrently.
- `ShareLock` (CREATE INDEX) blocks writes (RowExclusiveLock) but allows
  reads.
- `AccessExclusiveLock` blocks everything — total exclusive access.

### Which Lock Does Our Merge Need?

**For the `FIX BLOAT` command, we need a relation-level lock on the
index.**  The question is which level.

| Option | Lock Mode | What It Blocks | What It Allows |
|--------|-----------|----------------|----------------|
| A | `AccessShareLock` (1) | Only DROP INDEX | Everything — concurrent DML, VACUUM, other readers |
| B | `ShareUpdateExclusiveLock` (4) | Another FIX BLOAT, VACUUM | DML, reads |
| C | `ShareLock` (5) | Writes (INSERT/UPDATE/DELETE) | Reads |
| D | `AccessExclusiveLock` (8) | Everything | Nothing |

**Recommended: Option A or B.**

- We don't need to block DML — our buffer-level cleanup locks already
  serialize page-level access.
- We might want to block concurrent `FIX BLOAT` on the same index
  (Option B), to avoid redundant work.
- We should NOT use `ShareLock` or `AccessExclusiveLock` — that would
  be worse than `REINDEX` for concurrency, defeating the purpose.

**Decision for page merge:** `ShareUpdateExclusiveLock` on the index
relation (same as VACUUM uses).  This prevents two `FIX BLOAT` commands
from running on the same index simultaneously, while allowing all DML
and reads to continue.

---

## 3. Layer 2: Buffer Content Locks (Page Locks)

Defined in `src/include/storage/bufmgr.h`.  These protect the in-memory
content of a single 8KB buffer page.  They are the primary locking
mechanism for B-tree page operations.

### The 4 Modes

```c
typedef enum BufferLockMode
{
    BUFFER_LOCK_UNLOCK,           // release the lock
    BUFFER_LOCK_SHARE,            // read lock — multiple readers OK
    BUFFER_LOCK_SHARE_EXCLUSIVE,  // conflicts with itself and exclusive
    BUFFER_LOCK_EXCLUSIVE,        // write lock — exclusive access
} BufferLockMode;
```

In nbtree, these are aliased:

```c
#define BT_READ   BUFFER_LOCK_SHARE      // read a page
#define BT_WRITE  BUFFER_LOCK_EXCLUSIVE  // modify a page
```

### Usage in nbtree

| Operation | Lock | Why |
|-----------|------|-----|
| `_bt_search()` descent | `BT_READ` on each page | Only reading — checking keys to find correct child |
| `_bt_moveright()` | `BT_READ` → release → `BT_READ` on right sibling | Moving right after landing on wrong page due to split |
| `_bt_findinsertloc()` | Upgrades to `BT_WRITE` on target leaf | About to insert a tuple |
| `_bt_split()` | `BT_WRITE` on original page + new page + parent | Structural modification — adding new page |
| `_bt_mark_page_halfdead()` | `BT_WRITE` on target + parent | Structural modification — removing downlink |
| `_bt_unlink_halfdead_page()` | `BT_WRITE` on left sib, target, right sib | Modifying sibling chain |
| VACUUM `_bt_delitems_vacuum()` | **Cleanup lock** on page | Deleting items — need to ensure no pins |

### How to Use Them

```c
// nbtree uses wrapper functions, NOT raw LockBuffer():

// Pin + lock in one call:
Buffer buf = _bt_getbuf(rel, blkno, BT_READ);  // or BT_WRITE

// Lock an already-pinned buffer:
_bt_lockbuf(rel, buf, BT_READ);   // or BT_WRITE

// Unlock (but keep pin):
_bt_unlockbuf(rel, buf);

// Release pin and acquire different page:
buf = _bt_relandgetbuf(rel, buf, new_blkno, BT_READ);

// Conditional lock (don't block):
bool got_it = _bt_conditionallockbuf(rel, buf);

// Upgrade to cleanup lock (see Section 6):
_bt_upgradelockbufcleanup(rel, buf);
```

**Important:** In nbtree, always use `_bt_lockbuf()` / `_bt_unlockbuf()`
instead of raw `LockBuffer()`.  The wrappers add Valgrind annotations
that help detect unsafe page accesses during development.

### Lock Duration

Buffer locks are held only while actively examining or modifying a page.
They are **not** held across I/O waits on other pages.  The typical
pattern is:

```
Lock page A → read/modify A → unlock A → lock page B → ...
```

The one exception: when moving right or up in the tree, you can lock
the next page before releasing the current one (to avoid losing your
place).  But you must NEVER lock left or down while holding a lock —
that creates deadlock risk.

---

## 4. Layer 3: Heavyweight Relation Locks vs. Lightweight Locks

### Lightweight Locks (LWLocks)

LWLocks protect shared memory data structures (buffer descriptors,
shared hash tables, WAL buffers, etc.).  They are ultra-short-duration
and don't support deadlock detection.

```c
LWLockAcquire(lock, LW_SHARED);    // or LW_EXCLUSIVE
// ... access shared data structure ...
LWLockRelease(lock);
```

**We don't interact with LWLocks directly in B-tree code.**  They're
used internally by the buffer manager and other infrastructure.

---

## 5. Pins — Not a Lock, But Critical

A **pin** is a reference count on a buffer.  It prevents the buffer
manager from evicting the page from shared buffers.  A pin does NOT
prevent other backends from reading or modifying the page.

```c
// Pin is acquired implicitly by ReadBuffer():
Buffer buf = ReadBuffer(rel, blkno);  // pin count +1

// Pin is released by:
ReleaseBuffer(buf);                   // pin count -1
```

### Why Pins Matter for Merge

A backend can hold a pin on a page **without holding any lock**.  This
happens between `_bt_readpage()` calls in an index scan — the scan
returns tuples to the executor, which processes them without holding the
buffer lock but while keeping the pin.

This means:
- A `BT_WRITE` lock does NOT guarantee you're the only one with access
  to the page.  Someone might be holding a pin from an earlier read.
- A **cleanup lock** (`LockBufferForCleanup()`) waits until the pin
  count drops to 1 (only our pin remains).  This is the only way to
  guarantee exclusive physical access.

### The Pin Gap

When a scan moves from page L to page R, there is a brief window where
it holds zero pins (after releasing L, before pinning R).  During this
window, the page content could change completely.  This is why scans
re-check page state after acquiring a new lock.

---

## 6. Cleanup Locks — The Special One

### What a Cleanup Lock Is

A cleanup lock is `BUFFER_LOCK_EXCLUSIVE` + "wait until pin count = 1".
It is NOT a separate lock mode — it's an exclusive lock with an
additional guarantee about pins.

```c
// In bufmgr.c:
void LockBufferForCleanup(Buffer buffer)
{
    // 1. Acquire exclusive lock
    // 2. Check pin count
    // 3. If pin count > 1, release lock, wait, retry
    // 4. Return only when exclusive lock held AND pin count == 1
}
```

### When to Use Cleanup Locks

**Rule:** Any operation that modifies page content in a way that could
confuse a concurrent pin-holder needs a cleanup lock.

| Operation | Lock Needed | Why |
|-----------|------------|-----|
| Reading a page | `BT_READ` | Just looking |
| Inserting a tuple | `BT_WRITE` | New tuple at end of page — existing pin-holders already read past it |
| Splitting a page | `BT_WRITE` | New page gets the moved tuples; old page keeps its content valid for pin-holders via `_bt_moveright()` |
| Deleting tuples (VACUUM) | **Cleanup lock** | Removing tuples that a pin-holder might be pointing at |
| Deleting a page | **Cleanup lock** (on page being deleted) | Entire page content is destroyed |
| **Merging pages** | **Cleanup lock** (on both L and R) | L's content is destroyed, R's content is rebuilt — both changes affect pin-holders |

### Why Cleanup Lock on Both Pages for Merge

Our merge rebuilds R's content (prepends L's tuples) and destroys L.
Without cleanup locks:

- **A pin-holder on L** might have tuple pointers into L's content.
  After the merge, those pointers point to garbage or to R's tuples
  at wrong offsets.
- **A pin-holder on R** might have read some tuples from R.  After the
  merge, R's item offsets have all shifted (L's tuples were prepended).
  The pin-holder's `item[3]` pointer now points to a different tuple.
- **A scanner between L and R** (pin on L, about to move to R) would
  miss the tuples moved from L to R, or see them twice.

Cleanup locks guarantee that no scanner is mid-read on either page.

### Cleanup Lock API

```c
// Block until cleanup lock acquired:
LockBufferForCleanup(buf);
// This loops internally until pin_count == 1.
// Can block indefinitely if another backend keeps the page pinned.

// Try without blocking (returns true/false):
bool got_it = ConditionalLockBufferForCleanup(buf);
// Returns false immediately if pin_count > 1.
// Useful for "skip if busy" strategies.

// Upgrade existing BT_WRITE lock to cleanup lock:
_bt_upgradelockbufcleanup(rel, buf);
// Releases BT_WRITE, then calls LockBufferForCleanup().
// WARNING: page content may have changed between unlock and re-lock!
```

### `ConditionalLockBufferForCleanup` vs. `LockBufferForCleanup`

| | `LockBufferForCleanup` | `ConditionalLockBufferForCleanup` |
|--|------------------------|----------------------------------|
| Blocking | Yes — waits until pin_count = 1 | No — returns false immediately |
| Use when | You MUST have the lock (page delete, VACUUM) | You can skip this page and try later |
| Risk | Indefinite wait if scanner holds pin | None — but you may skip pages |
| Used by | `_bt_pagedel()`, VACUUM | Not commonly used in nbtree |

**For our merge:** We use `LockBufferForCleanup()` because we need
exclusive access to both pages.  If a page is busy, we wait.
Alternatively, we could use `ConditionalLockBufferForCleanup()` and
skip busy pages — this trades thoroughness for lower latency.

---

## 7. Predicate Locks (SSI)

Predicate locks support Serializable Snapshot Isolation (SSI).  They
track which tuples a transaction has *read*, so PostgreSQL can detect
read-write conflicts.

### What They Are

Predicate locks are NOT mutual exclusion locks.  They are "markers" that
record "transaction T has read data in page/index range X."  If another
transaction writes to X, the SSI system can detect the conflict and
abort one of the transactions.

### Why They Matter for Merge

When we merge page L into page R, we need to transfer L's predicate
locks to R.  If we don't, a serializable transaction that read from L
won't know that its data moved to R, and the SSI conflict detection
will miss a potential write-skew anomaly.

```c
// After moving L's tuples to R:
PredicateLockPageCombine(rel, L_blkno, R_blkno);
```

This is the same call that `_bt_mark_page_halfdead()` uses when
deleting a page.  We reuse it as-is.

---

## 8. nbtree's Locking Rules

The nbtree README (src/backend/access/nbtree/README) establishes these
rules.  Our merge must follow all of them.

### Rule 1: Always Hold Pin + Lock Together

> "The general rule in nbtree is that it's never okay to access a page
> without holding both a buffer pin and a buffer lock on the page's
> buffer." — nbtpage.c line 833

### Rule 2: Lock Right, Never Left (for deadlock prevention)

> "It is safe to lock the next page before releasing the current one
> when moving right or up, but not when moving left or down (else we'd
> create the possibility of deadlocks)." — README

This means:
- ✅ Lock L, then lock R (moving right) — SAFE
- ✅ Lock child, then lock parent (moving up) — SAFE
- ❌ Lock R, then lock L (moving left) — DEADLOCK RISK
- ❌ Lock parent, then lock child (moving down) — DEADLOCK RISK

### Rule 3: Use nbtree Wrappers, Not Raw LockBuffer

> "Raw LockBuffer() calls are disallowed in nbtree; all buffer lock
> requests need to go through wrapper functions such as _bt_lockbuf()."
> — nbtpage.c line 841

### Rule 4: Read Locks Are Short

> "Page read locks are held only for as long as a scan is examining a
> page." — README

### Rule 5: Key Space Moves Right

> "The target page's key space effectively belongs to its right sibling."
> — README (on page deletion)

After any structural operation (split, delete, merge), data moves right.
Scans that land on a stale page use `_bt_moveright()` to find the data.

### Rule 6: Same-Parent Constraint for Key Space Transfer

> "We cannot merge the key space of a page into its right sibling unless
> the right sibling is a child of the same parent." — README line 273

This is why our merge enforces the same-parent constraint.

---

## 9. What Our Page Merge Uses

### Lock Inventory

```
 1. Relation-level: ShareUpdateExclusiveLock on the index
    Acquired: once, at the start of FIX BLOAT
    Released: at end of command
    Purpose: prevent concurrent FIX BLOAT / VACUUM on same index

 2. Buffer lock: Cleanup lock on L (source page)
    Acquired: via LockBufferForCleanup()
    Released: after Phase 1 (merge + half-dead)
    Purpose: drain all pins — no scanner is reading L

 3. Buffer lock: Cleanup lock on R (destination page)
    Acquired: via LockBufferForCleanup() AFTER locking L
    Released: after Phase 1
    Purpose: drain all pins — no scanner is reading R

 4. Buffer lock: BT_WRITE on Parent
    Acquired: via _bt_lockbuf(rel, parent_buf, BT_WRITE)
    Released: after Phase 1
    Purpose: update downlink + delete pivot

 5. Predicate lock transfer: PredicateLockPageCombine(L → R)
    Called: during Phase 1, inside the critical section
    Purpose: SSI correctness

 6. Buffer locks for Phase 2 (_bt_unlink_halfdead_page):
    BT_WRITE on left sibling, L, right sibling
    (handled entirely by existing code — we don't touch this)
```

### Lock Acquisition Order

```
 Step 1: L cleanup lock          (leftmost leaf page)
 Step 2: R cleanup lock          (right sibling — moving right: SAFE)
 Step 3: Parent BT_WRITE         (moving up: SAFE)
 Step 4: ... critical section ...
 Step 5: Release all three
 Step 6: _bt_unlink_halfdead_page() acquires its own locks
```

This follows nbtree's deadlock prevention rules:
- L before R = left-to-right ✅
- Leaves before parent = bottom-to-top ✅
- No lock-left, no lock-down ✅

### Why Cleanup Lock, Not Just BT_WRITE?

| Scenario | BT_WRITE only | Cleanup lock |
|----------|---------------|--------------|
| Scanner reading L's tuples (holds pin, no lock) | We proceed — scanner has stale pointers → crash or wrong data | We wait until scanner releases pin → safe |
| Scanner between L and R (no pin, no lock) | We proceed — scanner might see duplicates or miss data | We proceed — scanner will re-read R and see merged data |
| Scanner holding lock on L | We block (BT_WRITE waits for BT_READ) | We block (cleanup lock also waits for any lock) |

The cleanup lock catches the first case, which BT_WRITE misses.  This
is the same reason VACUUM uses cleanup locks for tuple deletion and
`_bt_pagedel()` uses them for page deletion.

---

## 10. Lock Ordering Rules and Deadlock Prevention

### The Golden Rules

1. **Between pages on the same level:** always lock left-to-right.
2. **Between levels:** always lock child first, then parent (bottom-up).
3. **Never hold a page lock while acquiring a lock on a page to the
   left or below.**
4. **Relation-level locks are acquired before any buffer locks.**
5. **Within a critical section, no lock acquisition that could block.**
   (All locks are acquired before `START_CRIT_SECTION()`.)

### How Our Merge Follows Them

```
 FIX BLOAT INDEX idx;
   │
   ├── Acquire ShareUpdateExclusiveLock on idx         [Rule 4: relation first]
   │
   ├── For each candidate page L:
   │     ├── LockBufferForCleanup(L)                   [Rule 1: leftmost first]
   │     ├── LockBufferForCleanup(R)                   [Rule 1: moving right]
   │     ├── _bt_lockbuf(parent, BT_WRITE)             [Rule 2: bottom-up]
   │     ├── START_CRIT_SECTION()                      [Rule 5: all locks held]
   │     │     ├── Move tuples L → R
   │     │     ├── Update parent pivot
   │     │     ├── Mark L half-dead
   │     │     ├── PredicateLockPageCombine(L, R)
   │     │     ├── WAL log
   │     ├── END_CRIT_SECTION()
   │     ├── Release all three locks
   │     ├── _bt_unlink_halfdead_page()                [Phase 2]
   │
   └── Release ShareUpdateExclusiveLock
```

---

## 11. Common Mistakes to Avoid

### Mistake 1: Using BT_WRITE When Cleanup Lock Is Needed

```c
// WRONG — pin-holders can still access the page:
_bt_lockbuf(rel, buf, BT_WRITE);
// ... modify page content that pin-holders might reference ...

// RIGHT — waits until all pin-holders release:
LockBufferForCleanup(buf);
// ... now safe to modify page content ...
```

### Mistake 2: Locking Right-to-Left

```c
// WRONG — deadlock risk:
_bt_lockbuf(rel, right_buf, BT_WRITE);
_bt_lockbuf(rel, left_buf, BT_WRITE);   // another backend might hold
                                         // left and wait for right

// RIGHT:
_bt_lockbuf(rel, left_buf, BT_WRITE);
_bt_lockbuf(rel, right_buf, BT_WRITE);
```

### Mistake 3: Locking Top-Down

```c
// WRONG — deadlock risk:
_bt_lockbuf(rel, parent_buf, BT_WRITE);
_bt_lockbuf(rel, child_buf, BT_WRITE);  // inserter might hold child
                                         // and need parent for split

// RIGHT:
_bt_lockbuf(rel, child_buf, BT_WRITE);  // or cleanup lock
_bt_lockbuf(rel, parent_buf, BT_WRITE);
```

### Mistake 4: Using Raw LockBuffer in nbtree

```c
// WRONG — misses Valgrind annotations:
LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

// RIGHT — uses nbtree wrapper:
_bt_lockbuf(rel, buf, BT_WRITE);
```

### Mistake 5: Acquiring Locks Inside Critical Sections

```c
// WRONG — lock acquisition can block, and blocking inside a critical
// section means WAL could be left in an inconsistent state:
START_CRIT_SECTION();
_bt_lockbuf(rel, some_buf, BT_WRITE);  // might block!
// ... WAL log ...
END_CRIT_SECTION();

// RIGHT:
_bt_lockbuf(rel, some_buf, BT_WRITE);  // block here is fine
START_CRIT_SECTION();
// ... modify pages, WAL log ...
END_CRIT_SECTION();
```

### Mistake 6: Forgetting Predicate Lock Transfer

```c
// WRONG — SSI won't detect conflicts for tuples that moved:
// ... move tuples from L to R ...
// (forgot PredicateLockPageCombine)

// RIGHT:
PredicateLockPageCombine(rel, L_blkno, R_blkno);
```

---

## 12. Quick Reference Card

### Relation-Level Locks (for the index)

| Lock | Number | Our Use |
|------|--------|---------|
| `AccessShareLock` | 1 | Considered — too permissive (allows concurrent FIX BLOAT) |
| `ShareUpdateExclusiveLock` | 4 | **Our choice** — blocks concurrent FIX BLOAT & VACUUM |
| `ShareLock` | 5 | Too restrictive — blocks all DML |
| `AccessExclusiveLock` | 8 | Never — worse than REINDEX |

### Buffer Locks (for individual pages)

| Lock | When | Duration |
|------|------|----------|
| `BT_READ` | Reading page content | While examining page |
| `BT_WRITE` | Inserting tuple, updating sibling links | While modifying page |
| Cleanup lock | Deleting tuples, deleting pages, **merging pages** | While modifying + short wait for pin drain |

### Lock Ordering

```
Relation lock → Buffer locks (left-to-right, bottom-to-top)
```

### Function Reference

| Function | What It Does |
|----------|-------------|
| `_bt_getbuf(rel, blkno, BT_READ)` | Pin + read lock |
| `_bt_getbuf(rel, blkno, BT_WRITE)` | Pin + write lock |
| `_bt_lockbuf(rel, buf, mode)` | Lock already-pinned buffer |
| `_bt_unlockbuf(rel, buf)` | Unlock (keep pin) |
| `_bt_relandgetbuf(rel, buf, blkno, mode)` | Release old, pin+lock new |
| `_bt_conditionallockbuf(rel, buf)` | Try write lock, return false if busy |
| `_bt_upgradelockbufcleanup(rel, buf)` | Upgrade to cleanup lock (page may change!) |
| `LockBufferForCleanup(buf)` | Exclusive lock + wait for pin_count=1 |
| `ConditionalLockBufferForCleanup(buf)` | Try cleanup lock, return false if pins > 1 |
| `PredicateLockPageCombine(rel, from, to)` | Transfer SSI predicate locks |

---

*This document covers every locking concept our page merge needs.
For WAL mechanics, see `btree_wal_explainer.md`.
For B-tree structural internals, see `btree_internals.md`.
For the full implementation plan, see `btree_page_merge_plan.md`.*
