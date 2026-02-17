# Lock-free concurrent linked list (C, BSD-style macros)

A lock-free, thread-safe singly linked list in C using C11 atomics. The API is generic: embed a list entry in any struct and use BSD-style macros to operate on the list.

## Features

- **Lock-free**: Harris-style list with hazard pointers; no mutexes.
- **Generic**: Works with any struct; you define the element type and embed `LL_ENTRY(type, name)`.
- **BSD-style macros**: Similar to `sys/queue.h` (e.g. `LL_INSERT_HEAD`, `LL_REMOVE_HEAD`, `LL_FOREACH`).
- **Transactions**: Snapshot is defined by a **commit ID** (no copy of nodes). Each change (insert/remove) is tagged with a monotonic commit ID. A snapshot at ID S sees all nodes with `insert_txn_id <= S` and not removed at or before S (`removed_txn_id == 0 || removed_txn_id > S`). Starting a transaction records the current commit ID; you walk the list filtered by that ID. Buffered inserts/removes are applied on commit (with a new ID) or discarded on rollback.

## Build

```bash
mkdir -p build && cd build
cmake ..
make
./c_project
```

Requires a C11 compiler (e.g. GCC or Clang) and pthreads for the demo.

## Quick example

```c
#include "list.h"

struct item {
    int value;
    LL_ENTRY(item, link);
};
LL_HEAD(list_head, item);

struct list_head lst;
struct list_head *lst_p = &lst;

LL_INIT(lst_p);

struct item *a = malloc(sizeof(*a));
a->value = 42;
LL_INSERT_HEAD(lst_p, a, link);

struct item *p = LL_REMOVE_HEAD(lst_p, struct item, link);
if (p) free(p);
```

### Transactions (ID-based snapshot)

The list uses **versioned wrappers**: each element is stored in a wrapper with `insert_txn_id` and `removed_txn_id`. The list head has a `commit_id` that increments on every commit. A snapshot at ID S = "all changes committed with id ≤ S": a node is visible iff `insert_txn_id ≤ S` and (`removed_txn_id == 0` or `removed_txn_id > S`). No copy of nodes: you traverse the list and filter by S.

```c
ll_txn_t *txn = LL_TXN_START(lst_p, struct item, link);
if (txn) {
    /* Snapshot = list at commit_id when txn started; other threads can add/remove */
    LL_TXN_FOREACH(txn, my_callback, userdata);
    LL_TXN_INSERT_TAIL(txn, new_elm, link);
    LL_TXN_REMOVE(txn, old_elm, link);
    ll_txn_commit(txn);   /* gets new commit_id, applies changes */
    /* or ll_txn_rollback(txn); to discard */
}
```

See `include/list.h` for the full API and `src/main.c` for a demo.

## Layout

- `include/list.h` – Public macro API and internal declarations
- `src/list.c` – Lock-free implementation
- `src/main.c` – Demo (single- and multi-threaded)

## License

Use as you like (e.g. MIT/BSD-style).
