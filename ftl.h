/*
 * ftl.h - Mini FTL Simulator
 *
 * A page-level Flash Translation Layer with greedy garbage collection
 * and dynamic wear leveling. Only metadata is simulated (no user data).
 *
 * Page state within a block (bitmap + write pointer):
 *   index >= write_ptr               -> FREE
 *   index <  write_ptr, bit == 1     -> VALID
 *   index <  write_ptr, bit == 0     -> INVALID
 * NAND pages must be programmed sequentially, so one write pointer
 * is enough to describe the free region.
 */
#ifndef FTL_H
#define FTL_H

#include <stdint.h>
#include <stdbool.h>

 /* ---------- Bit helpers (GCC/Clang and MSVC) ---------- */
#if defined(_MSC_VER)
#include <intrin.h>
static __inline int bit_ctz64(uint64_t x) {        /* x must be non-zero */
    unsigned long idx; _BitScanForward64(&idx, x); return (int)idx;
}
static __inline int bit_popcount64(uint64_t x) { return (int)__popcnt64(x); }
#else
#define bit_ctz64(x)      __builtin_ctzll(x)
#define bit_popcount64(x) __builtin_popcountll(x)
#endif

/* ---------- Geometry ---------- */
#define NUM_BLOCKS        128u
#define PAGES_PER_BLOCK   64u      /* fits one uint64_t bitmap */
#define TOTAL_PAGES       (NUM_BLOCKS * PAGES_PER_BLOCK)

/* Over-provisioning in percent; override with -DOP_PERCENT=28 */
#ifndef OP_PERCENT
#define OP_PERCENT        7u
#endif
#define LOGICAL_PAGES     ((TOTAL_PAGES * 100u) / (100u + OP_PERCENT))

/* Run GC when free blocks drop below this; one block stays reserved for GC */
#define GC_THRESHOLD      2u

/* ---------- Address encoding ---------- */
#define INVALID_PPA       0xFFFFFFFFu
#define INVALID_LPN       0xFFFFFFFFu
#define INVALID_BLOCK     0xFFFFu

#define PPA(blk, pg)      ((uint32_t)(blk) * PAGES_PER_BLOCK + (uint32_t)(pg))
#define PPA_BLOCK(ppa)    ((ppa) / PAGES_PER_BLOCK)
#define PPA_PAGE(ppa)     ((ppa) % PAGES_PER_BLOCK)

typedef enum {
    FTL_OK = 0,
    FTL_ERR_LPN_OUT_OF_RANGE,
    FTL_ERR_UNMAPPED,
    FTL_ERR_NO_SPACE,
} ftl_status_t;

typedef enum {
    BLK_FREE = 0,   /* erased, ready to allocate */
    BLK_ACTIVE,     /* current write point */
    BLK_FULL,       /* fully written, GC candidate */
} blk_state_t;

typedef struct {
    uint64_t valid_bitmap;
    uint16_t write_ptr;
    uint16_t valid_count;    /* cached popcount(valid_bitmap) for fast victim selection */
    uint32_t erase_count;
    uint8_t  state;          /* blk_state_t */
} block_t;

typedef struct {
    uint64_t host_writes;
    uint64_t nand_writes;    /* includes GC relocation writes */
    uint64_t gc_count;
    uint64_t gc_moved_pages;
    uint64_t erase_total;
} ftl_stats_t;

typedef struct {
    block_t     blocks[NUM_BLOCKS];
    uint32_t    l2p[LOGICAL_PAGES];   /* logical -> physical */
    uint32_t    p2l[TOTAL_PAGES];     /* physical -> logical, models the LPN stored in page OOB */
    uint16_t    active_block;
    uint16_t    free_block_count;
    bool        in_gc;                /* guards against recursive GC */
    ftl_stats_t stats;
} ftl_t;

/* ---------- Host interface ---------- */
void         ftl_init(ftl_t* ftl);
ftl_status_t ftl_write(ftl_t* ftl, uint32_t lpn);
ftl_status_t ftl_read(const ftl_t* ftl, uint32_t lpn, uint32_t* ppa_out);
ftl_status_t ftl_trim(ftl_t* ftl, uint32_t lpn);   /* not implemented yet */
double       ftl_waf(const ftl_t* ftl);            /* nand_writes / host_writes */
void         ftl_print_stats(const ftl_t* ftl);

/* Verifies internal invariants (valid counts, free count, erase total,
 * l2p/p2l agreement, no leaked valid pages). Returns true if all hold. */
bool         ftl_check_consistency(const ftl_t* ftl);

#endif /* FTL_H */