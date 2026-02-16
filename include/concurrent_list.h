/**
 * Concurrent Linked List - Public API (BSD-style macros)
 *
 * Lock-free thread-safe singly linked list. Embed CONCURRENT_LIST_ENTRY in your
 * struct and use the macros to operate on any element type. List elements are
 * pointers to your structs; you allocate/free them. Optional free_cb on the
 * head is called when a removed element is safe to free (for REMOVE only).
 *
 * All macros that take a list head expect a *pointer* to the list head (e.g.
 * use a variable of type "struct your_head *", not &list in the macro).
 */

#ifndef CONCURRENT_LIST_H
#define CONCURRENT_LIST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Embed this in your struct to make it listable. "type" is your struct tag,
 * "name" is the member name for the list link.
 * Example: struct item { int id; CONCURRENT_LIST_ENTRY(item, link); };
 */
#define CONCURRENT_LIST_ENTRY(type, name) atomic_uintptr_t name

/*
 * Declare a list head type. "name" is the struct tag, "type" is the element type (struct tag).
 * The head holds the first element pointer and an optional free callback for
 * reclaimed elements (set to NULL if you manage memory yourself).
 * Example: CONCURRENT_LIST_HEAD(list_head, item) my_list;
 */
#define CONCURRENT_LIST_HEAD(name, type)          \
    struct name {                                \
        atomic_uintptr_t head;                   \
        void (*free_cb)(struct type *);         \
    }

/*
 * Initialize a list head. Call once before using the list.
 */
#define CONCURRENT_LIST_INIT(headp)                \
    do {                                          \
        concurrent_list_init_(&((headp)->head));   \
        (headp)->free_cb = NULL;                    \
    } while (0)

/*
 * Insert element at the head. "elm" is a pointer to your struct; "field" is
 * the member name of CONCURRENT_LIST_ENTRY. No allocation; you own "elm".
 */
/*
 * Helper: use statement expression so offsetof(..., field) comma doesn't
 * split function arguments. Requires GCC/clang (GNU C).
 */
#define CONCURRENT_LIST_OFFSET(elm, field) \
    ({ __typeof__(elm) _e = (elm); (size_t)offsetof(__typeof__(*_e), field); })

#define CONCURRENT_LIST_INSERT_HEAD(headp, elm, field)                   \
    concurrent_list_insert_head_(&((headp)->head), (void *)(elm),        \
        CONCURRENT_LIST_OFFSET(elm, field))

/*
 * Insert element at the tail.
 */
#define CONCURRENT_LIST_INSERT_TAIL(headp, elm, field)                   \
    concurrent_list_insert_tail_(&((headp)->head), (void *)(elm),         \
        CONCURRENT_LIST_OFFSET(elm, field))

/*
 * Remove and return the element at the head. Returns NULL if empty.
 * "type" is the element struct type; "field" is the list entry member name.
 * Caller may free the returned element when no longer needed.
 */
#define CONCURRENT_LIST_REMOVE_HEAD(headp, type, field)                  \
    ((type *)concurrent_list_remove_head_(&((headp)->head), (size_t)offsetof(type, field)))

/*
 * Remove the given element from the list. If head->free_cb is set, it will be
 * called when the element is safe to free (after reclaim); otherwise you must
 * not free the element until no thread can reference it (e.g. by design).
 */
#define CONCURRENT_LIST_REMOVE(headp, elm, field)                        \
    concurrent_list_remove_(&((headp)->head), (headp)->free_cb, (void *)(elm), \
        CONCURRENT_LIST_OFFSET(elm, field))

/*
 * Return true if "elm" is in the list (by pointer equality).
 */
#define CONCURRENT_LIST_CONTAINS(headp, elm, field)                       \
    concurrent_list_contains_(&((headp)->head), (void *)(elm),           \
        CONCURRENT_LIST_OFFSET(elm, field))

/*
 * Return true if the list is empty.
 */
#define CONCURRENT_LIST_IS_EMPTY(headp)  concurrent_list_is_empty_(&((headp)->head))

/*
 * Return the number of elements (unmarked) in the list. Lock-free snapshot.
 * "type" is the element struct type; "field" is the list entry member name.
 */
#define CONCURRENT_LIST_SIZE(headp, type, field)                         \
    concurrent_list_size_(&((headp)->head), (size_t)offsetof(type, field))

/*
 * Traverse the list. "var" is the loop variable (pointer to element); "type"
 * is the element struct type; "field" is the list entry member name. Skips
 * logically deleted nodes. Do not remove "var" from the list during the loop.
 */
#define CONCURRENT_LIST_FOREACH(var, headp, type, field)                  \
    for ((var) = (type *)concurrent_list_first_(&((headp)->head), (size_t)offsetof(type, field)); \
         (var) != NULL;                                                   \
         (var) = (type *)concurrent_list_next_((void *)(var), (size_t)offsetof(type, field)))

/*
 * --- Transactions ---
 * Start a transaction to see a snapshot of the list and buffer inserts/removes.
 * Other threads can keep modifying the list; you see contents as of txn start.
 * Commit applies your changes to the list; rollback discards them.
 * Only one thread should use a given txn at a time.
 */

typedef struct concurrent_list_txn concurrent_list_txn_t;

/** Callback for CONCURRENT_LIST_TXN_FOREACH: (element, userdata). */
typedef void (*concurrent_list_txn_foreach_fn)(void *elm, void *userdata);

/**
 * Start a transaction. Captures a snapshot of the list; other threads can
 * continue to add/remove. Returns NULL on allocation failure.
 */
#define CONCURRENT_LIST_TXN_START(headp, type, field)                     \
    concurrent_list_txn_start_(&((headp)->head), (void (*)(void *))(headp)->free_cb, \
        (size_t)offsetof(type, field))

/**
 * Insert at head (in transaction view). Applied to the list on commit.
 */
#define CONCURRENT_LIST_TXN_INSERT_HEAD(txn, elm, field)                   \
    concurrent_list_txn_insert_head_((txn), (void *)(elm))

/**
 * Insert at tail (in transaction view). Applied to the list on commit.
 */
#define CONCURRENT_LIST_TXN_INSERT_TAIL(txn, elm, field)                  \
    concurrent_list_txn_insert_tail_((txn), (void *)(elm))

/**
 * Remove element from the transaction view. Applied to the list on commit
 * (no-op if element was not in the snapshot or was already removed).
 */
#define CONCURRENT_LIST_TXN_REMOVE(txn, elm, field)                        \
    concurrent_list_txn_remove_((txn), (void *)(elm))

/**
 * Return true if elm is in the transaction view (snapshot minus removed,
 * plus inserted).
 */
#define CONCURRENT_LIST_TXN_CONTAINS(txn, elm, field)                      \
    concurrent_list_txn_contains_((txn), (void *)(elm))

/**
 * Call cb(elm, userdata) for each element in the transaction view, in order.
 */
#define CONCURRENT_LIST_TXN_FOREACH(txn, cb, userdata)                    \
    concurrent_list_txn_foreach_((txn), (cb), (userdata))

/**
 * Commit: apply all buffered removes then inserts to the list. Frees the txn;
 * do not use txn after this. Returns 0 on success.
 */
int concurrent_list_txn_commit(concurrent_list_txn_t *txn);

/**
 * Rollback: discard all buffered changes and free the txn. Do not use txn after this.
 */
void concurrent_list_txn_rollback(concurrent_list_txn_t *txn);

/* Internal (used by macros). */
concurrent_list_txn_t *concurrent_list_txn_start_(atomic_uintptr_t *head,
    void (*free_cb)(void *), size_t offset);
void concurrent_list_txn_insert_head_(concurrent_list_txn_t *txn, void *elm);
void concurrent_list_txn_insert_tail_(concurrent_list_txn_t *txn, void *elm);
void concurrent_list_txn_remove_(concurrent_list_txn_t *txn, void *elm);
bool concurrent_list_txn_contains_(concurrent_list_txn_t *txn, const void *elm);
void concurrent_list_txn_foreach_(concurrent_list_txn_t *txn,
    concurrent_list_txn_foreach_fn cb, void *userdata);

/* Internal API (used by macros; do not call directly with wrong offset). */
void concurrent_list_init_(atomic_uintptr_t *head);
void concurrent_list_insert_head_(atomic_uintptr_t *head, void *elm, size_t offset);
void concurrent_list_insert_tail_(atomic_uintptr_t *head, void *elm, size_t offset);
void *concurrent_list_remove_head_(atomic_uintptr_t *head, size_t offset);
int concurrent_list_remove_(atomic_uintptr_t *head, void (*free_cb)(void *), void *elm, size_t offset);
bool concurrent_list_contains_(atomic_uintptr_t *head, const void *elm, size_t offset);
bool concurrent_list_is_empty_(atomic_uintptr_t *head);
size_t concurrent_list_size_(atomic_uintptr_t *head, size_t offset);
void *concurrent_list_first_(atomic_uintptr_t *head, size_t offset);
void *concurrent_list_next_(void *elm, size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* CONCURRENT_LIST_H */
