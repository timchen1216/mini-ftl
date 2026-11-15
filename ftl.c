/*
 * ftl.c - Mini FTL Simulator 實作
 * 目前全部是空殼(stub),只用來確認環境能編譯、能執行
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ftl.h"

static uint16_t alloc_free_block(ftl_t* ftl);
static void     nand_program(ftl_t* ftl, uint32_t ppa);
static void     invalidate_ppa(ftl_t* ftl, uint32_t ppa);
static uint32_t get_next_ppa(ftl_t* ftl);
static void     nand_erase(ftl_t* ftl, uint16_t blk);
static uint16_t gc_pick_victim(const ftl_t* ftl);

static void nand_program(ftl_t* ftl, uint32_t ppa)
{
    uint32_t blk = PPA_BLOCK(ppa);   
    uint32_t page = PPA_PAGE(ppa);
    block_t* b = &ftl->blocks[blk];

    assert(page == b->write_ptr);
    b->valid_bitmap |= (1ULL << page);
    b->write_ptr++;
    b->valid_count++;
    ftl->stats.nand_writes++;
}

static void invalidate_ppa(ftl_t* ftl, uint32_t ppa) {
    uint32_t blk = PPA_BLOCK(ppa);
    uint32_t page = PPA_PAGE(ppa);
    block_t* b = &ftl->blocks[blk];

    assert(b->valid_bitmap & (1ULL << page));   // 這個 page 必須原本是 valid
    b->valid_bitmap &= ~(1ULL << page);
    b->valid_count--;
    ftl->p2l[ppa] = INVALID_LPN;
}

static uint32_t get_next_ppa(ftl_t* ftl) {
    block_t* b = &ftl->blocks[ftl->active_block];

    if (b->write_ptr == PAGES_PER_BLOCK) {
        b->state = BLK_FULL;
        ftl->active_block = alloc_free_block(ftl);
        assert(ftl->active_block != INVALID_BLOCK);
        b = &ftl->blocks[ftl->active_block];
    }
    return PPA(ftl->active_block, b->write_ptr);
}

static uint16_t alloc_free_block(ftl_t* ftl) {
    uint16_t best = INVALID_BLOCK;
    uint32_t min_ec = UINT32_MAX;

    for (uint16_t i = 0; i < NUM_BLOCKS; i++) {
        if (ftl->blocks[i].state != BLK_FREE) continue;
        if (ftl->blocks[i].erase_count < min_ec) {
            best = i;
            min_ec = ftl->blocks[i].erase_count;
        }
    }

    if (best == INVALID_BLOCK) return INVALID_BLOCK;

    ftl->blocks[best].state = BLK_ACTIVE;
    ftl->free_block_count--;
    return best;
}

static void nand_erase(ftl_t* ftl, uint16_t blk) {
    block_t* b = &ftl->blocks[blk];
    assert(b->valid_count == 0);
    b->valid_bitmap = 0;
    b->write_ptr = 0;
    b->erase_count++;
    b->state = BLK_FREE;
    ftl->free_block_count++;
    ftl->stats.erase_total++;
}

static uint16_t gc_pick_victim(const ftl_t* ftl) {
    uint16_t best = INVALID_BLOCK;
    uint32_t min_va = UINT32_MAX;

    for (uint16_t i = 0; i < NUM_BLOCKS; i++) {
        if (ftl->blocks[i].state != BLK_FULL) continue;
        if (ftl->blocks[i].valid_count < min_va) {
            best = i;
            min_va = ftl->blocks[i].valid_count;
        }
    }
    return best;
}
static void gc_run(ftl_t* ftl) {
    ftl->in_gc = true;
    uint16_t victim = gc_pick_victim(ftl);
    assert(victim != INVALID_BLOCK);
    block_t* b = &ftl->blocks[victim];

    for (uint32_t page = 0; page < PAGES_PER_BLOCK; page++) {
        if (!(b->valid_bitmap & (1ULL << page))) continue;   // 不是 valid 就跳過
        uint32_t old_ppa = PPA(victim, page);
        uint32_t lpn = ftl->p2l[old_ppa];
        uint32_t new_ppa = get_next_ppa(ftl);
        nand_program(ftl, new_ppa);
        invalidate_ppa(ftl, old_ppa);
        ftl->l2p[lpn] = new_ppa;  ftl->p2l[new_ppa] = lpn;
        ftl->stats.gc_moved_pages++;
    }
    nand_erase(ftl, victim);
    ftl->stats.gc_count++;
    ftl->in_gc = false;
}

void ftl_init(ftl_t* ftl)
{
    memset(ftl, 0, sizeof(*ftl));   // blocks、stats、in_gc 全部歸零
    memset(ftl->l2p, 0xFF, sizeof(ftl->l2p));
    memset(ftl->p2l, 0xFF, sizeof(ftl->p2l));
    ftl->in_gc = false;
    ftl->free_block_count = NUM_BLOCKS;              // 一開始全部都是 free
    ftl->active_block = alloc_free_block(ftl);       // 由它挑選並負責扣 1
}

ftl_status_t ftl_write(ftl_t* ftl, uint32_t lpn)
{
    if (lpn >= LOGICAL_PAGES) return FTL_ERR_LPN_OUT_OF_RANGE;
    ftl->stats.host_writes++;
    while (!ftl->in_gc && ftl->free_block_count < GC_THRESHOLD) {
        gc_run(ftl);
    }
    uint32_t old_ppa = ftl->l2p[lpn];     
    uint32_t new_ppa = get_next_ppa(ftl);
    nand_program(ftl, new_ppa);
    if (old_ppa != INVALID_PPA) invalidate_ppa(ftl, old_ppa);
    ftl->l2p[lpn] = new_ppa;  ftl->p2l[new_ppa] = lpn;
    return FTL_OK;
}

ftl_status_t ftl_read(const ftl_t* ftl, uint32_t lpn, uint32_t* ppa_out)
{
    if (lpn >= LOGICAL_PAGES) return FTL_ERR_LPN_OUT_OF_RANGE;
    if (ftl->l2p[lpn] == INVALID_PPA) return FTL_ERR_UNMAPPED;
    *ppa_out = ftl->l2p[lpn];
    return FTL_OK;
}

ftl_status_t ftl_trim(ftl_t* ftl, uint32_t lpn)
{
    (void)ftl; (void)lpn;   /* TODO(選做) */
    return FTL_OK;
}

double ftl_waf(const ftl_t* ftl)
{
    (void)ftl;   /* TODO */
    return 0.0;
}

void ftl_print_stats(const ftl_t* ftl)
{
    (void)ftl;   /* TODO */
    printf("(stats not implemented yet)\n");
}

bool ftl_check_consistency(const ftl_t* ftl)
{
    (void)ftl;   /* TODO */
    return true;
}