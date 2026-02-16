/**
 * Lock-free concurrent linked list (Harris-style with hazard pointers).
 * Generic: works on any struct via (void *elm, size_t offset) to the le_next field.
 */

#include "concurrent_list.h"
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#define MARK_BIT 1UL
#define PTR_MASK (~(uintptr_t)MARK_BIT)

static inline void *get_ptr(uintptr_t u)
{
    return (void *)(u & PTR_MASK);
}

static inline int is_marked(uintptr_t u)
{
    return (int)(u & MARK_BIT);
}

#define ELM_NEXT(elm, off) ((atomic_uintptr_t *)((char *)(elm) + (off)))

void concurrent_list_init_(atomic_uintptr_t *head)
{
    atomic_store_explicit(head, (uintptr_t)0, memory_order_release);
}

/* Hazard pointer support */
#define MAX_HP_THREADS 64
static _Atomic(void *) hazard_ptrs[MAX_HP_THREADS];
static _Atomic(int) hp_next_index;
static _Thread_local int my_hp_index = -1;

static int get_hp_index(void)
{
    if (my_hp_index >= 0)
        return my_hp_index;
    int i = atomic_fetch_add(&hp_next_index, 1);
    if (i >= MAX_HP_THREADS)
        return -1;
    my_hp_index = i;
    return i;
}

static void hp_acquire(void *p)
{
    int i = get_hp_index();
    if (i >= 0)
        atomic_store_explicit(&hazard_ptrs[i], p, memory_order_release);
}

static void hp_release(void)
{
    int i = get_hp_index();
    if (i >= 0)
        atomic_store_explicit(&hazard_ptrs[i], NULL, memory_order_release);
}

static int hp_can_retire(void *p)
{
    for (int i = 0; i < MAX_HP_THREADS; i++) {
        if (atomic_load_explicit(&hazard_ptrs[i], memory_order_acquire) == p)
            return 0;
    }
    return 1;
}

/* Retire list: when free_cb is set we call it on reclaim. One free_cb per thread (last passed). */
#define RETIRE_CAP 256
static _Thread_local void *retire_ptrs[RETIRE_CAP];
static _Thread_local int retire_count;
static _Thread_local void (*retire_cb)(void *);

static void retire_node(void *elm, void (*free_cb)(void *))
{
    retire_cb = free_cb;
    if (retire_count >= RETIRE_CAP) {
        int j = 0;
        for (int i = 0; i < retire_count; i++) {
            void *p = retire_ptrs[i];
            if (hp_can_retire(p)) {
                if (retire_cb)
                    retire_cb(p);
            } else {
                retire_ptrs[j++] = p;
            }
        }
        retire_count = j;
    }
    if (retire_count < RETIRE_CAP)
        retire_ptrs[retire_count++] = elm;
}

void concurrent_list_insert_head_(atomic_uintptr_t *head, void *elm, size_t offset)
{
    atomic_uintptr_t *elm_next = ELM_NEXT(elm, offset);
    uintptr_t old_head;
    do {
        old_head = atomic_load_explicit(head, memory_order_acquire);
        atomic_store_explicit(elm_next, old_head, memory_order_release);
    } while (!atomic_compare_exchange_weak_explicit(head, &old_head, (uintptr_t)elm,
                                                    memory_order_release, memory_order_acquire));
}

void concurrent_list_insert_tail_(atomic_uintptr_t *head, void *elm, size_t offset)
{
    atomic_uintptr_t *elm_next = ELM_NEXT(elm, offset);
    atomic_store_explicit(elm_next, (uintptr_t)NULL, memory_order_release);
    for (;;) {
        uintptr_t head_val = atomic_load_explicit(head, memory_order_acquire);
        void *head_node = get_ptr(head_val);
        if (!head_node) {
            if (atomic_compare_exchange_weak_explicit(head, &head_val, (uintptr_t)elm,
                                                      memory_order_release, memory_order_acquire))
                return;
            continue;
        }
        hp_acquire(head_node);
        if (atomic_load_explicit(head, memory_order_acquire) != head_val) {
            hp_release();
            continue;
        }
        void *curr = head_node;
        for (;;) {
            uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
            void *next_node = get_ptr(next_val);
            if (is_marked(next_val)) {
                curr = next_node;
                continue;
            }
            if (!next_node)
                break;
            hp_acquire(next_node);
            curr = next_node;
        }
        uintptr_t expected = (uintptr_t)NULL;
        if (atomic_compare_exchange_weak_explicit(ELM_NEXT(curr, offset), &expected, (uintptr_t)elm,
                                                  memory_order_release, memory_order_acquire)) {
            hp_release();
            return;
        }
        hp_release();
    }
}

void *concurrent_list_remove_head_(atomic_uintptr_t *head, size_t offset)
{
    void *head_node;
    uintptr_t head_val;
    do {
        head_val = atomic_load_explicit(head, memory_order_acquire);
        head_node = get_ptr(head_val);
        if (!head_node)
            return NULL;
        uintptr_t next_val = atomic_load_explicit(ELM_NEXT(head_node, offset), memory_order_acquire);
        if (is_marked(next_val))
            continue;
        if (!atomic_compare_exchange_weak_explicit(head, &head_val, next_val,
                                                    memory_order_release, memory_order_acquire))
            continue;
        return head_node;
    } while (1);
}

static int mark_node(void *node, size_t offset)
{
    atomic_uintptr_t *node_next = ELM_NEXT(node, offset);
    uintptr_t next_val;
    do {
        next_val = atomic_load_explicit(node_next, memory_order_acquire);
        if (is_marked(next_val))
            return 0;
    } while (!atomic_compare_exchange_weak_explicit(node_next, &next_val, next_val | MARK_BIT,
                                                    memory_order_release, memory_order_acquire));
    return 1;
}

int concurrent_list_remove_(atomic_uintptr_t *head, void (*free_cb)(void *), void *elm, size_t offset)
{
    for (;;) {
        uintptr_t head_val = atomic_load_explicit(head, memory_order_acquire);
        void *head_node = get_ptr(head_val);
        if (!head_node)
            return -1;
        hp_acquire(head_node);
        if (atomic_load_explicit(head, memory_order_acquire) != head_val) {
            hp_release();
            continue;
        }
        void *curr = head_node;
        void *prev = NULL;
        atomic_uintptr_t *atom_ptr = head;
        uintptr_t expected = head_val;
        for (;;) {
            uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
            void *next_node = get_ptr(next_val);
            if (is_marked(next_val)) {
                uintptr_t unmarked = (uintptr_t)next_node;
                if (!atomic_compare_exchange_weak_explicit(atom_ptr, &expected, unmarked,
                                                          memory_order_release, memory_order_acquire))
                    break;
                curr = next_node;
                if (!curr) {
                    hp_release();
                    return -1;
                }
                continue;
            }
            if (curr == elm) {
                if (!mark_node(curr, offset)) {
                    hp_release();
                    continue;
                }
                uintptr_t unmarked_next = (uintptr_t)next_node;
                if (atomic_compare_exchange_weak_explicit(atom_ptr, &expected, unmarked_next,
                                                          memory_order_release, memory_order_acquire)) {
                    hp_release();
                    retire_node(curr, free_cb);
                    return 0;
                }
                hp_release();
                continue;
            }
            prev = curr;
            atom_ptr = ELM_NEXT(curr, offset);
            expected = next_val;
            curr = next_node;
            if (!curr) {
                hp_release();
                return -1;
            }
            hp_acquire(curr);
        }
        hp_release();
    }
}

bool concurrent_list_contains_(atomic_uintptr_t *head, const void *elm, size_t offset)
{
    void *curr = get_ptr(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
        if (!is_marked(next_val) && curr == elm)
            return true;
        curr = get_ptr(next_val);
    }
    return false;
}

bool concurrent_list_is_empty_(atomic_uintptr_t *head)
{
    return get_ptr(atomic_load_explicit(head, memory_order_acquire)) == NULL;
}

size_t concurrent_list_size_(atomic_uintptr_t *head, size_t offset)
{
    size_t n = 0;
    void *curr = get_ptr(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
        if (!is_marked(next_val))
            n++;
        curr = get_ptr(next_val);
    }
    return n;
}

void *concurrent_list_first_(atomic_uintptr_t *head, size_t offset)
{
    void *curr = get_ptr(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
        if (!is_marked(next_val))
            return curr;
        curr = get_ptr(next_val);
    }
    return NULL;
}

void *concurrent_list_next_(void *elm, size_t offset)
{
    if (!elm)
        return NULL;
    void *curr = get_ptr(atomic_load_explicit(ELM_NEXT(elm, offset), memory_order_acquire));
    while (curr) {
        uintptr_t next_val = atomic_load_explicit(ELM_NEXT(curr, offset), memory_order_acquire);
        if (!is_marked(next_val))
            return curr;
        curr = get_ptr(next_val);
    }
    return NULL;
}
