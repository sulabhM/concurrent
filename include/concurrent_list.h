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
