/*
 * test_ftl.c - Mini FTL 測試
 * 編譯:gcc -std=c11 -Wall -Wextra -O2 ftl.c test_ftl.c -o test_ftl
 * 換 OP:gcc ... -DOP_PERCENT=28 ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "ftl.h"

static ftl_t g_ftl;   /* 很大，放 static 區不要放 stack */

/* 簡單的 xorshift 亂數，結果可重現 */
static uint32_t rng_state = 12345u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* 1. 基本讀寫 */
static void test_basic_rw(void) {
    uint32_t ppa;
    ftl_init(&g_ftl);
    assert(ftl_read(&g_ftl, 0, &ppa) == FTL_ERR_UNMAPPED);
    assert(ftl_write(&g_ftl, 0) == FTL_OK);
    assert(ftl_read(&g_ftl, 0, &ppa) == FTL_OK);
    assert(ppa != INVALID_PPA);
    assert(ftl_write(&g_ftl, LOGICAL_PAGES) == FTL_ERR_LPN_OUT_OF_RANGE);
    assert(ftl_check_consistency(&g_ftl));
    printf("[PASS] basic read/write\n");
}

/* 2. 覆寫同一個 LPN 必須 out-of-place */
static void test_out_of_place(void) {
    uint32_t ppa1, ppa2;
    ftl_init(&g_ftl);
    ftl_write(&g_ftl, 42);
    ftl_read(&g_ftl, 42, &ppa1);
    ftl_write(&g_ftl, 42);
    ftl_read(&g_ftl, 42, &ppa2);
    assert(ppa1 != ppa2);
    assert(g_ftl.p2l[ppa1] == INVALID_LPN);
    assert(g_ftl.p2l[ppa2] == 42);
    assert(ftl_check_consistency(&g_ftl));
    printf("[PASS] out-of-place update\n");
}

/* 3. 寫入量遠超容量，GC 必須讓 mapping 保持正確 */
static void test_gc_correctness(void) {
    ftl_init(&g_ftl);
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) ftl_write(&g_ftl, i);   /* 先填滿 */
    for (uint32_t i = 0; i < LOGICAL_PAGES * 10u; i++) {
        assert(ftl_write(&g_ftl, rng() % LOGICAL_PAGES) == FTL_OK);
    }
    assert(g_ftl.stats.gc_count > 0);
    assert(ftl_check_consistency(&g_ftl));
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) {
        uint32_t ppa;
        assert(ftl_read(&g_ftl, i, &ppa) == FTL_OK);
        assert(g_ftl.p2l[ppa] == i);
    }
    printf("[PASS] GC keeps mapping correct (gc_count=%llu)\n",
           (unsigned long long)g_ftl.stats.gc_count);
}

/* 4. 實驗:sequential vs random 的 WAF，輸出成 CSV 方便畫圖 */
static void run_workload(const char *name, int random) {
    ftl_init(&g_ftl);
    for (uint32_t i = 0; i < LOGICAL_PAGES; i++) ftl_write(&g_ftl, i);
    g_ftl.stats.host_writes = 0;          /* 只看穩態，扣掉預填 */
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
