/**
 * Demo: BSD-style concurrent list macros with a custom struct.
 */
#include "concurrent_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* Define an element type and embed the list entry. */
struct item {
    int value;
    CONCURRENT_LIST_ENTRY(item, link);
};

/* Declare a list head type for this element type. */
CONCURRENT_LIST_HEAD(list_head, item);

static struct list_head lst;
#define NUM_THREADS 4
#define OPS_PER_THREAD 100

static struct list_head *g_lst;

static void *thread_insert_remove(void *arg)
{
    (void)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        struct item *a = malloc(sizeof(*a));
        struct item *b = malloc(sizeof(*b));
        if (a) { a->value = i; CONCURRENT_LIST_INSERT_HEAD(g_lst, a, link); }
        if (b) { b->value = i + 1000; CONCURRENT_LIST_INSERT_TAIL(g_lst, b, link); }
    }
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        struct item *p = CONCURRENT_LIST_REMOVE_HEAD(g_lst, struct item, link);
        if (p)
            free(p);
    }
    return NULL;
}

int main(void)
{
    struct list_head *lst_p = &lst;
    CONCURRENT_LIST_INIT(lst_p);

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
    CONCURRENT_LIST_INSERT_HEAD(lst_p, a, link);
    CONCURRENT_LIST_INSERT_HEAD(lst_p, b, link);
    CONCURRENT_LIST_INSERT_TAIL(lst_p, c, link);

    printf("size after inserts: %zu\n", CONCURRENT_LIST_SIZE(lst_p, struct item, link));
    printf("contains b: %d\n", CONCURRENT_LIST_CONTAINS(lst_p, b, link));

    struct item *p;
    while ((p = CONCURRENT_LIST_REMOVE_HEAD(lst_p, struct item, link)) != NULL) {
        printf("popped %d\n", p->value);
        free(p);
    }
    printf("is_empty: %d\n", CONCURRENT_LIST_IS_EMPTY(lst_p));

    /* Multi-threaded stress */
    g_lst = lst_p;
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_insert_remove, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("size after concurrent ops: %zu\n", CONCURRENT_LIST_SIZE(lst_p, struct item, link));

    /* Drain and free remaining */
    while ((p = CONCURRENT_LIST_REMOVE_HEAD(lst_p, struct item, link)) != NULL)
        free(p);

    return 0;
}
