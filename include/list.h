/**
 * Concurrent Linked List - Public API (BSD-style macros)
 *
 * Lock-free thread-safe singly linked list. Embed LL_ENTRY in your
 * struct and use the macros to operate on any element type. List elements are
 * pointers to your structs; you allocate/free them. Optional free_cb on the
 * head is called when a removed element is safe to free (for REMOVE only).
 *
 * All macros that take a list head expect a *pointer* to the list head (e.g.
 * use a variable of type "struct your_head *", not &list in the macro).
 */

#ifndef LIST_H
#define LIST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Commit ID type (64-bit when available). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
typedef _Atomic(uint64_t) ll_commit_id_t;
#else
typedef _Atomic(unsigned long long) ll_commit_id_t;
#endif


#ifdef __cplusplus
extern "C" {
#endif

/*
 * Embed this in your struct to make it listable. "type" is your struct tag,
 * "name" is the member name for the list link.
 * Example: struct item { int id; LL_ENTRY(item, link); };
 */
#define LL_ENTRY(type, name) atomic_uintptr_t name

/*
 * Declare a list head type. "name" is the struct tag, "type" is the element type (struct tag).
 * The head holds the first element pointer and an optional free callback for
 * reclaimed elements (set to NULL if you manage memory yourself).
 * Example: LL_HEAD(list_head, item) my_list;
 */
#define LL_HEAD(name, type)          \
    struct name {                                \
        atomic_uintptr_t head;                    \
        ll_commit_id_t commit_id;                 \
        void (*free_cb)(struct type *);           \
    }

/*
 * Initialize a list head. Call once before using the list.
 */
#define LL_INIT(headp)                \
    do {                                          \
        ll_init_(&((headp)->head), &((headp)->commit_id)); \
        (headp)->free_cb = NULL;                  \
    } while (0)

/*
 * Insert element at the head. "elm" is a pointer to your struct; "field" is
 * the member name of LL_ENTRY. No allocation; you own "elm".
 */
/*
 * Helper: use statement expression so offsetof(..., field) comma doesn't
 * split function arguments. Requires GCC/clang (GNU C).
 */
#define LL_OFFSET(elm, field) \
    ({ __typeof__(elm) _e = (elm); (size_t)offsetof(__typeof__(*_e), field); })

#define LL_INSERT_HEAD(headp, elm, field)                   \
    ll_insert_head_(&((headp)->head), &((headp)->commit_id), (void *)(elm))

/*
 * Insert element at the tail.
 */
#define LL_INSERT_TAIL(headp, elm, field)                   \
    ll_insert_tail_(&((headp)->head), &((headp)->commit_id), (void *)(elm))

/*
 * Insert element after the node containing after_elm (by pointer). Lock-free;
 * uses current commit_id snapshot to find after_elm. No-op if after_elm not in list.
 */
#define LL_INSERT_AFTER(headp, after_elm, elm, field)                       \
    ll_insert_after_(&((headp)->head), &((headp)->commit_id), (void *)(after_elm), (void *)(elm))

/*
 * Remove and return the element at the head. Returns NULL if empty.
 * "type" is the element struct type; "field" is the list entry member name.
 * Caller may free the returned element when no longer needed.
 */
#define LL_REMOVE_HEAD(headp, type, field)                  \
    ((type *)ll_remove_head_(&((headp)->head), &((headp)->commit_id)))

/*
 * Remove the given element from the list. If head->free_cb is set, it will be
 * called when the element is safe to free (after reclaim); otherwise you must
 * not free the element until no thread can reference it (e.g. by design).
 */
#define LL_REMOVE(headp, elm, field)                        \
    ll_remove_(&((headp)->head), &((headp)->commit_id), (void (*)(void *))(headp)->free_cb, (void *)(elm))

/*
 * Return true if "elm" is in the list (by pointer equality).
 */
#define LL_CONTAINS(headp, elm, field)                       \
    ll_contains_(&((headp)->head), &((headp)->commit_id), (void *)(elm))

/*
 * Return true if the list is empty.
 */
#define LL_IS_EMPTY(headp)  ll_is_empty_(&((headp)->head))

/*
 * Return the number of elements (unmarked) in the list. Lock-free snapshot.
 * "type" is the element struct type; "field" is the list entry member name.
 */
#define LL_SIZE(headp, type, field)                         \
    ll_size_(&((headp)->head), &((headp)->commit_id))

/*
 * Iterator for versioned traversal (snapshot at current commit_id).
 * Do not remove elements during iteration.
 */
typedef struct ll_iter {
    int begun;
    void *cur;           /* current versioned_node *; internal */
    atomic_uintptr_t *head;
    ll_commit_id_t *commit_id;
    uint64_t snapshot_version;
} ll_iter_t;

void ll_iter_begin(ll_iter_t *it,
    atomic_uintptr_t *head, ll_commit_id_t *commit_id);
bool ll_iter_has(ll_iter_t *it);
void ll_iter_next(ll_iter_t *it);
void *ll_iter_get(ll_iter_t *it);

/*
 * Traverse the list at current snapshot. "var" is the loop variable.
 */
#define LL_FOREACH(var, headp, type, field)                  \
    for (ll_iter_t _ll_it = {0};                             \
         (void)(!_ll_it.begun && (ll_iter_begin(&_ll_it, &((headp)->head), &((headp)->commit_id)), 0)), \
         ll_iter_has(&_ll_it);                               \
         ll_iter_next(&_ll_it))                              \
        if (((var) = (type *)ll_iter_get(&_ll_it)) != NULL)

/*
 * --- Snapshot visibility ---
 * Each change is tagged with a commit_id (transaction id). When walking or
 * searching, a read uses a snapshot S (e.g. commit_id at txn start or at
 * iter_begin). A node is visible (committed) at S iff insert_txn_id <= S and
 * (removed_txn_id == 0 || removed_txn_id > S). Nodes with removed_txn_id set
 * but not yet reclaimed are logically removed for that snapshot. Thus the
 * snapshot determines which nodes are committed vs in progress of being
 * modified (removed).
 */

/*
 * --- Transactions ---
 * Start a transaction to see a snapshot of the list and buffer inserts/removes.
 * Other threads can keep modifying the list; you see contents as of txn start.
 * Commit applies your changes to the list; rollback discards them.
 * Insert/remove anywhere: head, tail, or after a given element.
 * Only one thread should use a given txn at a time.
 */

typedef struct ll_txn ll_txn_t;

/** Callback for LL_TXN_FOREACH: (element, userdata). */
typedef void (*ll_txn_foreach_fn)(void *elm, void *userdata);

/**
 * Start a transaction. Captures a snapshot of the list; other threads can
 * continue to add/remove. Returns NULL on allocation failure.
 */
#define LL_TXN_START(headp, type, field)                     \
    ll_txn_start_(&((headp)->head), &((headp)->commit_id), (void (*)(void *))(headp)->free_cb)

/**
 * Insert at head (in transaction view). Applied to the list on commit.
 */
#define LL_TXN_INSERT_HEAD(txn, elm, field)                   \
    ll_txn_insert_head_((txn), (void *)(elm))

/**
 * Insert at tail (in transaction view). Applied to the list on commit.
 */
#define LL_TXN_INSERT_TAIL(txn, elm, field)                   \
    ll_txn_insert_tail_((txn), (void *)(elm))

/**
 * Insert elm after after_elm (in transaction view). Applied to the list on commit.
 * Multiple insert_after with the same anchor are applied in call order.
 */
#define LL_TXN_INSERT_AFTER(txn, after_elm, elm, field)       \
    ll_txn_insert_after_((txn), (void *)(after_elm), (void *)(elm))

/**
 * Remove element from the transaction view. Applied to the list on commit
 * (no-op if element was not in the snapshot or was already removed).
 */
#define LL_TXN_REMOVE(txn, elm, field)                        \
    ll_txn_remove_((txn), (void *)(elm))

/**
 * Return true if elm is in the transaction view (snapshot minus removed,
 * plus inserted).
 */
#define LL_TXN_CONTAINS(txn, elm, field)                      \
    ll_txn_contains_((txn), (void *)(elm))

/**
 * Call cb(elm, userdata) for each element in the transaction view, in order.
 */
#define LL_TXN_FOREACH(txn, cb, userdata)                    \
    ll_txn_foreach_((txn), (cb), (userdata))

/**
 * Commit: apply all buffered removes then inserts to the list. Frees the txn;
 * do not use txn after this. Returns 0 on success.
 */
int ll_txn_commit(ll_txn_t *txn);

/**
 * Rollback: discard all buffered changes and free the txn. Do not use txn after this.
 */
void ll_txn_rollback(ll_txn_t *txn);

/* Internal (used by macros). */
ll_txn_t *ll_txn_start_(atomic_uintptr_t *head,
    ll_commit_id_t *commit_id, void (*free_cb)(void *));
void ll_txn_insert_head_(ll_txn_t *txn, void *elm);
void ll_txn_insert_tail_(ll_txn_t *txn, void *elm);
void ll_txn_insert_after_(ll_txn_t *txn, void *after_elm, void *elm);
void ll_txn_remove_(ll_txn_t *txn, void *elm);
bool ll_txn_contains_(ll_txn_t *txn, const void *elm);
void ll_txn_foreach_(ll_txn_t *txn,
    ll_txn_foreach_fn cb, void *userdata);

/* Internal API: list uses versioned wrappers; commit_id tags each change. */
void ll_init_(atomic_uintptr_t *head, ll_commit_id_t *commit_id);
void ll_insert_head_(atomic_uintptr_t *head, ll_commit_id_t *commit_id, void *elm);
void ll_insert_tail_(atomic_uintptr_t *head, ll_commit_id_t *commit_id, void *elm);
void ll_insert_after_(atomic_uintptr_t *head, ll_commit_id_t *commit_id, void *after_elm, void *elm);
void *ll_remove_head_(atomic_uintptr_t *head, ll_commit_id_t *commit_id);
int ll_remove_(atomic_uintptr_t *head, ll_commit_id_t *commit_id, void (*free_cb)(void *), void *elm);
bool ll_contains_(atomic_uintptr_t *head, ll_commit_id_t *commit_id, const void *elm);
bool ll_is_empty_(atomic_uintptr_t *head);
size_t ll_size_(atomic_uintptr_t *head, ll_commit_id_t *commit_id);

#ifdef __cplusplus
}
#endif

#endif /* LIST_H */
