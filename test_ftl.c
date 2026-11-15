/*
 * test_ftl.c - Mini FTL tests and WAF experiment
 * Build: gcc -std=c11 -Wall -Wextra -O2 ftl.c test_ftl.c -o test_ftl
 * Change OP: add -DOP_PERCENT=28
 */
#include <stdio.h>
#include <stdlib.h>
#include "ftl.h"

 /* Unlike assert, expr is always evaluated, even in Release builds (NDEBUG). */
#define CHECK(expr) do {                                               \
    if (!(expr)) {                                                     \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n",                  \
                #expr, __FILE__, __LINE__);                            \
        exit(1);                                                       \
    }                                                                  \
} while (0)

static ftl_t g_ftl;   /* too large for the stack */

/* xorshift PRNG, deterministic */
static uint32_t rng_state = 12345u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* 1. Basic read/write */
static void test_basic_rw(void) {
    uint32_t ppa;
    ftl_init(&g_ftl);
    CHECK(ftl_read(&g_ftl, 0, &ppa) == FTL_ERR_UNMAPPED);
    CHECK(ftl_write(&g_ftl, 0) == FTL_OK);
    CHECK(ftl_read(&g_ftl, 0, &ppa) == FTL_OK);
    CHECK(ppa != INVALID_PPA);
    CHECK(ftl_write(&g_ftl, LOGICAL_PAGES) == FTL_ERR_LPN_OUT_OF_RANGE);
    CHECK(ftl_check_consistency(&g_ftl));
    printf("[PASS] basic read/write\n");
}

/* 2. Overwriting an LPN must go to a new page */
static void test_out_of_place(void) {
    uint32_t ppa1, ppa2;
    ftl_init(&g_ftl);
    ftl_write(&g_ftl, 42);
    ftl_read(&g_ftl, 42, &ppa1);
    ftl_write(&g_ftl, 42);
    ftl_read(&g_ftl, 42, &ppa2);
    CHECK(ppa1 != ppa2);
    CHECK(g_ftl.p2l[ppa1] == INVALID_LPN);
    CHECK(g_ftl.p2l[ppa2] == 42);
    CHECK(ftl_check_consistency(&g_ftl));
    printf("[PASS] out-of-place update\n");
}

/* 3. Many times the capacity in random writes; mapping must survive GC */
static void test_gc_correctness(void) {
    ftl_init(&g_ftl);
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) ftl_write(&g_ftl, i);   /* fill the drive */
    for (uint32_t i = 0; i < LOGICAL_PAGES * 10u; i++) {
        CHECK(ftl_write(&g_ftl, rng() % LOGICAL_PAGES) == FTL_OK);
    }
    CHECK(g_ftl.stats.gc_count > 0);
    CHECK(ftl_check_consistency(&g_ftl));
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) {
        uint32_t ppa;
        CHECK(ftl_read(&g_ftl, i, &ppa) == FTL_OK);
        CHECK(g_ftl.p2l[ppa] == i);
    }
    printf("[PASS] GC keeps mapping correct (gc_count=%llu)\n",
        (unsigned long long)g_ftl.stats.gc_count);
}

/* 4. WAF experiment: sequential vs random, printed as CSV */
static void run_workload(const char* name, int random) {
    ftl_init(&g_ftl);
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) ftl_write(&g_ftl, i);
    g_ftl.stats.host_writes = 0;          /* measure steady state only */
    g_ftl.stats.nand_writes = 0;
    for (uint32_t i = 0; i < LOGICAL_PAGES * 20u; i++) {
        uint32_t lpn = random ? (rng() % LOGICAL_PAGES) : (i % LOGICAL_PAGES);
        ftl_write(&g_ftl, lpn);
    }
    printf("%s,%u,%.3f\n", name, OP_PERCENT, ftl_waf(&g_ftl));
}

int main(void) {
    test_basic_rw();
    test_out_of_place();
    test_gc_correctness();

    printf("\nworkload,op_percent,waf\n");
    run_workload("sequential", 0);
    run_workload("random", 1);

    ftl_print_stats(&g_ftl);
    return 0;
}