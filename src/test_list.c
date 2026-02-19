/**
 * Unit and concurrent tests for the concurrent linked list.
 */
#include "list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct item {
    int value;
    LL_ENTRY(item, link);
};
LL_HEAD(list_head, item);

static int tests_run, tests_failed;

#define RUN_TEST(name, fn) do { \
    tests_run++; \
    if (fn()) { \
        fprintf(stderr, "FAIL: %s\n", name); \
        tests_failed++; \
    } else { \
        printf("  ok %s\n", name); \
    } \
} while (0)

#define ASSERT(c) do { if (!(c)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "  assert %s: %ld != %ld\n", #a " == " #b, (long)(a), (long)(b)); return 1; } } while (0)

/* --- Unit: init, empty, size --- */
static int test_init_empty(void) {
    struct list_head lst;
    LL_INIT(&lst);
    ASSERT(LL_IS_EMPTY(&lst));
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 0);
    return 0;
}

static int test_insert_head_size(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    a->value = 1;
    LL_INSERT_HEAD(&lst, a, link);
    ASSERT(!LL_IS_EMPTY(&lst));
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 1);
    ASSERT(LL_CONTAINS(&lst, a, link));
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == a);
    free(p);
    return 0;
}

static int test_insert_tail_contains(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    struct item *b = malloc(sizeof(*b));
    a->value = 1; b->value = 2;
    LL_INSERT_TAIL(&lst, a, link);
    LL_INSERT_TAIL(&lst, b, link);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 2);
    ASSERT(LL_CONTAINS(&lst, a, link));
    ASSERT(LL_CONTAINS(&lst, b, link));
    ASSERT(!LL_CONTAINS(&lst, (void *)0x1, link));
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == a);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == b);
    free(p);
    return 0;
}

static int test_insert_after_order(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    struct item *b = malloc(sizeof(*b));
    struct item *c = malloc(sizeof(*c));
    a->value = 1; b->value = 2; c->value = 3;
    LL_INSERT_TAIL(&lst, a, link);
    LL_INSERT_TAIL(&lst, b, link);
    LL_INSERT_TAIL(&lst, c, link);
    struct item *m = malloc(sizeof(*m));
    m->value = 99;
    LL_INSERT_AFTER(&lst, a, m, link);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 4);
    /* Order should be a, m, b, c */
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == a && p->value == 1);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == m && p->value == 99);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == b && p->value == 2);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == c && p->value == 3);
    free(p);
    return 0;
}

static int test_remove_by_elm(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    struct item *b = malloc(sizeof(*b));
    a->value = 1; b->value = 2;
    LL_INSERT_TAIL(&lst, a, link);
    LL_INSERT_TAIL(&lst, b, link);
    LL_REMOVE(&lst, a, link);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 1);
    ASSERT(!LL_CONTAINS(&lst, a, link));
    ASSERT(LL_CONTAINS(&lst, b, link));
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == b);
    free(p);
    free(a);
    return 0;
}

static int test_foreach_order(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *e[5];
    for (int i = 0; i < 5; i++) {
        e[i] = malloc(sizeof(struct item));
        e[i]->value = i;
        LL_INSERT_TAIL(&lst, e[i], link);
    }
    int idx = 0;
    struct item *var;
    LL_FOREACH(var, &lst, struct item, link) {
        ASSERT(var == e[idx] && var->value == idx);
        idx++;
    }
    ASSERT_EQ(idx, 5);
    while ((var = LL_REMOVE_HEAD(&lst, struct item, link)) != NULL)
        free(var);
    return 0;
}

static int test_remove_head_empty(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == NULL);
    return 0;
}

static int test_insert_after_nonexistent(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    a->value = 1;
    LL_INSERT_HEAD(&lst, a, link);
    struct item *orphan = malloc(sizeof(*orphan));
    orphan->value = 2;
    LL_INSERT_AFTER(&lst, orphan, orphan, link); /* after_elm not in list: no-op */
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 1);
    free(orphan);
    LL_REMOVE_HEAD(&lst, struct item, link);
    free(a);
    return 0;
}

/* --- Transaction unit tests --- */
static int test_txn_insert_after_commit(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *x = malloc(sizeof(*x));
    struct item *y = malloc(sizeof(*y));
    x->value = 1; y->value = 2;
    LL_INSERT_TAIL(&lst, x, link);
    LL_INSERT_TAIL(&lst, y, link);
    ll_txn_t *txn = LL_TXN_START(&lst, struct item, link);
    ASSERT(txn);
    struct item *z = malloc(sizeof(*z));
    z->value = 42;
    LL_TXN_INSERT_AFTER(txn, x, z, link);
    ASSERT(LL_TXN_CONTAINS(txn, z, link));
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 2); /* list unchanged before commit */
    ll_txn_commit(txn);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 3);
    ASSERT(LL_CONTAINS(&lst, z, link));
    struct item *p;
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == x);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == z);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == y);
    free(p);
    return 0;
}

static int test_txn_rollback_discards(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    a->value = 1;
    LL_INSERT_TAIL(&lst, a, link);
    ll_txn_t *txn = LL_TXN_START(&lst, struct item, link);
    ASSERT(txn);
    struct item *b = malloc(sizeof(*b));
    b->value = 2;
    LL_TXN_INSERT_TAIL(txn, b, link);
    LL_TXN_REMOVE(txn, a, link);
    ll_txn_rollback(txn);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 1);
    ASSERT(LL_CONTAINS(&lst, a, link));
    free(b);
    LL_REMOVE_HEAD(&lst, struct item, link);
    free(a);
    return 0;
}

static int test_txn_multiple_insert_after_same_anchor(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *anchor = malloc(sizeof(*anchor));
    anchor->value = 0;
    LL_INSERT_TAIL(&lst, anchor, link);
    ll_txn_t *txn = LL_TXN_START(&lst, struct item, link);
    ASSERT(txn);
    struct item *u = malloc(sizeof(*u));
    struct item *v = malloc(sizeof(*v));
    u->value = 1; v->value = 2;
    LL_TXN_INSERT_AFTER(txn, anchor, u, link);
    LL_TXN_INSERT_AFTER(txn, anchor, v, link); /* v after anchor in txn = after u when applied */
    ll_txn_commit(txn);
    /* Order: anchor, u, v */
    struct item *p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == anchor);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == u);
    free(p);
    p = LL_REMOVE_HEAD(&lst, struct item, link);
    ASSERT(p == v);
    free(p);
    return 0;
}

static int test_txn_remove_inserted_after(void) {
    struct list_head lst;
    LL_INIT(&lst);
    struct item *a = malloc(sizeof(*a));
    a->value = 1;
    LL_INSERT_TAIL(&lst, a, link);
    ll_txn_t *txn = LL_TXN_START(&lst, struct item, link);
    struct item *b = malloc(sizeof(*b));
    b->value = 2;
    LL_TXN_INSERT_AFTER(txn, a, b, link);
    LL_TXN_REMOVE(txn, b, link);
    ASSERT(!LL_TXN_CONTAINS(txn, b, link));
    ll_txn_commit(txn);
    ASSERT_EQ(LL_SIZE(&lst, struct item, link), 1);
    free(b);
    LL_REMOVE_HEAD(&lst, struct item, link);
    free(a);
    return 0;
}

/* --- Concurrent tests --- */
#define CONCURRENT_THREADS 8
#define CONCURRENT_OPS     200

static struct list_head *conc_lst;

static void *thread_mixed_ops(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < CONCURRENT_OPS; i++) {
        struct item *a = malloc(sizeof(*a));
        struct item *b = malloc(sizeof(*b));
        if (a) { a->value = (int)(id * 10000 + i);       LL_INSERT_HEAD(conc_lst, a, link); }
        if (b) { b->value = (int)(id * 10000 + i + 500); LL_INSERT_TAIL(conc_lst, b, link); }
    }
    for (int i = 0; i < CONCURRENT_OPS; i++) {
        struct item *p = LL_REMOVE_HEAD(conc_lst, struct item, link);
        if (p) free(p);
    }
    return NULL;
}

static int test_concurrent_mixed_head_tail(void) {
    struct list_head lst;
    LL_INIT(&lst);
    conc_lst = &lst;
    pthread_t th[CONCURRENT_THREADS];
    for (int i = 0; i < CONCURRENT_THREADS; i++)
        pthread_create(&th[i], NULL, thread_mixed_ops, (void *)(long)i);
    for (int i = 0; i < CONCURRENT_THREADS; i++)
        pthread_join(th[i], NULL);
    size_t sz = LL_SIZE(&lst, struct item, link);
    /* Each thread does CONCURRENT_OPS head + CONCURRENT_OPS tail - CONCURRENT_OPS remove_head = CONCURRENT_OPS net per thread */
    ASSERT(sz == (size_t)(CONCURRENT_THREADS * CONCURRENT_OPS));
    struct item *p;
    while ((p = LL_REMOVE_HEAD(&lst, struct item, link)) != NULL)
        free(p);
    return 0;
}

static void *thread_insert_after_worker(void *arg) {
    (void)arg;
    struct item *anchors[4];
    for (int i = 0; i < 4; i++) {
        anchors[i] = malloc(sizeof(struct item));
        anchors[i]->value = 100 + i;
        LL_INSERT_TAIL(conc_lst, anchors[i], link);
    }
    for (int i = 0; i < 50; i++) {
        struct item *n = malloc(sizeof(struct item));
        n->value = 1000 + i;
        LL_INSERT_AFTER(conc_lst, anchors[i % 4], n, link);
    }
    return NULL;
}

static int test_concurrent_insert_after(void) {
    struct list_head lst;
    LL_INIT(&lst);
    conc_lst = &lst;
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, thread_insert_after_worker, (void *)(long)i);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    size_t sz = LL_SIZE(&lst, struct item, link);
    /* 4 threads * 4 anchors + up to 4 * 50 insert_after = 16 + 200 */
    ASSERT(sz >= 16u && sz <= 216u);
    struct item *p;
    while ((p = LL_REMOVE_HEAD(&lst, struct item, link)) != NULL)
        free(p);
    return 0;
}

static void *thread_txn_commit_worker(void *arg) {
    long id = (long)arg;
    for (int k = 0; k < 20; k++) {
        ll_txn_t *txn = LL_TXN_START(conc_lst, struct item, link);
        struct item *a = malloc(sizeof(struct item));
        struct item *b = malloc(sizeof(struct item));
        if (!txn) {
            free(a);
            free(b);
            continue;
        }
        if (a) { a->value = (int)(id * 1000 + k);       LL_TXN_INSERT_HEAD(txn, a, link); }
        if (b) { b->value = (int)(id * 1000 + k + 100); LL_TXN_INSERT_TAIL(txn, b, link); }
        ll_txn_commit(txn);
    }
    return NULL;
}

static void *thread_txn_rollback_worker(void *arg) {
    (void)arg;
    for (int k = 0; k < 30; k++) {
        ll_txn_t *txn = LL_TXN_START(conc_lst, struct item, link);
        if (!txn) continue;
        struct item *a = malloc(sizeof(struct item));
        if (a) { a->value = -1; LL_TXN_INSERT_TAIL(txn, a, link); }
        ll_txn_rollback(txn);
        if (a) free(a);
    }
    return NULL;
}

static int test_concurrent_transactions(void) {
    struct list_head lst;
    LL_INIT(&lst);
    conc_lst = &lst;
    pthread_t th_commit[4], th_rollback[2];
    for (int i = 0; i < 4; i++)
        pthread_create(&th_commit[i], NULL, thread_txn_commit_worker, (void *)(long)i);
    for (int i = 0; i < 2; i++)
        pthread_create(&th_rollback[i], NULL, thread_txn_rollback_worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(th_commit[i], NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(th_rollback[i], NULL);
    size_t sz = LL_SIZE(&lst, struct item, link);
    ASSERT(sz == 4u * 20u * 2u); /* 4 threads * 20 commits * 2 elements per commit */
    struct item *p;
    while ((p = LL_REMOVE_HEAD(&lst, struct item, link)) != NULL)
        free(p);
    return 0;
}

static void *thread_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        size_t n = LL_SIZE(conc_lst, struct item, link);
        (void)n;
        struct item *var;
        LL_FOREACH(var, conc_lst, struct item, link) {
            (void)var;
        }
    }
    return NULL;
}

static void *thread_writer(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 50; i++) {
        struct item *a = malloc(sizeof(struct item));
        struct item *b = malloc(sizeof(struct item));
        if (a) { a->value = (int)(id + i); LL_INSERT_HEAD(conc_lst, a, link); }
        if (b) { b->value = (int)(id + i + 100); LL_INSERT_TAIL(conc_lst, b, link); }
        struct item *p = LL_REMOVE_HEAD(conc_lst, struct item, link);
        if (p) free(p);
    }
    return NULL;
}

static int test_concurrent_readers_writers(void) {
    struct list_head lst;
    LL_INIT(&lst);
    conc_lst = &lst;
    pthread_t readers[4], writers[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&readers[i], NULL, thread_reader, NULL);
    for (int i = 0; i < 4; i++)
        pthread_create(&writers[i], NULL, thread_writer, (void *)(long)i);
    for (int i = 0; i < 4; i++) {
        pthread_join(readers[i], NULL);
        pthread_join(writers[i], NULL);
    }
    struct item *p;
    while ((p = LL_REMOVE_HEAD(&lst, struct item, link)) != NULL)
        free(p);
    return 0;
}

static void run_unit_tests(void) {
    printf("Unit tests:\n");
    RUN_TEST("init empty", test_init_empty);
    RUN_TEST("insert head size", test_insert_head_size);
    RUN_TEST("insert tail contains", test_insert_tail_contains);
    RUN_TEST("insert after order", test_insert_after_order);
    RUN_TEST("remove by elm", test_remove_by_elm);
    RUN_TEST("foreach order", test_foreach_order);
    RUN_TEST("remove head empty", test_remove_head_empty);
    RUN_TEST("insert after nonexistent", test_insert_after_nonexistent);
    RUN_TEST("txn insert after commit", test_txn_insert_after_commit);
    RUN_TEST("txn rollback discards", test_txn_rollback_discards);
    RUN_TEST("txn multiple insert_after same anchor", test_txn_multiple_insert_after_same_anchor);
    RUN_TEST("txn remove inserted_after", test_txn_remove_inserted_after);
}

static void run_concurrent_tests(void) {
    printf("Concurrent tests:\n");
    RUN_TEST("concurrent mixed head/tail", test_concurrent_mixed_head_tail);
    RUN_TEST("concurrent insert_after", test_concurrent_insert_after);
    RUN_TEST("concurrent transactions", test_concurrent_transactions);
    RUN_TEST("concurrent readers writers", test_concurrent_readers_writers);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    tests_run = 0;
    tests_failed = 0;
    run_unit_tests();
    run_concurrent_tests();
    printf("\nTotal: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
