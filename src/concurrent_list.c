/**
 * Versioned concurrent linked list.
 * Each change is tagged with a commit_id. Snapshot at S = all nodes with
 * insert_txn_id <= S and (removed_txn_id == 0 || removed_txn_id > S).
 * No copy of nodes for transactions; snapshot is defined by ID.
 */

#include "concurrent_list.h"
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

/* Versioned wrapper: list chains these; each holds user element + version ids. */
typedef struct versioned_node {
    void *user_elm;
    uint64_t insert_txn_id;
    _Atomic uint64_t removed_txn_id;  /* 0 = not removed */
    atomic_uintptr_t next;
} versioned_node_t;

static inline versioned_node_t *get_wrapper(uintptr_t u)
{
    return (versioned_node_t *)(u & ~(uintptr_t)1UL);
}

static int visible(versioned_node_t *w, uint64_t snapshot_version)
{
    if (!w)
        return 0;
    uint64_t rid = atomic_load_explicit(&w->removed_txn_id, memory_order_acquire);
    return w->insert_txn_id <= snapshot_version && (rid == 0 || rid > snapshot_version);
}

void concurrent_list_init_(atomic_uintptr_t *head, cl_commit_id_t *commit_id)
{
    atomic_store_explicit(head, (uintptr_t)0, memory_order_release);
    atomic_store_explicit(commit_id, 1, memory_order_release);
}

/* Hazard pointers: 2 slots per thread so we can hold prev and curr during traversal. */
#define MAX_HP_THREADS 32
#define HP_SLOTS_PER_THREAD 2
static _Atomic(void *) hazard_ptrs[MAX_HP_THREADS * HP_SLOTS_PER_THREAD];
static _Atomic(int) hp_next_index;
static _Thread_local int my_hp_base = -1;

static int get_hp_base(void)
{
    if (my_hp_base >= 0)
        return my_hp_base;
    int i = atomic_fetch_add(&hp_next_index, 1);
    if (i >= MAX_HP_THREADS)
        return -1;
    my_hp_base = i * HP_SLOTS_PER_THREAD;
    return my_hp_base;
}

static void hp_acquire(void *p)
{
    int i = get_hp_base();
    if (i >= 0)
        atomic_store_explicit(&hazard_ptrs[i], p, memory_order_release);
}

static void hp_acquire_1(void *p)
{
    int i = get_hp_base();
    if (i >= 0)
        atomic_store_explicit(&hazard_ptrs[i + 1], p, memory_order_release);
}

static void hp_release(void)
{
    int i = get_hp_base();
    if (i >= 0) {
        atomic_store_explicit(&hazard_ptrs[i], NULL, memory_order_release);
        atomic_store_explicit(&hazard_ptrs[i + 1], NULL, memory_order_release);
    }
}

/* Active snapshot versions for reclaim: only free nodes removed before min(active). */
static _Atomic(uint64_t) active_snapshot_version[MAX_HP_THREADS];

static uint64_t min_active_snapshot(void)
{
    uint64_t min = UINT64_MAX;
    for (int i = 0; i < MAX_HP_THREADS; i++) {
        uint64_t v = atomic_load_explicit(&active_snapshot_version[i], memory_order_acquire);
        if (v != 0 && v < min)
            min = v;
    }
    return min;  /* UINT64_MAX if no active txns */
}

/* Thread-local list of unlinked nodes waiting to be freed when no hp references them. */
static _Thread_local versioned_node_t *retired_list;

static int any_hp_equals(void *p)
{
    for (int i = 0; i < MAX_HP_THREADS * HP_SLOTS_PER_THREAD; i++) {
        if (atomic_load_explicit(&hazard_ptrs[i], memory_order_acquire) == p)
            return 1;
    }
    return 0;
}

static void reclaim(atomic_uintptr_t *head, cl_commit_id_t *commit_id, void (*free_cb)(void *))
{
    uint64_t min_active = min_active_snapshot();
    if (min_active == UINT64_MAX)
        min_active = atomic_load_explicit(commit_id, memory_order_acquire);
    versioned_node_t *prev = NULL;
    uintptr_t prev_next = atomic_load_explicit(head, memory_order_acquire);
    versioned_node_t *curr = get_wrapper(prev_next);
    while (curr) {
        uint64_t rid = atomic_load_explicit(&curr->removed_txn_id, memory_order_acquire);
        int reclaimable = (rid != 0 && rid < min_active);
        uintptr_t next_val = atomic_load_explicit(&curr->next, memory_order_acquire);
        versioned_node_t *next = get_wrapper(next_val);
        if (reclaimable) {
            hp_acquire(curr);
            uintptr_t unlink_val = (uintptr_t)curr;
            int unlinked = 0;
            if (prev) {
                if (atomic_compare_exchange_weak_explicit(&prev->next, &unlink_val, next_val,
                                                          memory_order_release, memory_order_acquire))
                    unlinked = 1;
            } else {
                if (atomic_compare_exchange_weak_explicit(head, &unlink_val, next_val,
                                                          memory_order_release, memory_order_acquire))
                    unlinked = 1;
            }
            if (unlinked) {
                hp_release();
                /* Push to retire list; free only when no hazard ptr references this node. */
                atomic_store_explicit(&curr->next, (uintptr_t)retired_list, memory_order_release);
                retired_list = curr;
                curr = next;
                continue;
            }
            hp_release();
        }
        prev = curr;
        prev_next = (uintptr_t)curr;
        curr = next;
    }
    /* Free retired nodes that no hazard ptr references. */
    versioned_node_t *still_held = NULL;
    while (retired_list) {
        versioned_node_t *n = retired_list;
        retired_list = get_wrapper(atomic_load_explicit(&n->next, memory_order_acquire));
        if (any_hp_equals(n)) {
            atomic_store_explicit(&n->next, (uintptr_t)still_held, memory_order_release);
            still_held = n;
        } else {
            void *user = n->user_elm;
            free(n);
            if (free_cb)
                free_cb(user);
        }
    }
    retired_list = still_held;
}

void concurrent_list_insert_head_(atomic_uintptr_t *head, cl_commit_id_t *commit_id, void *elm)
{
    uint64_t C = atomic_fetch_add_explicit(commit_id, 1, memory_order_acq_rel);
    versioned_node_t *w = (versioned_node_t *)aligned_alloc(alignof(versioned_node_t), sizeof(versioned_node_t));
    if (!w)
        return;
    w->user_elm = elm;
    w->insert_txn_id = C;
    atomic_store_explicit(&w->removed_txn_id, (uint64_t)0, memory_order_release);
    uintptr_t old_head;
    do {
        old_head = atomic_load_explicit(head, memory_order_acquire);
        atomic_store_explicit(&w->next, old_head, memory_order_release);
    } while (!atomic_compare_exchange_weak_explicit(head, &old_head, (uintptr_t)w,
                                                    memory_order_release, memory_order_acquire));
}

void concurrent_list_insert_tail_(atomic_uintptr_t *head, cl_commit_id_t *commit_id, void *elm)
{
    uint64_t C = atomic_fetch_add_explicit(commit_id, 1, memory_order_acq_rel);
    versioned_node_t *w = (versioned_node_t *)aligned_alloc(alignof(versioned_node_t), sizeof(versioned_node_t));
    if (!w)
        return;
    w->user_elm = elm;
    w->insert_txn_id = C;
    atomic_store_explicit(&w->removed_txn_id, (uint64_t)0, memory_order_release);
    atomic_store_explicit(&w->next, (uintptr_t)0, memory_order_release);
    for (;;) {
        uintptr_t head_val = atomic_load_explicit(head, memory_order_acquire);
        versioned_node_t *curr = get_wrapper(head_val);
        if (!curr) {
            if (atomic_compare_exchange_weak_explicit(head, &head_val, (uintptr_t)w,
                                                      memory_order_release, memory_order_acquire))
                return;
            continue;
        }
        hp_acquire(curr);
        if (atomic_load_explicit(head, memory_order_acquire) != head_val) {
            hp_release();
            continue;
        }
        versioned_node_t *prev = curr;
        for (;;) {
            uintptr_t next_val = atomic_load_explicit(&prev->next, memory_order_acquire);
            versioned_node_t *next = get_wrapper(next_val);
            if (!next)
                break;
            hp_acquire(next);
            prev = next;
        }
        uintptr_t expected = (uintptr_t)0;
        if (atomic_compare_exchange_weak_explicit(&prev->next, &expected, (uintptr_t)w,
                                                  memory_order_release, memory_order_acquire)) {
            hp_release();
            return;
        }
        hp_release();
    }
}

void *concurrent_list_remove_head_(atomic_uintptr_t *head, cl_commit_id_t *commit_id)
{
    uint64_t S = atomic_load_explicit(commit_id, memory_order_acquire);
    for (;;) {
        uintptr_t head_val = atomic_load_explicit(head, memory_order_acquire);
        versioned_node_t *w = get_wrapper(head_val);
        if (!w)
            return NULL;
        hp_acquire(w);
        if (atomic_load_explicit(head, memory_order_acquire) != head_val) {
            hp_release();
            continue;
        }
        if (visible(w, S)) {
            uintptr_t next_val = atomic_load_explicit(&w->next, memory_order_acquire);
            if (atomic_compare_exchange_weak_explicit(head, &head_val, next_val,
                                                      memory_order_release, memory_order_acquire)) {
                void *user = w->user_elm;
                hp_release();
                free(w);
                return user;
            }
            hp_release();
            continue;
        }
        /* Head not visible; traverse to find first visible and unlink it. */
        versioned_node_t *prev = w;
        versioned_node_t *curr = get_wrapper(atomic_load_explicit(&w->next, memory_order_acquire));
        int cas_failed = 0;
        while (curr) {
            hp_acquire_1(curr);
            if (visible(curr, S)) {
                uintptr_t unmarked = (uintptr_t)get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
                uintptr_t prev_next = (uintptr_t)curr;
                if (atomic_compare_exchange_weak_explicit(&prev->next, &prev_next, unmarked,
                                                          memory_order_release, memory_order_acquire)) {
                    void *user = curr->user_elm;
                    hp_release();
                    free(curr);
                    return user;
                }
                cas_failed = 1;
                break;
            }
            prev = curr;
            curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
        }
        hp_release();
        if (cas_failed)
            continue;  /* Retry outer loop */
        return NULL;   /* No visible node (list logically empty) */
    }
}

int concurrent_list_remove_(atomic_uintptr_t *head, cl_commit_id_t *commit_id,
                            void (*free_cb)(void *), void *elm)
{
    (void)free_cb;
    uint64_t C = atomic_fetch_add_explicit(commit_id, 1, memory_order_acq_rel);
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        if (curr->user_elm == elm) {
            atomic_store_explicit(&curr->removed_txn_id, C, memory_order_release);
            return 0;
        }
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
    return -1;
}

bool concurrent_list_contains_(atomic_uintptr_t *head, cl_commit_id_t *commit_id, const void *elm)
{
    uint64_t S = atomic_load_explicit(commit_id, memory_order_acquire);
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        if (curr->user_elm == elm && visible(curr, S))
            return true;
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
    return false;
}

bool concurrent_list_is_empty_(atomic_uintptr_t *head)
{
    return get_wrapper(atomic_load_explicit(head, memory_order_acquire)) == NULL;
}

size_t concurrent_list_size_(atomic_uintptr_t *head, cl_commit_id_t *commit_id)
{
    uint64_t S = atomic_load_explicit(commit_id, memory_order_acquire);
    size_t n = 0;
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(head, memory_order_acquire));
    while (curr) {
        if (visible(curr, S))
            n++;
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
    return n;
}

/* --- Iterator (snapshot at current commit_id) --- */

void concurrent_list_iter_begin(concurrent_list_iter_t *it,
    atomic_uintptr_t *head, cl_commit_id_t *commit_id)
{
    it->head = head;
    it->commit_id = commit_id;
    it->snapshot_version = atomic_load_explicit(commit_id, memory_order_acquire);
    it->begun = 1;
    it->cur = get_wrapper(atomic_load_explicit(head, memory_order_acquire));
    while (it->cur && !visible((versioned_node_t *)it->cur, it->snapshot_version))
        it->cur = get_wrapper(atomic_load_explicit(&((versioned_node_t *)it->cur)->next, memory_order_acquire));
}

bool concurrent_list_iter_has(concurrent_list_iter_t *it)
{
    return it->cur != NULL;
}

void concurrent_list_iter_next(concurrent_list_iter_t *it)
{
    if (!it->cur)
        return;
    versioned_node_t *w = (versioned_node_t *)it->cur;
    it->cur = get_wrapper(atomic_load_explicit(&w->next, memory_order_acquire));
    while (it->cur && !visible((versioned_node_t *)it->cur, it->snapshot_version))
        it->cur = get_wrapper(atomic_load_explicit(&((versioned_node_t *)it->cur)->next, memory_order_acquire));
}

void *concurrent_list_iter_get(concurrent_list_iter_t *it)
{
    return it->cur ? ((versioned_node_t *)it->cur)->user_elm : NULL;
}

/* --- Transaction: snapshot = commit_id at start; no copy --- */

#define TXN_INIT_CAP 8

static int append(void ***arr, size_t *n, size_t *cap, void *ptr)
{
    if (*n >= *cap) {
        size_t new_cap = (*cap == 0) ? TXN_INIT_CAP : *cap * 2;
        void **p = (void **)realloc(*arr, new_cap * sizeof(void *));
        if (!p)
            return -1;
        *arr = p;
        *cap = new_cap;
    }
    (*arr)[(*n)++] = ptr;
    return 0;
}

static int ptr_in(void **arr, size_t n, const void *ptr)
{
    for (size_t i = 0; i < n; i++)
        if (arr[i] == ptr)
            return 1;
    return 0;
}

static void remove_from(void **arr, size_t *n, const void *ptr)
{
    for (size_t i = 0; i < *n; i++) {
        if (arr[i] == ptr) {
            (*n)--;
            arr[i] = arr[*n];
            return;
        }
    }
}

struct concurrent_list_txn {
    atomic_uintptr_t *head;
    cl_commit_id_t *commit_id;
    void (*free_cb)(void *);
    uint64_t snapshot_version;   /* snapshot at this id; no copy */
    void **inserted_head;
    size_t n_ins_head;
    size_t cap_ins_head;
    void **inserted_tail;
    size_t n_ins_tail;
    size_t cap_ins_tail;
    void **removed;
    size_t n_removed;
    size_t cap_removed;
};

concurrent_list_txn_t *concurrent_list_txn_start_(atomic_uintptr_t *head,
    cl_commit_id_t *commit_id, void (*free_cb)(void *))
{
    concurrent_list_txn_t *txn = (concurrent_list_txn_t *)calloc(1, sizeof(*txn));
    if (!txn)
        return NULL;
    txn->head = head;
    txn->commit_id = commit_id;
    txn->free_cb = free_cb;
    txn->snapshot_version = atomic_load_explicit(commit_id, memory_order_acquire);
    /* Register so reclaim won't free nodes visible to this snapshot. */
    int base = get_hp_base();
    if (base >= 0)
        atomic_store_explicit(&active_snapshot_version[base / HP_SLOTS_PER_THREAD], txn->snapshot_version, memory_order_release);
    return txn;
}

void concurrent_list_txn_insert_head_(concurrent_list_txn_t *txn, void *elm)
{
    append(&txn->inserted_head, &txn->n_ins_head, &txn->cap_ins_head, elm);
}

void concurrent_list_txn_insert_tail_(concurrent_list_txn_t *txn, void *elm)
{
    append(&txn->inserted_tail, &txn->n_ins_tail, &txn->cap_ins_tail, elm);
}

void concurrent_list_txn_remove_(concurrent_list_txn_t *txn, void *elm)
{
    if (ptr_in(txn->inserted_head, txn->n_ins_head, elm)) {
        remove_from(txn->inserted_head, &txn->n_ins_head, elm);
        return;
    }
    if (ptr_in(txn->inserted_tail, txn->n_ins_tail, elm)) {
        remove_from(txn->inserted_tail, &txn->n_ins_tail, elm);
        return;
    }
    /* Check if elm is in list at snapshot_version (traverse once). */
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(txn->head, memory_order_acquire));
    while (curr) {
        if (curr->user_elm == elm && visible(curr, txn->snapshot_version)) {
            append(&txn->removed, &txn->n_removed, &txn->cap_removed, elm);
            return;
        }
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
}

bool concurrent_list_txn_contains_(concurrent_list_txn_t *txn, const void *elm)
{
    if (ptr_in(txn->inserted_head, txn->n_ins_head, elm))
        return true;
    if (ptr_in(txn->inserted_tail, txn->n_ins_tail, elm))
        return true;
    if (ptr_in(txn->removed, txn->n_removed, elm))
        return false;
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(txn->head, memory_order_acquire));
    while (curr) {
        if (curr->user_elm == elm && visible(curr, txn->snapshot_version))
            return true;
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
    return false;
}

void concurrent_list_txn_foreach_(concurrent_list_txn_t *txn,
    concurrent_list_txn_foreach_fn cb, void *userdata)
{
    /* Snapshot: walk list at snapshot_version (no copy). */
    versioned_node_t *curr = get_wrapper(atomic_load_explicit(txn->head, memory_order_acquire));
    while (curr) {
        if (visible(curr, txn->snapshot_version) && !ptr_in(txn->removed, txn->n_removed, curr->user_elm))
            cb(curr->user_elm, userdata);
        curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
    }
    for (size_t i = txn->n_ins_head; i > 0; i--)
        cb(txn->inserted_head[i - 1], userdata);
    for (size_t i = 0; i < txn->n_ins_tail; i++)
        cb(txn->inserted_tail[i], userdata);
}

int concurrent_list_txn_commit(concurrent_list_txn_t *txn)
{
    uint64_t C = atomic_fetch_add_explicit(txn->commit_id, 1, memory_order_acq_rel);
    for (size_t i = 0; i < txn->n_removed; i++) {
        versioned_node_t *curr = get_wrapper(atomic_load_explicit(txn->head, memory_order_acquire));
        while (curr) {
            if (curr->user_elm == txn->removed[i]) {
                atomic_store_explicit(&curr->removed_txn_id, C, memory_order_release);
                break;
            }
            curr = get_wrapper(atomic_load_explicit(&curr->next, memory_order_acquire));
        }
    }
    for (size_t i = 0; i < txn->n_ins_tail; i++)
        concurrent_list_insert_tail_(txn->head, txn->commit_id, txn->inserted_tail[i]);
    for (size_t i = txn->n_ins_head; i > 0; i--)
        concurrent_list_insert_head_(txn->head, txn->commit_id, txn->inserted_head[i - 1]);
    /* Unregister snapshot, then reclaim removed nodes not visible to any active txn. */
    int base = get_hp_base();
    if (base >= 0)
        atomic_store_explicit(&active_snapshot_version[base / HP_SLOTS_PER_THREAD], (uint64_t)0, memory_order_release);
    reclaim(txn->head, txn->commit_id, txn->free_cb);
    free(txn->inserted_head);
    free(txn->inserted_tail);
    free(txn->removed);
    free(txn);
    return 0;
}

void concurrent_list_txn_rollback(concurrent_list_txn_t *txn)
{
    int base = get_hp_base();
    if (base >= 0)
        atomic_store_explicit(&active_snapshot_version[base / HP_SLOTS_PER_THREAD], (uint64_t)0, memory_order_release);
    free(txn->inserted_head);
    free(txn->inserted_tail);
    free(txn->removed);
    free(txn);
}
