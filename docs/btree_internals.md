# PostgreSQL B-Tree Internals — A Deep Dive

> This document is a comprehensive reference for understanding PostgreSQL's B-tree
> index implementation in `src/backend/access/nbtree/`. It is intended for someone
> who will be implementing new B-tree operations (such as page merging).

---

## Table of Contents

1. [Overview & Theoretical Foundation](#1-overview--theoretical-foundation)
2. [On-Disk Page Layout](#2-on-disk-page-layout)
3. [B-Tree Page Layout & Opaque Data](#3-b-tree-page-layout--opaque-data)
4. [Index Tuple Format](#4-index-tuple-format)
5. [Tree Structure & Navigation](#5-tree-structure--navigation)
6. [The Metapage](#6-the-metapage)
7. [Key Space & High Keys](#7-key-space--high-keys)
8. [Suffix Truncation](#8-suffix-truncation)
9. [Searches (Descending the Tree)](#9-searches-descending-the-tree)
10. [Insertions & Page Splits](#10-insertions--page-splits)
11. [Deletion Mechanisms](#11-deletion-mechanisms)
12. [VACUUM Integration](#12-vacuum-integration)
13. [Deduplication & Posting Lists](#13-deduplication--posting-lists)
14. [Locking & Concurrency](#14-locking--concurrency)
15. [WAL Logging & Crash Recovery](#15-wal-logging--crash-recovery)
16. [Source File Map](#16-source-file-map)

---

## 1. Overview & Theoretical Foundation

PostgreSQL's B-tree is based on the **Lehman & Yao (L&Y) algorithm** (1981), with
extensions from **Lanin & Shasha** (1986) for deletion.

### Key Differences from a Textbook B-Tree

| Feature | Classic B-Tree | Lehman & Yao B-Tree (Postgres) |
|---------|---------------|-------------------------------|
| Right-links | No | Yes — every page has a `btpo_next` pointer |
| Left-links | No | Yes (Postgres extension) — `btpo_prev` |
| High key | No | Yes — upper bound of keys on a page |
| Read locks | Held down the tree | Only on the *current* page |
| Concurrent splits | Detected by parent | Detected by comparing key vs. high key |
| Unique keys | Optional | **Always** unique (heap TID is tiebreaker) |

### The Core Insight

The right-link and high key allow **lock-free tree descent**: a search only needs
to lock *one page at a time*. If a page split occurs between descending from
parent to child, the searcher detects it by comparing its search key to the
child's high key. If the search key exceeds the high key, the search follows
the right-link to the next page.

```
              ┌──────────────┐
              │   Parent     │
              │  [10, 20, 30]│
              └──┬───┬───┬───┘
                 │   │   │
         ┌───────┘   │   └───────┐
         ▼           ▼           ▼
   ┌──────────┐ ┌──────────┐ ┌──────────┐
   │ Page A   │→│ Page B   │→│ Page C   │
   │ hi=10    │ │ hi=20    │ │ hi=30    │
   │ [1,3,5,7]│ │[12,14,17]│ │[22,25,28]│
   └──────────┘ └──────────┘ └──────────┘
        ↑            ↑             ↑
        └────────────┘             │
          btpo_prev links          │
              ←────────────────────┘
```

---

## 2. On-Disk Page Layout

Every PostgreSQL page (heap or index) is **8 KB** (BLCKSZ) and shares the same
fundamental layout defined in `src/include/storage/bufpage.h`.

```
 0                                                           8191
 ┌──────────────────────────────────────────────────────────────┐
 │  PageHeaderData (24 bytes)                                   │
 ├──────────────────────────────────────────────────────────────┤
 │  ItemIdData[] (line pointer array — grows downward)          │
 │  Each entry: 4 bytes (lp_off:15, lp_flags:2, lp_len:15)     │
 │  pd_lower ──────────────────────────────────┐                │
 ├─────────────────────────────────────────────┤                │
 │                                             │                │
 │  Free Space                                 │                │
 │                                             │                │
 │  pd_upper ──────────────────────────────────┘                │
 ├──────────────────────────────────────────────────────────────┤
 │  Tuple Data (grows upward from pd_upper)                     │
 │  Index tuples are stored here                                │
 │  pd_special ─────────────────────────────────┐               │
 ├──────────────────────────────────────────────┤               │
 │  Special Space (BTPageOpaqueData for btree)  │               │
 │  Contains btpo_prev, btpo_next, level, flags │               │
 └──────────────────────────────────────────────────────────────┘
```

### `PageHeaderData` Structure

```c
typedef struct PageHeaderData {
    PageXLogRecPtr pd_lsn;       /* LSN of last change (8 bytes) */
    uint16         pd_checksum;  /* page checksum */
    uint16         pd_flags;     /* PD_HAS_FREE_LINES, PD_PAGE_FULL, PD_ALL_VISIBLE */
    LocationIndex  pd_lower;     /* offset to start of free space */
    LocationIndex  pd_upper;     /* offset to end of free space */
    LocationIndex  pd_special;   /* offset to start of special space */
    uint16         pd_pagesize_version;
    TransactionId  pd_prune_xid;
    ItemIdData     pd_linp[];    /* line pointer array */
} PageHeaderData;                /* 24 bytes (not counting pd_linp) */
```

### `ItemIdData` (Line Pointer) — 4 bytes each

```c
typedef struct ItemIdData {
    unsigned lp_off:15;    /* byte offset to tuple from page start */
    unsigned lp_flags:2;   /* LP_UNUSED=0, LP_NORMAL=1, LP_REDIRECT=2, LP_DEAD=3 */
    unsigned lp_len:15;    /* byte length of tuple */
} ItemIdData;
```

### Key Macros for Page Manipulation

```c
PageGetFreeSpace(page)           /* pd_upper - pd_lower */
PageGetMaxOffsetNumber(page)     /* number of line pointers */
PageGetItem(page, itemid)        /* dereference a line pointer */
PageGetItemId(page, offsetNumber)/* get the N-th ItemIdData */
PageAddItemExtended(page, item, size, offsetNumber, flags)
PageIndexTupleDelete(page, offnum)
PageIndexMultiDelete(page, itemnos, nitems)
```

---

## 3. B-Tree Page Layout & Opaque Data

Every B-tree page stores its metadata in the "special space" at the end of the
page. This is the `BTPageOpaqueData` structure:

```c
typedef struct BTPageOpaqueData {
    BlockNumber btpo_prev;    /* left sibling (P_NONE if leftmost) */
    BlockNumber btpo_next;    /* right sibling (P_NONE if rightmost) */
    uint32      btpo_level;   /* 0 = leaf, increases upward */
    uint16      btpo_flags;   /* see below */
    BTCycleId   btpo_cycleid; /* vacuum cycle ID of latest split */
} BTPageOpaqueData;
```

### Page Flags (`btpo_flags`)

| Flag | Bit | Meaning |
|------|-----|---------|
| `BTP_LEAF` | 0x0001 | This is a leaf page |
| `BTP_ROOT` | 0x0002 | This is the root page |
| `BTP_DELETED` | 0x0004 | Page has been deleted from tree |
| `BTP_META` | 0x0008 | This is the metapage (page 0) |
| `BTP_HALF_DEAD` | 0x0010 | Downlink removed, awaiting unlink |
| `BTP_SPLIT_END` | 0x0020 | Rightmost page of a split group |
| `BTP_HAS_GARBAGE` | 0x0040 | Has LP_DEAD items (hint) |
| `BTP_INCOMPLETE_SPLIT` | 0x0080 | Right sibling missing its downlink |
| `BTP_HAS_FULLXID` | 0x0100 | Contains BTDeletedPageData |

### B-Tree Page Item Layout

```
 For a non-rightmost page:
 ┌───────────────────────────────────────────────────────┐
 │ Item 1: HIGH KEY (upper bound for this page)          │
 │ Item 2: First real data item                          │
 │ Item 3: Second data item                              │
 │ ...                                                   │
 │ Item N: Last data item                                │
 │                     [Special: BTPageOpaqueData]        │
 └───────────────────────────────────────────────────────┘

 For the rightmost page on a level:
 ┌───────────────────────────────────────────────────────┐
 │ Item 1: First real data item (NO high key!)           │
 │ Item 2: Second data item                              │
 │ ...                                                   │
 │                     [Special: BTPageOpaqueData]        │
 └───────────────────────────────────────────────────────┘
```

**Important macros:**
```c
P_FIRSTDATAKEY(opaque)   /* OffsetNumber of first data item */
P_HIKEY                  /* = 1 — high key is always item 1 */
P_FIRSTKEY               /* = 2 — first real key on non-rightmost pages */
P_NONE                   /* = 0 — sentinel for "no page" */
P_RIGHTMOST(opaque)      /* btpo_next == P_NONE */
P_ISLEAF(opaque)         /* btpo_flags & BTP_LEAF */
P_ISROOT(opaque)         /* btpo_flags & BTP_ROOT */
P_ISDELETED(opaque)      /* btpo_flags & BTP_DELETED */
P_ISHALFDEAD(opaque)     /* btpo_flags & BTP_HALF_DEAD */
P_IGNORE(opaque)         /* deleted OR half-dead */
```

---

## 4. Index Tuple Format

Each item on a B-tree page is an `IndexTupleData`:

```c
typedef struct IndexTupleData {
    ItemPointerData t_tid;   /* 6 bytes: pointer to heap tuple (block + offset) */
    unsigned short  t_info;  /* 2 bytes: size + flags */
} IndexTupleData;            /* 8 bytes header, followed by key data */
```

### `t_info` Bit Layout

```
 Bits 0-12:  INDEX_SIZE_MASK (0x1FFF) — tuple size in bytes
 Bit  13:    INDEX_ALT_TID_MASK (0x2000) — AM-specific (btree uses for pivots/postings)
 Bit  14:    INDEX_VAR_MASK (0x4000) — has variable-width attributes
 Bit  15:    INDEX_NULL_MASK (0x8000) — has null attributes
```

### Three Types of B-Tree Tuples

```
 1. LEAF (Non-Pivot) Tuple:
    ┌──────────────────────────────────────────┐
    │ t_tid → heap TID  │ t_info (size+flags)  │
    ├──────────────────────────────────────────┤
    │ Key attribute values (all columns)       │
    └──────────────────────────────────────────┘
    Points to an actual heap row.

 2. PIVOT Tuple (on internal pages / high keys):
    ┌──────────────────────────────────────────┐
    │ t_tid → child block│ t_info (ALT_TID set)│
    ├──────────────────────────────────────────┤
    │ Key attributes (possibly truncated)      │
    └──────────────────────────────────────────┘
    The t_tid encodes a downlink (child page's block number).
    May have fewer key attributes than the index has columns.
    Truncated attributes are implicitly "minus infinity".

 3. POSTING LIST Tuple (deduplicated leaf tuple):
    ┌──────────────────────────────────────────┐
    │ t_tid (first TID)  │ t_info (ALT_TID set)│
    ├──────────────────────────────────────────┤
    │ Key attribute values                     │
    ├──────────────────────────────────────────┤
    │ Array of ItemPointerData (heap TIDs)     │
    └──────────────────────────────────────────┘
    Multiple heap TIDs with the same key, stored in one tuple.
```

---

## 5. Tree Structure & Navigation

### Vertical Structure

```
Level 2 (Root):    ┌──────────────────────┐
                   │ [Pivot₁] [Pivot₂]    │   ← One page (or more)
                   └───┬──────────┬────┬──┘
                       │          │    │
Level 1 (Internal):┌───┴──┐  ┌───┴──┐ ┌┴─────┐
                   │      │→│      │→│      │   ← Linked by btpo_next
                   └──┬───┘ └──┬───┘ └──┬───┘
                      │        │        │
Level 0 (Leaf):   ┌───┴─┐ ┌───┴─┐ ┌───┴─┐ ┌─────┐
                  │     │→│     │→│     │→│     │   ← Doubly linked
                  │     │←│     │←│     │←│     │
                  └─────┘ └─────┘ └─────┘ └─────┘
                     │       │       │       │
                     ▼       ▼       ▼       ▼
                   Heap    Heap    Heap    Heap
                   Rows    Rows    Rows    Rows
```

### Horizontal Navigation (Sibling Links)

Every page has:
- `btpo_next` → right sibling (required by L&Y)
- `btpo_prev` → left sibling (PostgreSQL extension for backward scans)

The **rightmost** page on any level has `btpo_next = P_NONE`.
The **leftmost** page on any level has `btpo_prev = P_NONE`.

### Vertical Navigation (Downlinks)

On internal pages, each pivot tuple's `t_tid` encodes a **downlink** — the
block number of the child page. The first data item on an internal page
has a "minus infinity" key (its actual key value is irrelevant).

```
 Internal Page:
 ┌─────────────────────────────────────────────────────────┐
 │ HI_KEY: [30]                                            │
 │ Item 2: [-∞]  → child page for keys (-∞, 10]           │
 │ Item 3: [10]  → child page for keys (10, 20]           │
 │ Item 4: [20]  → child page for keys (20, 30]           │
 └─────────────────────────────────────────────────────────┘
```

---

## 6. The Metapage

Page 0 of every B-tree index is the **metapage**. It is not part of the
tree structure — it's a fixed-location index into the tree.

```c
typedef struct BTMetaPageData {
    uint32      btm_magic;        /* BTREE_MAGIC = 0x053162 */
    uint32      btm_version;      /* BTREE_VERSION = 4 */
    BlockNumber btm_root;         /* true root page */
    uint32      btm_level;        /* level of the root */
    BlockNumber btm_fastroot;     /* "fast root" — lowest single-page level */
    uint32      btm_fastlevel;    /* level of the fast root */
    uint32      btm_last_cleanup_num_delpages;
    float8      btm_last_cleanup_num_heap_tuples; /* deprecated */
    bool        btm_allequalimage;
} BTMetaPageData;
```

### The "Fast Root" Optimization

After many deletions, the tree may have several single-page levels:

```
 True Root (level 3): ┌───────┐
                      │  [∅]  │ ← single page
                      └───┬───┘
                          │
 Level 2:             ┌───┴───┐
                      │  [∅]  │ ← single page
                      └───┬───┘
                          │
 Fast Root (level 1): ┌───┴───┐
                      │[A][B] │ ← first level with > 1 child or > 1 key
                      └─┬───┬─┘
                        │   │
 Leaves:           ┌────┘   └────┐
                   │             │
                ┌──┴──┐       ┌──┴──┐
                │     │  →    │     │
                └─────┘       └─────┘
```

Searches start at the **fast root**, skipping the "skinny" single-page levels.
The metapage caches both the true root and the fast root.

---

## 7. Key Space & High Keys

### The High Key Rule

Every non-rightmost page has a **high key** stored as item 1. The high key
is the **upper bound** of keys that belong on this page:

```
 For a key v to belong on page P:
   - If P has a left sibling:  left_sibling.high_key < v <= P.high_key
   - If P is leftmost:         v <= P.high_key
```

The rightmost page has no high key (implicitly +∞).

### How High Keys Enable Concurrent Split Detection

```
 Scenario: Searching for key=25

 1. Descend from parent, which says keys [20,30] are on Page B
 2. But Page B was split while we were descending!
 3. We land on Page B, check: high_key = 23, but 25 > 23
 4. We know the page was split → follow btpo_next to Page B'
 5. Page B' has high_key = 30, and 25 <= 30 → correct page found

 Before split:              After split:
 ┌──────────────┐          ┌──────────┐    ┌──────────┐
 │ Page B       │          │ Page B   │ →  │ Page B'  │
 │ hi=30        │   ──►    │ hi=23    │    │ hi=30    │
 │ [20,22,25,28]│          │ [20,22]  │    │ [25,28]  │
 └──────────────┘          └──────────┘    └──────────┘
```

---

## 8. Suffix Truncation

When a leaf page splits, the new high key (which also becomes the downlink
in the parent) is created by **suffix truncation**: only the attributes
needed to distinguish the last item on the left page from the first item
on the right page are kept.

```
 Example: Index on (city, street, number)

 Last item on left:   ('Boston', 'Main St',  100)
 First item on right: ('Boston', 'Oak Ave',   50)

 The distinguishing attribute is "street" (column 2).
 Truncated high key:  ('Boston', 'Main St')   ← "number" truncated away

 This smaller pivot tuple saves space in the parent page,
 increasing fan-out and reducing tree height.
```

Truncated attributes are implicitly treated as **minus infinity** during
comparisons, which preserves the invariant that the high key is an upper
bound.

---

## 9. Searches (Descending the Tree)

### `_bt_search()` — The Main Descent

```
 _bt_search(rel, key, &buf, access):
   1. Read metapage → get fast root
   2. Lock fast root page (BT_READ)
   3. Loop (descending levels):
       a. _bt_moveright() — follow right-links if needed
       b. If leaf level reached → return (page is locked)
       c. _bt_binsrch() → find downlink to follow
       d. Push current (blkno, offset) onto stack
       e. Read child page, lock it (BT_READ)
       f. Release parent lock
   4. If access == BT_WRITE, upgrade leaf lock:
       - Release BT_READ
       - Acquire BT_WRITE
       - _bt_moveright() again (page may have split)
   5. Return stack + locked leaf buffer
```

**Key invariant**: Only ONE page is locked at any time during descent (except
during the brief lock upgrade at the leaf).

### `_bt_moveright()` — Handling Concurrent Splits

```c
// Pseudocode
while (!P_RIGHTMOST(opaque) && search_key > high_key) {
    next = opaque->btpo_next;
    buf = _bt_relandgetbuf(rel, buf, next, access);
    // Now on the right sibling — check again
}
```

Also skips deleted/half-dead pages by following right links.

### `_bt_binsrch()` — Binary Search Within a Page

Standard binary search. On internal pages, finds the last key ≤ search key
(the downlink to follow). On leaf pages, finds the first key ≥ search key
(the scan start point).

---

## 10. Insertions & Page Splits

### Insert Path

```
 _bt_doinsert():
   1. Build scankey from new tuple
   2. _bt_search() → descend to correct leaf, get lock + stack
   3. _bt_findinsertloc() → find exact offset, handle dedup/delete if needed
   4. _bt_insertonpg() → insert tuple (may trigger split)
```

### `_bt_insertonpg()` — The Core Insert

If there's enough free space:
```
 CRITICAL SECTION:
   PageAddItem(page, tuple, size, offset)
   MarkBufferDirty(buf)
   Clear INCOMPLETE_SPLIT on child (if internal insert)
   XLogInsert(RM_BTREE_ID, XLOG_BTREE_INSERT_LEAF)
 END CRITICAL SECTION
```

If there's NOT enough space → **page split**.

### `_bt_split()` — Page Split (The Inverse of Merge)

This is the most important function to understand for implementing merges,
because a merge is conceptually the reverse operation.

```
 _bt_split(rel, buf, newitem, newitemoff):

   Step 1: Choose Split Point
     _bt_findsplitloc() → returns firstrightoff, newitemonleft
     Goal: balance space, maximize suffix truncation effectiveness

   Step 2: Build Left Page (temporary copy)
     - Copy items 1..firstrightoff-1 to temp page
     - Insert newitem at correct position (if newitemonleft)
     - Create new high key via suffix truncation
     - Set BTP_INCOMPLETE_SPLIT flag

   Step 3: Build Right Page (new buffer)
     - _bt_allocbuf() → get a new page
     - Copy items firstrightoff..N to new page
     - Insert newitem at correct position (if !newitemonleft)
     - High key = original page's old high key (for non-rightmost)
     - btpo_prev = original page, btpo_next = original's old right sibling

   Step 4: Update Original Right Sibling
     - Lock original right sibling
     - Set its btpo_prev = new right page

   Step 5: Atomic Commit (CRITICAL SECTION)
     - Copy temp left page back onto original buffer
     - Mark original + right + right_sibling buffers dirty
     - XLogInsert(XLOG_BTREE_SPLIT_L or _R)

   Step 6: Insert Downlink in Parent
     _bt_insert_parent() → recursive insert
```

**Visual: A Page Split**

```
 BEFORE:                                 AFTER:
 ┌─────────────────┐                    ┌─────────────────┐
 │ Page P           │                    │ Page P (left)    │
 │ hi = 50          │                    │ hi = 25 (new!)   │
 │ [10,15,20,25,    │     ──SPLIT──►    │ [10,15,20]       │
 │  30,35,40,45]    │                    │ INCOMPLETE_SPLIT │
 │ next→R, prev→L   │                    │ next→N, prev→L   │
 └─────────────────┘                    └────────┬────────┘
                                                  │
                                                  ▼ btpo_next
                                        ┌─────────────────┐
                                        │ Page N (new right)│
                                        │ hi = 50           │
                                        │ [25,30,35,40,45]  │
                                        │ next→R, prev→P    │
                                        └────────┬────────┘
                                                  │
                                                  ▼ btpo_next
                                        ┌─────────────────┐
                                        │ Page R (old right)│
                                        │ prev→N (updated!) │
                                        └─────────────────┘

 Then parent gets a new downlink [25]→N inserted.
 Once parent insert completes, INCOMPLETE_SPLIT is cleared on P.
```

### Lock Order During Split

```
 1. Original page (write lock — already held)
 2. New right page (write lock — just allocated)
 3. Original right sibling (write lock — for btpo_prev update)
 4. Parent page (write lock — for downlink insertion, after releasing above)
```

Always **left-to-right, then up**. This prevents deadlocks.

---

## 11. Deletion Mechanisms

PostgreSQL has **five** distinct deletion mechanisms for B-tree indexes:

### 11.1 LP_DEAD Marking (Hint Bits)

```
 During an index scan, if the heap tuple is found to be dead:
   → Set LP_DEAD flag on the index line pointer
   → No WAL logging (it's just a hint)
   → Set BTP_HAS_GARBAGE flag on the page
```

### 11.2 Simple Deletion (`_bt_delitems_delete`)

When inserting finds LP_DEAD items on a page:
```
 → Batch-delete all LP_DEAD items
 → Check heap to generate snapshotConflictHorizon
 → Also opportunistically check "extra" nearby tuples
 → WAL: XLOG_BTREE_DELETE
```

### 11.3 Bottom-Up Deletion

Triggered when a page is about to split due to version churn (UPDATE cycles):
```
 → Identifies duplicate keys caused by MVCC versions
 → Cooperates with table AM to check which are safe to delete
 → Same WAL record as simple deletion
 → Acts as a "backstop" against unnecessary page splits
```

### 11.4 VACUUM Tuple Deletion (`_bt_delitems_vacuum`)

During btbulkdelete:
```
 → Acquires cleanup lock on the leaf page
 → Identifies dead tuples via callback
 → Handles posting list partial updates (BTVacuumPosting)
 → Bulk deletes via PageIndexMultiDelete()
 → WAL: XLOG_BTREE_VACUUM
```

### 11.5 Page Deletion (Two-Phase Protocol)

For pages that become **completely empty** after tuple deletion:

```
 Phase 1: _bt_mark_page_halfdead()
   ┌──────────┐         ┌──────────┐
   │ Parent   │         │ Parent   │
   │ [A][B][C]│   ──►   │ [A]  [C] │  ← B's downlink removed,
   └─┬──┬──┬─┘         └─┬────┬──┘    C's downlink moved over B's position
     │  │  │              │    │
    ┌┘  │  └┐            ┌┘    └┐
    ▼   ▼   ▼            ▼      ▼
   [A] [B] [C]          [A]    [C]
        ↓                       ↑
    BTP_HALF_DEAD        Key space of B now belongs to C

 Phase 2: _bt_unlink_halfdead_page()
   [A] ←→ [B] ←→ [C]   ──►   [A] ←→ [C]
                                       ↑
                               [B] marked BTP_DELETED
                               btpo_prev/next preserved (tombstone)
                               safexid stamped for deferred recycling
```

**Restrictions:**
- Never delete the rightmost page on any level
- Never delete the root
- Left sibling must not have INCOMPLETE_SPLIT

---

## 12. VACUUM Integration

### VACUUM Scan Flow

```
 btbulkdelete(info, stats, callback, callback_state)
   └─► btvacuumscan()
         │
         │  For each page (physical order, block 1..N):
         └─► btvacuumpage(vstate, buf)
               │
               ├─ If deleted/recyclable → record in FSM
               ├─ If half-dead → attempt_pagedel = true
               ├─ If leaf:
               │    ├─ Upgrade to cleanup lock
               │    ├─ Check for concurrent splits (btpo_cycleid)
               │    ├─ Iterate tuples, call callback
               │    ├─ _bt_delitems_vacuum() for deletable tuples
               │    └─ If page now empty → attempt_pagedel = true
               │
               └─ If attempt_pagedel:
                    └─ _bt_pagedel() → two-phase deletion
```

### The BTVacState Structure

```c
typedef struct BTVacState {
    IndexVacuumInfo        *info;
    IndexBulkDeleteResult  *stats;
    IndexBulkDeleteCallback callback;
    void                   *callback_state;
    BTCycleId               cycleid;        /* detects concurrent splits */
    MemoryContext           pagedelcontext;
    BTPendingFSM           *pendingpages;   /* deferred FSM entries */
    int                     npendingpages;
    /* ... */
} BTVacState;
```

### Backtracking for Concurrent Splits

When VACUUM finds that a page was split during its scan, it may need to
re-visit pages to avoid missing dead tuples:

```
 VACUUM scans left-to-right (block 1, 2, 3, ...)
 If page 5 was split → items may have moved to page 3 (already scanned!)
 Detection: btpo_cycleid matches current VACUUM's cycleid
 Recovery: Follow btpo_next, backtrack to the lower-numbered page
```

---

## 13. Deduplication & Posting Lists

### When Deduplication Happens

Deduplication is **lazy** — it only runs when a page would otherwise split:

```
 Insert → page is full → try LP_DEAD deletion → try bottom-up deletion
   → try deduplication → if still full → page split
```

### Posting List Structure

```
 Before deduplication:
 ┌──────────────────────────────────────────────────┐
 │ (key=42, tid=(10,1))  ← 16 bytes                │
 │ (key=42, tid=(10,5))  ← 16 bytes                │
 │ (key=42, tid=(11,3))  ← 16 bytes                │
 │ Total: 48 bytes for 3 items                      │
 └──────────────────────────────────────────────────┘

 After deduplication:
 ┌──────────────────────────────────────────────────┐
 │ (key=42, tids=[(10,1),(10,5),(11,3)])  ← 22 bytes│
 │ Total: 22 bytes for 3 items (54% savings)        │
 └──────────────────────────────────────────────────┘
```

### Posting List Splits

When a new tuple overlaps with an existing posting list:
```
 Existing:  (key=42, tids=[(10,1),(10,5),(11,3)])
 New:       (key=42, tid=(10,3))

 The new TID (10,3) falls inside the posting list's range.
 → Posting list split: insert (10,3) into the sorted TID array
 → May also trigger a page split if the page is full
```

---

## 14. Locking & Concurrency

### Lock Levels in B-Tree Operations

PostgreSQL B-tree uses **two types of locks**:

#### 1. Buffer-Level Locks (Lightweight, Page-Level)

```c
BT_READ   = BUFFER_LOCK_SHARE       /* Concurrent reads OK, blocks writes */
BT_WRITE  = BUFFER_LOCK_EXCLUSIVE   /* Blocks everything */
/* Cleanup lock = exclusive + wait for all pins to be released */
```

Used via:
```c
_bt_lockbuf(rel, buf, BT_READ);
_bt_lockbuf(rel, buf, BT_WRITE);
_bt_upgradelockbufcleanup(rel, buf);  /* upgrade to cleanup lock */
```

#### 2. Relation-Level Locks (Heavyweight, Table-Level)

```
 AccessShareLock (1)      — SELECT
 RowShareLock (2)         — SELECT FOR UPDATE
 RowExclusiveLock (3)     — INSERT, UPDATE, DELETE
 ShareUpdateExclusiveLock (4) — VACUUM, ANALYZE
 ShareLock (5)            — CREATE INDEX (non-concurrent)
 ShareRowExclusiveLock (6)
 ExclusiveLock (7)
 AccessExclusiveLock (8)  — ALTER TABLE, DROP, REINDEX
```

### Lock Ordering Rules (Critical for Deadlock Prevention)

```
 Rule 1: Always lock pages LEFT-TO-RIGHT at the same level.
         Never acquire a lock on a left sibling while holding
         a lock on a right sibling.

 Rule 2: Always lock CHILD before PARENT when going UP.
         (Parent lock is acquired after child, during split/delete.)

 Rule 3: Never hold locks across levels during DESCENT.
         (Release parent before locking child.)

 Rule 4: When upgrading from BT_READ to BT_WRITE on a leaf,
         release the read lock first, then acquire the write lock.
         The page may have changed — use _bt_moveright() to verify.

 Rule 5: Metapage is always locked LAST (after tree pages).
```

### Cleanup Locks vs. Exclusive Locks

```
 Exclusive Lock (BT_WRITE):
   - Blocks other readers and writers
   - Does NOT wait for existing pins
   - Used by: inserts, splits, simple deletion

 Cleanup Lock (LockBufferForCleanup):
   - Exclusive lock that ALSO waits until no other backend pins the page
   - Guarantees nobody else can reference the page at all
   - Used by: VACUUM tuple deletion (prevents TID recycling races)
   - More expensive — can block for a long time if a cursor holds a pin
```

### Concurrency Scenarios

#### Concurrent Insert During Scan

```
 Scanner holds BT_READ on page P.
 Inserter wants to insert on page P.
   → Inserter blocks until scanner releases.
   → Scanner finishes, releases lock.
   → Inserter acquires BT_WRITE, inserts.
 Scanner moves to next page. If P split while scanner didn't hold lock:
   → Scanner's saved "next page" link is still valid (items don't move left).
```

#### Concurrent Split During Descent

```
 Searcher descends from parent, follows downlink to child page.
 Between reading parent and locking child, the child splits.
   → Searcher locks the (now left-half) child.
   → Compares search key to high key.
   → Search key > high key → follow btpo_next to the right half.
   → This is the _bt_moveright() logic.
```

#### Concurrent Delete During Backward Scan

```
 Scanner is on page C, moving left to page B.
 Between releasing C and locking B, page B is deleted.
   → Scanner locks B, sees BTP_DELETED.
   → Returns to original page C.
   → If C is still live, restarts from C's left link.
   → If C is also deleted, moves right until finding a live page.
   → This is the _bt_lock_and_validate_left() logic.
```

---

## 15. WAL Logging & Crash Recovery

### WAL Record Types

| Record Type | Value | Description | Block References |
|-------------|-------|-------------|-----------------|
| `XLOG_BTREE_INSERT_LEAF` | 0x00 | Leaf insert | 0=page |
| `XLOG_BTREE_INSERT_UPPER` | 0x10 | Internal insert | 0=page, 1=child |
| `XLOG_BTREE_INSERT_META` | 0x20 | Internal insert + meta | 0=page, 1=child, 2=meta |
| `XLOG_BTREE_SPLIT_L` | 0x30 | Split, new item on left | 0=left, 1=right, 2=right_sib, 3=child |
| `XLOG_BTREE_SPLIT_R` | 0x40 | Split, new item on right | same |
| `XLOG_BTREE_INSERT_POST` | 0x50 | Posting list split | 0=page |
| `XLOG_BTREE_DEDUP` | 0x60 | Deduplication | 0=page |
| `XLOG_BTREE_DELETE` | 0x70 | Simple deletion | 0=page |
| `XLOG_BTREE_UNLINK_PAGE` | 0x80 | Unlink deleted page | 0=target, 1=left, 2=right |
| `XLOG_BTREE_UNLINK_PAGE_META` | 0x90 | Unlink + meta update | 0-2 + 4=meta |
| `XLOG_BTREE_NEWROOT` | 0xA0 | New root | 0=root, 1=left_child, 2=meta |
| `XLOG_BTREE_MARK_PAGE_HALFDEAD` | 0xB0 | Mark half-dead | 0=leaf, 1=parent |
| `XLOG_BTREE_VACUUM` | 0xC0 | VACUUM deletion | 0=page |
| `XLOG_BTREE_REUSE_PAGE` | 0xD0 | Page recycling | (conflict only) |
| `XLOG_BTREE_META_CLEANUP` | 0xE0 | Meta cleanup update | 0=meta |
| *(available)* | 0xF0 | **Unused — could be MERGE** | |

### WAL Logging Pattern

```c
// In the CRITICAL SECTION of any B-tree operation:

START_CRIT_SECTION();

// 1. Modify pages
PageAddItem(page, tuple, size, offset);
MarkBufferDirty(buf);

// 2. Register WAL data
XLogBeginInsert();
XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
XLogRegisterData((char *) &xlrec, sizeof(xlrec));
XLogRegisterBufData(0, (char *) tuple, tuple_size);

// 3. Insert WAL record
recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_INSERT_LEAF);

// 4. Update page LSN
PageSetLSN(page, recptr);

END_CRIT_SECTION();
```

### Replay Pattern (in `nbtxlog.c`)

```c
// In btree_redo():
static void btree_xlog_insert(bool isleaf, XLogReaderState *record)
{
    // 1. Get the record data
    xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);

    // 2. Read the buffer (skip if page LSN is already >= record LSN)
    if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
    {
        Page page = BufferGetPage(buf);

        // 3. Get the tuple data from the WAL record
        char *datapos = XLogRecGetBlockData(record, 0, &datalen);
        IndexTuple newitem = (IndexTuple) datapos;

        // 4. Apply the modification
        PageAddItem(page, (Item) newitem, datalen, xlrec->offnum);

        // 5. Update LSN and mark dirty
        PageSetLSN(page, lsn);
        MarkBufferDirty(buf);
    }
    UnlockReleaseBuffer(buf);
}
```

### INCOMPLETE_SPLIT and Crash Safety

Page splits span two WAL records (split + parent insert). If the system
crashes between them:

```
 State after crash recovery:
 - Left page: has INCOMPLETE_SPLIT flag set
 - Right page: exists, has data, is linked to siblings
 - Parent: missing the downlink to right page

 Recovery: The NEXT inserter or VACUUM that encounters the INCOMPLETE_SPLIT
 flag will call _bt_finish_split() to insert the missing downlink.
 Searches work correctly even without the downlink (via right-links).
```

---

## 16. Source File Map

| File | Purpose |
|------|---------|
| `src/backend/access/nbtree/nbtree.c` | Top-level AM interface, VACUUM entry points |
| `src/backend/access/nbtree/nbtinsert.c` | Insertion, page splits, deduplication |
| `src/backend/access/nbtree/nbtsearch.c` | Tree descent, scanning, binary search |
| `src/backend/access/nbtree/nbtpage.c` | Page management, buffer access, page deletion |
| `src/backend/access/nbtree/nbtxlog.c` | WAL replay for all B-tree operations |
| `src/backend/access/nbtree/nbtsort.c` | Bulk loading (CREATE INDEX) |
| `src/backend/access/nbtree/nbtutils.c` | Utilities: kill items, scan keys, compare |
| `src/backend/access/nbtree/nbtvalidate.c` | Operator class validation |
| `src/backend/access/nbtree/nbtcompare.c` | Comparison functions for built-in types |
| `src/backend/access/nbtree/README` | Comprehensive design documentation |
| `src/include/access/nbtree.h` | All B-tree data structures and WAL records |
| `src/include/access/nbtxlog.h` | WAL record type definitions (included by nbtree.h) |
| `src/include/storage/bufpage.h` | Page layout structures |
| `src/include/access/itup.h` | Index tuple structures |
| `src/include/storage/bufmgr.h` | Buffer manager interface |
| `contrib/amcheck/verify_nbtree.c` | B-tree integrity verification (used for testing) |

---

*This document is based on the PostgreSQL 19devel source code (master branch).*
