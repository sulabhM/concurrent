/**
 * Demo: BSD-style linked list macros with a custom struct.
 */
#include "list.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* Define an element type and embed the list entry. */
struct item {
    int value;
    LL_ENTRY(item, link);
};

/* Declare a list head type for this element type. */
LL_HEAD(list_head, item);

static struct list_head lst;
#define NUM_THREADS 4
#define OPS_PER_THREAD 100

static struct list_head *g_lst;

static void txn_foreach_cb(void *elm, void *userdata)
{
    (void)userdata;
    printf("%d ", ((struct item *)elm)->value);
}

static void *thread_insert_remove(void *arg)
{
    (void)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        struct item *a = malloc(sizeof(*a));
        struct item *b = malloc(sizeof(*b));
        if (a) { a->value = i; LL_INSERT_HEAD(g_lst, a, link); }
        if (b) { b->value = i + 1000; LL_INSERT_TAIL(g_lst, b, link); }
    }
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        struct item *p = LL_REMOVE_HEAD(g_lst, struct item, link);
        if (p)
            free(p);
    }
    return NULL;
}

int main(void)
{
    struct list_head *lst_p = &lst;
    LL_INIT(lst_p);

    /* Single-threaded API usage */
    struct item *a = malloc(sizeof(*a));
    struct item *b = malloc(sizeof(*b));
    struct item *c = malloc(sizeof(*c));
    if (!a || !b || !c) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    a->value = 10;
    b->value = 20;
    c->value = 30;
    LL_INSERT_HEAD(lst_p, a, link);
    LL_INSERT_HEAD(lst_p, b, link);
    LL_INSERT_TAIL(lst_p, c, link);

    /* Insert in middle: after b (value 10) */
    struct item *d = malloc(sizeof(*d));
    if (d) { d->value = 15; LL_INSERT_AFTER(lst_p, b, d, link); }  /* list: b, d, a, c */
    printf("size after inserts: %zu\n", LL_SIZE(lst_p, struct item, link));
    printf("contains b: %d\n", LL_CONTAINS(lst_p, b, link));

    struct item *p;
    while ((p = LL_REMOVE_HEAD(lst_p, struct item, link)) != NULL) {
        printf("popped %d\n", p->value);
        free(p);
    }
    printf("is_empty: %d\n", LL_IS_EMPTY(lst_p));

    /* Multi-threaded stress */
    g_lst = lst_p;
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_insert_remove, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("size after concurrent ops: %zu\n", LL_SIZE(lst_p, struct item, link));

    /* Drain and free remaining */
    while ((p = LL_REMOVE_HEAD(lst_p, struct item, link)) != NULL)
        free(p);

    /* --- Transaction demo --- */
    struct item *x = malloc(sizeof(*x));
    struct item *y = malloc(sizeof(*y));
    struct item *z = malloc(sizeof(*z));
    if (x && y && z) {
        x->value = 1;
        y->value = 2;
        z->value = 3;
        LL_INSERT_TAIL(lst_p, x, link);
        LL_INSERT_TAIL(lst_p, y, link);
        LL_INSERT_TAIL(lst_p, z, link);
    }

    ll_txn_t *txn = LL_TXN_START(lst_p, struct item, link);
    if (txn) {
        printf("txn view (snapshot): ");
        LL_TXN_FOREACH(txn, txn_foreach_cb, NULL);
        printf("\n");

        /* Buffered insert/remove in txn (list unchanged until commit) */
        struct item *w = malloc(sizeof(*w));
        if (w) {
            w->value = 99;
            LL_TXN_INSERT_TAIL(txn, w, link);
        }
        struct item *mid = malloc(sizeof(*mid));
        if (mid) {
            mid->value = 42;
            LL_TXN_INSERT_AFTER(txn, x, mid, link);  /* insert 42 after 1 → view: 1 42 2 3 */
        }
        if (y)
            LL_TXN_REMOVE(txn, y, link);

        printf("txn view after insert 42 after 1, 99 at tail, remove 2: ");
        LL_TXN_FOREACH(txn, txn_foreach_cb, NULL);
        printf("\n");

        ll_txn_commit(txn);
        printf("after commit: size=%zu\n", LL_SIZE(lst_p, struct item, link));
    }

    /* Rollback demo: txn changes are discarded */
    if (x && z) {
        txn = LL_TXN_START(lst_p, struct item, link);
        if (txn) {
            struct item *tmp = malloc(sizeof(*tmp));
            if (tmp) {
                tmp->value = 100;
                LL_TXN_INSERT_HEAD(txn, tmp, link);
            }
            LL_TXN_REMOVE(txn, x, link);
            ll_txn_rollback(txn);
            if (tmp)
                free(tmp);
        }
        printf("after rollback: size=%zu (unchanged)\n", LL_SIZE(lst_p, struct item, link));
    }

    while ((p = LL_REMOVE_HEAD(lst_p, struct item, link)) != NULL)
        free(p);

    return 0;
}
