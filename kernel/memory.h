#ifndef _KERNEL_MOMERY_H
#define _KERNEL_MOMERY_H
#include "stdint.h"
#include "bitmap.h"
#include "list.h"

/* 内存池标记，用于判断用哪个内存池 */
enum pool_flags{
    PF_KERNEL = 1,
    PF_USER = 2
};

#define PG_P_1      1   //页表项或页目录存在属性位
#define PG_P_0      0   //页表项或页目录存在属性位
#define PG_RW_R     0   //R/W属性位值，读/执行
#define PG_RW_W     2   //R/W属性位值，读/写/执行
#define PG_US_S     0   //U/S属性位值，系统级
#define PG_US_U     4   //U/S属性位值，用户级

struct virtual_addr
{
    /* data */
    struct bitmap vaddr_bitmap; //虚拟地址用到的位图结构
    uint32_t vaddr_start;       //虚拟地址起始位置
};

/* 内存块 */
struct mem_block {
    struct list_elem free_elem;
};

/* 内存块描述符 */
struct mem_block_desc {
    uint32_t block_size;    //内存块大小（16，32，64，128，512，1024字节大小）
    uint32_t block_per_arena;   //本arena中可容纳此mem_block的数量
    struct list free_list;  //目前可用的mem_blovk链表
};

#define DESC_CNT 7    // 内存块描述符个数

extern struct pool kernel_pool, user_pool;

uint32_t* pte_ptr(uint32_t vaddr);
uint32_t* pde_ptr(uint32_t vaddr);
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt);
void* get_kernel_pages(uint32_t pg_cnt);
void* get_user_pages(uint32_t pg_cnt);
void* get_a_page(enum pool_flags pf, uint32_t vaddr);
void* get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr);
uint32_t addr_v2p(uint32_t vaddr);
void pfree(uint32_t pg_phy_addr);
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt);
void sys_free(void* ptr);
void block_desc_init(struct mem_block_desc* desc_array);
void* sys_malloc(uint32_t size);
void mem_init(void);
#endif
