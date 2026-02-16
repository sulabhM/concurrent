# Lock-free concurrent linked list (C, BSD-style macros)

A lock-free, thread-safe singly linked list in C using C11 atomics. The API is generic: embed a list entry in any struct and use BSD-style macros to operate on the list.

## Features

- **Lock-free**: Harris-style list with hazard pointers; no mutexes.
- **Generic**: Works with any struct; you define the element type and embed `CONCURRENT_LIST_ENTRY(type, name)`.
- **BSD-style macros**: Similar to `sys/queue.h` (e.g. `CONCURRENT_LIST_INSERT_HEAD`, `CONCURRENT_LIST_REMOVE_HEAD`, `CONCURRENT_LIST_FOREACH`).

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
#include "concurrent_list.h"

struct item {
    int value;
    CONCURRENT_LIST_ENTRY(item, link);
};
CONCURRENT_LIST_HEAD(list_head, item);

struct list_head lst;
struct list_head *lst_p = &lst;

CONCURRENT_LIST_INIT(lst_p);

struct item *a = malloc(sizeof(*a));
a->value = 42;
CONCURRENT_LIST_INSERT_HEAD(lst_p, a, link);

struct item *p = CONCURRENT_LIST_REMOVE_HEAD(lst_p, struct item, link);
if (p) free(p);
```

See `include/concurrent_list.h` for the full API and `src/main.c` for a demo.

## Layout

- `include/concurrent_list.h` – Public macro API and internal declarations
- `src/concurrent_list.c` – Lock-free implementation
- `src/main.c` – Demo (single- and multi-threaded)

## License

Use as you like (e.g. MIT/BSD-style).
