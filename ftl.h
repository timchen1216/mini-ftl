/*
 * ftl.h - Mini FTL Simulator
 *
 * 設計原則(貼近韌體風格):
 *   - 全部用固定大小的靜態陣列，不用 malloc
 *   - page 狀態用 bitmap + write pointer 表示，練位元操作
 *   - 只模擬 metadata，不存真正的資料內容
 *
 * Page 狀態怎麼判斷(每個 block 內):
 *   page index >= write_ptr                → FREE    (還沒寫過)
 *   page index <  write_ptr 且 bit = 1     → VALID
 *   page index <  write_ptr 且 bit = 0     → INVALID (被新資料取代或被 TRIM)
 *   NAND 只能依序寫入 block 內的 page，所以一個 write_ptr 就足以表示 free 區段
 */
#ifndef FTL_H
#define FTL_H

#include <stdint.h>
#include <stdbool.h>

/* ---------- 位元操作:GCC/Clang 與 MSVC 相容層 ---------- */
#if defined(_MSC_VER)
  #include <intrin.h>
  static __inline int bit_ctz64(uint64_t x) {        /* x 不可為 0 */
      unsigned long idx; _BitScanForward64(&idx, x); return (int)idx;
  }
  static __inline int bit_popcount64(uint64_t x) { return (int)__popcnt64(x); }
#else
  #define bit_ctz64(x)      __builtin_ctzll(x)
  #define bit_popcount64(x) __builtin_popcountll(x)
#endif

/* ---------- 幾何參數 ---------- */
#define NUM_BLOCKS        128u
#define PAGES_PER_BLOCK   64u      /* = 64，剛好用一個 uint64_t 當 bitmap */
#define TOTAL_PAGES       (NUM_BLOCKS * PAGES_PER_BLOCK)

/* OP(over-provisioning)以百分比表示，可在編譯時用 -DOP_PERCENT=28 覆蓋 */
#ifndef OP_PERCENT
#define OP_PERCENT        7u
#endif
/* 使用者看得到的邏輯 page 數:實體 / (1 + OP) */
#define LOGICAL_PAGES     ((TOTAL_PAGES * 100u) / (100u + OP_PERCENT))

/* free block 少於這個數量就觸發 GC */
#define GC_THRESHOLD      2u

/* ---------- 實體位址(PPA)編碼 ---------- */
#define INVALID_PPA       0xFFFFFFFFu
#define INVALID_LPN       0xFFFFFFFFu
#define INVALID_BLOCK     0xFFFFu

#define PPA(blk, pg)      ((uint32_t)(blk) * PAGES_PER_BLOCK + (uint32_t)(pg))
#define PPA_BLOCK(ppa)    ((ppa) / PAGES_PER_BLOCK)
#define PPA_PAGE(ppa)     ((ppa) % PAGES_PER_BLOCK)

/* ---------- 回傳碼 ---------- */
typedef enum {
    FTL_OK = 0,
    FTL_ERR_LPN_OUT_OF_RANGE,
    FTL_ERR_UNMAPPED,       /* 讀到沒寫過(或被 TRIM)的 LPN */
    FTL_ERR_NO_SPACE,       /* GC 後仍無 free block，理論上不應發生 */
} ftl_status_t;

/* ---------- Block 狀態 ---------- */
typedef enum {
    BLK_FREE = 0,   /* 已抹除，可配置 */
    BLK_ACTIVE,     /* 目前正在寫入的 block */
    BLK_FULL,       /* 已寫滿，可能成為 GC victim */
} blk_state_t;

typedef struct {
    uint64_t valid_bitmap;   /* bit i = 1 表示 page i 是 VALID */
    uint16_t write_ptr;      /* 下一個可寫的 page index */
    uint16_t valid_count;    /* = popcount(valid_bitmap)，存起來加速 victim 挑選 */
    uint32_t erase_count;
    uint8_t  state;          /* blk_state_t */
} block_t;

typedef struct {
    uint64_t host_writes;    /* 主機下的寫入 page 數 */
    uint64_t nand_writes;    /* NAND 實際 program 的 page 數(含 GC 搬移) */
    uint64_t gc_count;       /* GC 觸發次數 */
    uint64_t gc_moved_pages; /* GC 搬移的 valid page 數 */
    uint64_t erase_total;
} ftl_stats_t;

typedef struct {
    block_t     blocks[NUM_BLOCKS];
    uint32_t    l2p[LOGICAL_PAGES];   /* 邏輯 → 實體 */
    uint32_t    p2l[TOTAL_PAGES];     /* 實體 → 邏輯，模擬寫在 page OOB 裡的 LPN，GC 搬移時要用 */
    uint16_t    active_block;         /* 目前的寫入點 */
    uint16_t    free_block_count;
    bool        in_gc;                /* 防止 GC 內部的寫入又遞迴觸發 GC */
    ftl_stats_t stats;
} ftl_t;

/* ================= 對外介面(主機視角) ================= */

/* 全部 block 設成 FREE、erase_count 歸零、l2p/p2l 填 INVALID、配置第一個 active block */
void         ftl_init(ftl_t *ftl);

/* 寫入一個邏輯 page:out-of-place 寫到新位置，舊位置標 INVALID，必要時觸發 GC */
ftl_status_t ftl_write(ftl_t *ftl, uint32_t lpn);

/* 查 L2P，把實體位址放進 *ppa_out */
ftl_status_t ftl_read(const ftl_t *ftl, uint32_t lpn, uint32_t *ppa_out);

/* (選做)TRIM:把 lpn 對應的實體 page 標 INVALID，l2p 設成 INVALID_PPA */
ftl_status_t ftl_trim(ftl_t *ftl, uint32_t lpn);

/* WAF = nand_writes / host_writes */
double       ftl_waf(const ftl_t *ftl);

/* 印出統計與 erase count 的 min / max / 平均 */
void         ftl_print_stats(const ftl_t *ftl);

/* 檢查內部一致性，測試用:
 *   - 每個 block 的 valid_count == popcount(valid_bitmap)
 *   - l2p[lpn] = ppa  ⇔  p2l[ppa] = lpn 且該 page 是 VALID
 *   - free_block_count 等於實際 BLK_FREE 的數量
 * 全部成立回傳 true */
bool         ftl_check_consistency(const ftl_t *ftl);

/* ================= 內部函式(放在 ftl.c，宣告成 static) =================
 *
 * static void     nand_program(ftl_t *ftl, uint32_t ppa);
 *     模擬 NAND 寫入:確認該 page 是 FREE(不能覆寫!),nand_writes++
 *
 * static void     nand_erase(ftl_t *ftl, uint16_t blk);
 *     整個 block 清空:bitmap=0、write_ptr=0、valid_count=0、erase_count++、state=FREE
 *
 * static uint16_t alloc_free_block(ftl_t *ftl);
 *     Dynamic wear leveling:在所有 FREE block 中挑 erase_count 最小的
 *
 * static uint32_t get_next_ppa(ftl_t *ftl);
 *     從 active block 取下一個 page;寫滿就把它設成 FULL 並配置新 block
 *
 * static void     invalidate_ppa(ftl_t *ftl, uint32_t ppa);
 *     清掉 bitmap 對應的 bit、valid_count--、p2l 設成 INVALID_LPN
 *
 * static uint16_t gc_pick_victim(const ftl_t *ftl);
 *     Greedy:在 FULL block 中挑 valid_count 最少的
 *
 * static void     gc_run(ftl_t *ftl);
 *     1. 挑 victim
 *     2. 掃 victim 的 valid_bitmap，對每個 VALID page:
 *        用 p2l 找回 lpn → 寫到新位置 → 更新 l2p / p2l → gc_moved_pages++
 *     3. nand_erase(victim)
 *
 * 提示:掃 bitmap 可以練 `bitmap & (bitmap - 1)` 清最低位、
 *       bit_ctz64() 找最低位的 1、bit_popcount64() 算數量(上方相容層)
 */

#endif /* FTL_H */
