# PostgreSQL Buffer Manager & Pin Semantics

> Reference for how PostgreSQL manages shared buffers, buffer pins,
> and the read/write pipeline for on-disk pages.  Essential background
> for any code that touches B-tree pages.

---

## Table of Contents

1. [Overview: From Disk to Memory](#1-overview-from-disk-to-memory)
2. [Shared Buffers Architecture](#2-shared-buffers-architecture)
3. [Buffer Pins — The Reference Count](#3-buffer-pins--the-reference-count)
4. [The Read Path: ReadBuffer](#4-the-read-path-readbuffer)
5. [The Write Path: MarkBufferDirty](#5-the-write-path-markbufferdirty)
6. [Buffer Locks vs. Pins — The Distinction](#6-buffer-locks-vs-pins--the-distinction)
7. [The Pin Gap Problem](#7-the-pin-gap-problem)
8. [Buffer Replacement (Clock Sweep)](#8-buffer-replacement-clock-sweep)
9. [Critical Sections](#9-critical-sections)
10. [WAL and Buffer Interaction](#10-wal-and-buffer-interaction)
11. [Relevance to Page Merge](#11-relevance-to-page-merge)
12. [Function Reference](#12-function-reference)

---

## 1. Overview: From Disk to Memory

PostgreSQL never reads or writes disk pages directly.  All page access
goes through the **shared buffer pool** — a fixed-size region of shared
memory that caches recently used pages.

```
 Backend process                 Shared Memory               Disk
 ┌──────────┐                   ┌──────────────┐        ┌──────────┐
 │ _bt_getbuf│──── ReadBuffer ─→│ Buffer Pool  │←─────→ │ Data file│
 │           │←── returns buf ──│ (shared_buf) │  bgwriter/  │
 │ work with │                  │              │  checkpointer│
 │ page in   │                  │ [page][page] │        └──────────┘
 │ memory    │                  │ [page][page] │
 └──────────┘                   └──────────────┘
```

Key facts:
- Buffer pool size = `shared_buffers` GUC (default 128 MB).
- Each buffer slot holds one 8 KB page (`BLCKSZ`).
- Multiple backends can access the same buffer simultaneously (with
  appropriate locking).
- Dirty buffers are written back to disk by the background writer
  (`bgwriter`) or checkpointer — NOT by the backend that modified them
  (usually).

---

## 2. Shared Buffers Architecture

Each buffer in the pool has two parts:

### Buffer Descriptor (metadata)

```c
typedef struct BufferDesc
{
    BufferTag   tag;           // which relation + fork + block this buffer holds
    int         buf_id;        // index in the buffer array
    pg_atomic_uint32 state;    // pin count + flags (dirty, valid, locked)
    int         wait_backend_pgprocno;  // for cleanup lock waiting
    forknum     forkNum;       // MAIN_FORKNUM, FSM_FORKNUM, etc.
    ...
} BufferDesc;
```

The `state` field is an atomic 32-bit integer that packs:
- **Pin count** (reference count) — bits 18–31
- **Usage count** (for clock sweep eviction) — bits 14–17
- **Flags**: BM_DIRTY, BM_VALID, BM_TAG_VALID, BM_IO_IN_PROGRESS, BM_LOCKED

### Buffer Content (the actual page)

The actual 8 KB page data, stored in a separate shared memory array.
Accessed via `BufferGetPage(buf)`.

---

## 3. Buffer Pins — The Reference Count

A **pin** is a reference count that tells the buffer manager:
"I'm using this buffer — don't evict it."

### Acquiring a Pin

```c
Buffer buf = ReadBuffer(rel, blockno);
// Pin count is now incremented.  The page is guaranteed to remain in
// shared buffers until we release the pin.
```

### Releasing a Pin

```c
ReleaseBuffer(buf);
// Pin count decremented.  If pin count reaches 0 and the buffer is
// needed for another page, it can be evicted.
```

### What a Pin Does NOT Do

- A pin does NOT prevent other backends from reading the page.
- A pin does NOT prevent other backends from **modifying** the page
  (they still need a lock, but pins don't stop them).
- A pin only prevents **eviction** — the page stays in memory.

### Why Pins Exist

Without pins, a page could be evicted while a backend is still looking
at it.  The buffer content would be replaced with a completely different
page, and the backend would read garbage.

### Pin Count and Cleanup Locks

`LockBufferForCleanup()` waits until pin_count = 1 (only the caller's
pin).  This is the only way to guarantee that no other backend has
**any** access to the buffer content — not even a stale pointer from
a previous read.

---

## 4. The Read Path: ReadBuffer

When a backend wants to read a page:

```
ReadBuffer(rel, blockno)
  │
  ├── 1. Hash lookup: is (rel, fork, blockno) already in buffer pool?
  │     YES → increment pin count → return buffer
  │     NO  → continue
  │
  ├── 2. Find a free buffer (clock sweep eviction if needed)
  │     - Write back dirty buffer if evicting one
  │
  ├── 3. Read page from disk into buffer
  │
  ├── 4. Set buffer tag, mark valid, increment pin count
  │
  └── 5. Return buffer
```

After `ReadBuffer()` returns, the backend has a **pinned** buffer but
**no lock**.  It must call `LockBuffer()` (or `_bt_lockbuf()` in nbtree)
before accessing the page content.

---

## 5. The Write Path: MarkBufferDirty

Modifying a page is a multi-step process:

```
1. Pin the buffer (ReadBuffer or already pinned)
2. Lock the buffer (BUFFER_LOCK_EXCLUSIVE)
3. Modify the page content in shared memory
4. Call MarkBufferDirty(buf)  — sets BM_DIRTY flag
5. Unlock the buffer
6. (Later, release pin)
```

The actual disk write happens asynchronously:
- The **bgwriter** periodically scans for dirty buffers and writes them.
- The **checkpointer** writes all dirty buffers at checkpoint time.
- The backend itself might write if it needs to evict a dirty buffer to
  make room (but this is not the common path).

### Important: MarkBufferDirty Must Be Called Inside Critical Section

If the modification is WAL-logged (and all B-tree modifications are),
the pattern is:

```c
START_CRIT_SECTION();
// Modify page content
MarkBufferDirty(buf);
// Write WAL record
XLogInsert(RM_BTREE_ID, XLOG_BTREE_MERGE);
// Set LSN on page
PageSetLSN(page, lsn);
END_CRIT_SECTION();
```

---

## 6. Buffer Locks vs. Pins — The Distinction

| Property | Pin | Buffer Lock |
|----------|-----|-------------|
| What it prevents | Eviction | Concurrent read/write |
| Can coexist with other pins | Yes (unlimited) | Depends on mode |
| Can coexist with locks | Yes (pin + any lock) | Shared: multiple; Exclusive: one |
| Duration | Can span multiple operations | Should be short |
| Acquired by | `ReadBuffer()` | `LockBuffer()` / `_bt_lockbuf()` |
| Released by | `ReleaseBuffer()` | `UnlockBuffer()` / `_bt_unlockbuf()` |

### The Dangerous Case: Pin Without Lock

A backend can hold a pin on a buffer **without holding a lock**.  This
happens in index scans between `_bt_readpage()` calls:

```
1. Lock page A (BT_READ)
2. Read tuples from page A into scan state
3. Unlock page A (but KEEP PIN)
4. Return tuples to executor
5. Executor processes tuples (pin still held, no lock)
6. Executor asks for more tuples → re-lock page A or move to page B
```

During step 5, another backend could acquire an exclusive lock on page A
and modify its content.  The scanning backend still has a pin (so the
page can't be evicted), but the content under the pin has changed.

This is why:
- **INSERT** only appends tuples at the end of the page — existing
  tuple pointers remain valid.
- **VACUUM** uses cleanup locks — it waits for all pin-holders to leave
  before deleting tuples, because deletion changes tuple offsets.
- **MERGE** uses cleanup locks for the same reason — we're changing
  tuple offsets on both pages.

---

## 7. The Pin Gap Problem

When a scan moves from page L to page R:

```
1. Scan has pin+lock on L
2. Unlock L (keep pin)
3. Read tuples from L's right-link → R
4. Release pin on L
5.                         ← THE GAP: no pin, no lock on R yet
6. Pin R (ReadBuffer)
7. Lock R (BT_READ)
8. Read tuples from R
```

During the gap (step 5), **anything** can happen to R:
- R could be split
- R could be deleted (page deletion)
- R could be merged (our operation!)
- R's content could change completely

This is why scans always re-validate page state after acquiring a new
lock.  `_bt_moveright()` handles the case where the page was split
while the scan had no lock.  Our merge must also be safe against this:
a scan that lands on L after the merge finds L marked `BTP_HALF_DEAD`
and follows the right-link to R (where the data now lives).

---

## 8. Buffer Replacement (Clock Sweep)

When the buffer pool is full and a new page needs to be read:

1. The clock sweep algorithm scans buffer descriptors.
2. For each buffer with pin_count = 0:
   - If usage_count > 0: decrement usage_count, continue.
   - If usage_count = 0: this buffer is the eviction victim.
3. If the victim is dirty, write it to disk first.
4. Replace the buffer content with the new page.

**Buffers with pin_count > 0 are never evicted.**  This is why pins
exist — they prevent the clock sweep from recycling a buffer while
someone is using it.

---

## 9. Critical Sections

A critical section is a region of code where a failure (ERROR, FATAL)
would leave the database in an inconsistent state.  Inside a critical
section, errors are promoted to PANIC (crash the whole server + redo
from WAL on restart).

```c
START_CRIT_SECTION();
// Must not ERROR between here...
// Modify pages
MarkBufferDirty(buf1);
MarkBufferDirty(buf2);
// Write WAL record
XLogRecPtr lsn = XLogInsert(RM_BTREE_ID, op);
PageSetLSN(page1, lsn);
PageSetLSN(page2, lsn);
// ...and here
END_CRIT_SECTION();
```

### Rules for Critical Sections

1. **No memory allocation** inside a critical section (palloc can ERROR).
2. **No lock acquisition** (acquiring a lock can block, and if the lock
   holder errors, we deadlock — both sides are in critical sections).
3. **All locks must be acquired BEFORE** `START_CRIT_SECTION()`.
4. **All page modifications + WAL logging** happen inside.
5. **Critical section = atomic** — either all changes survive (via WAL
   replay) or none do.

### Why This Matters for Merge

Our merge modifies three pages (L, R, Parent) and writes one WAL record.
All three modifications must be inside a single critical section to
ensure atomicity.  If we crash between modifying L and modifying R, the
WAL replay will redo ALL three modifications from the single WAL record.

---

## 10. WAL and Buffer Interaction

### LSN (Log Sequence Number)

Every page has an LSN in its header (`PageHeaderData.pd_lsn`).  This
records which WAL record last modified this page.

### The LSN Contract

After modifying a page and writing a WAL record:

```c
XLogRecPtr lsn = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MERGE);
PageSetLSN(page, lsn);
```

The buffer manager guarantees: **a dirty page will not be written to
disk until its WAL record has been flushed to disk.**  This is the
Write-Ahead Logging guarantee — the WAL record is always on disk before
the data change.

### During Recovery

The redo function checks the page's LSN before replaying:

```c
if (lsn > PageGetLSN(page))
{
    // Page is old — replay the change
    ...
}
else
{
    // Page already has this change — skip
}
```

This makes recovery idempotent — replaying the same WAL record twice
is harmless.

---

## 11. Relevance to Page Merge

### Buffers We Touch

| Buffer | How Accessed | Lock Needed |
|--------|-------------|-------------|
| L (source page) | `_bt_getbuf(rel, L_blkno, BT_WRITE)` → upgrade to cleanup | Cleanup lock |
| R (destination page) | `_bt_getbuf(rel, R_blkno, BT_WRITE)` → upgrade to cleanup | Cleanup lock |
| Parent | `_bt_getbuf(rel, parent_blkno, BT_WRITE)` | BT_WRITE |

### Buffer Lifecycle During Merge

```
 1. Pin + lock L (cleanup)     [_bt_getbuf + LockBufferForCleanup]
 2. Pin + lock R (cleanup)     [_bt_getbuf + LockBufferForCleanup]
 3. Pin + lock Parent (write)  [_bt_getbuf]
 4. START_CRIT_SECTION()
 5. Build merged page in local temp buffer
 6. memcpy temp → R's page
 7. MarkBufferDirty(R)
 8. Set L as half-dead tombstone
 9. MarkBufferDirty(L)
10. Delete L's pivot from Parent, update R's downlink
11. MarkBufferDirty(Parent)
12. XLogInsert(XLOG_BTREE_MERGE)
13. PageSetLSN(L, lsn); PageSetLSN(R, lsn); PageSetLSN(Parent, lsn)
14. END_CRIT_SECTION()
15. Unlock + unpin all three
16. Call _bt_unlink_halfdead_page() for L
```

### Why Cleanup Locks, Not Just BT_WRITE

During step 5–6, we **replace R's entire page content** (prepend L's
tuples).  If a scanner holds a pin on R (from a previous read), its
offsets into R's page are now wrong.  The cleanup lock on R ensures the
pin count is 1 (only ours) before we proceed.

Same for L — we replace its content with a half-dead tombstone.  Any
pin-holder would see garbage after we modify L.

---

## 12. Function Reference

| Function | File | Purpose |
|----------|------|---------|
| `ReadBuffer(rel, blkno)` | bufmgr.c | Pin a buffer, read from disk if needed |
| `ReleaseBuffer(buf)` | bufmgr.c | Release pin |
| `LockBuffer(buf, mode)` | bufmgr.c | Acquire buffer lock |
| `UnlockReleaseBuffer(buf)` | bufmgr.c | Unlock + release pin in one call |
| `MarkBufferDirty(buf)` | bufmgr.c | Set dirty flag (must be inside crit section for WAL-logged changes) |
| `BufferGetPage(buf)` | bufmgr.h | Get pointer to page content |
| `LockBufferForCleanup(buf)` | bufmgr.c | Exclusive lock + wait for pin_count=1 |
| `ConditionalLockBufferForCleanup(buf)` | bufmgr.c | Try cleanup lock, return false if busy |
| `_bt_getbuf(rel, blkno, access)` | nbtpage.c | nbtree wrapper: pin + lock |
| `_bt_relandgetbuf(rel, buf, blkno, access)` | nbtpage.c | Release old buffer, pin+lock new one |
| `_bt_lockbuf(rel, buf, access)` | nbtpage.c | Lock already-pinned buffer |
| `_bt_unlockbuf(rel, buf)` | nbtpage.c | Unlock (keep pin) |
| `START_CRIT_SECTION()` | miscadmin.h | Begin critical section |
| `END_CRIT_SECTION()` | miscadmin.h | End critical section |
| `XLogBeginInsert()` | xloginsert.h | Begin building a WAL record |
| `XLogRegisterBuffer(block_id, buf, flags)` | xloginsert.h | Register a buffer for WAL |
| `XLogRegisterData(data, len)` | xloginsert.h | Register data payload for WAL |
| `XLogInsert(rmgr, info)` | xloginsert.h | Write WAL record, return LSN |
| `PageSetLSN(page, lsn)` | bufpage.h | Set page's LSN |

---

*For locking details, see `postgres_locking_reference.md`.
For WAL mechanics, see `btree_wal_explainer.md`.
For B-tree structure, see `btree_internals.md`.*
