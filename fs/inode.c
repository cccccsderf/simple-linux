#include "inode.h"
#include "../device/ide.h"
#include "debug.h"
#include "../thread/thread.h"
#include "memory.h"
#include "string.h"
#include "interrupt.h"
#include "file.h"

// 用来存储inode位置
struct inode_position
{
    bool two_sec;       // inode是否跨扇区
    uint32_t sec_lba;   // inode所在扇区号
    uint32_t off_size;  // inode在扇区内的偏移量
};

// 获取inode所在的扇区号和扇区内的偏移量
static void inode_locate(struct partition* part, uint32_t inode_no, struct inode_position* inode_pos){
     // part：分区  inode_no：inode编号          inode_pos：inode位置
     // inode_table在硬盘上是连续的
     ASSERT(inode_no < 4096);
     // inode数组起始扇区位置
     uint32_t inode_table_lba = part->sb->inode_table_lba;
     // inode的大小
     uint32_t inode_size = sizeof(struct inode);
     // 第inode_no号I结点相对于inode_table_lba的字节偏移量
     uint32_t off_size = inode_no * inode_size;
     // 第inode_no号I结点相对于inode_table_lba的扇区偏移量
     uint32_t off_sec = off_size / 512;
     // 待查找的inode所在扇区中的起始地址
     uint32_t off_size_in_sec = off_size % 512;

     // 判断I结点是否跨过两个扇区
     uint32_t left_in_sec = 512 - off_size_in_sec;
     // 若扇区剩下的空间不足以容纳一个inode 则跨了两个扇区
     if (left_in_sec < inode_size)
     {
        inode_pos->two_sec = true;
     }else{
        inode_pos->two_sec = false;
     }
     inode_pos->sec_lba = inode_table_lba + off_sec;
     inode_pos->off_size = off_size_in_sec;
}
/* 将inode写入到分区part */
void inode_sync(struct partition* part, struct inode* inode, void* io_buf){
    // 获取inode编号
    uint8_t inode_no = inode->i_no;
    struct inode_position inode_pos;
    inode_locate(part,inode_no,&inode_pos);
    // 验证inode所在扇区号是否小于等于分区起始lba地址和扇区数量的和
    ASSERT(inode_pos.sec_lba <= (part->start_lba + part->sec_cnt));
    // 硬盘中inode中的成员 i_open_cnts 和 inode_tag是不需要的
    struct inode pure_inode;
    memcpy(&pure_inode, inode, sizeof(struct inode));
    // 以下三个成员只存在与内存中，现在将inode同步到硬盘，清楚掉这三项即可
    pure_inode.i_open_cnts = 0;
    pure_inode.write_deny = false;
    pure_inode.inode_tag.prev = pure_inode.inode_tag.next = NULL;
    // io_buf 用于硬盘inode缓冲区
    char* inode_buf = (char*)io_buf;
    // 跨扇区
    if (inode_pos.two_sec)
    {
        // 读写硬盘以扇区为单位，若写入的数据小于一个扇区，则将原来扇区的数据读取出来
        // 再和新数据拼成一扇区后再写入
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
        // 开始将待写入的inode拼入到这两个扇区中的相应位置
        memcpy((inode_buf + inode_pos.off_size), &pure_inode, sizeof(struct inode));
        // 拼接好的数据再写入磁盘
        ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
    }else{
        // 只有一个扇区
        ide_read(part->my_disk,inode_pos.sec_lba,inode_buf,1);
        memcpy((inode_buf + inode_pos.off_size), &pure_inode, sizeof(struct inode));
        ide_write(part->my_disk, inode_pos.sec_lba,inode_buf,1);
    }
}
/* 根据I结点号返回相应的i结点 */
struct inode* inode_open(struct partition* part, uint32_t inode_no){
    // 先在已打开inode链表中找inode，此链表是为提速创建的缓冲区
    struct list_elem* elem = part->open_inodes.head.next;
    struct inode* inode_found;
    while (elem != &part->open_inodes.tail)
    {
        inode_found = elem2entry(struct inode, inode_tag, elem);
        if (inode_found->i_no == inode_no)
        {
            inode_found->i_open_cnts++;
            return inode_found;
        }
        elem = elem->next;
    }
    
    /* 由于open_inode链表中找不到，就在硬盘上读入此inode并加入链表 */
    struct inode_position ionde_pos;
    inode_locate(part, inode_no, &ionde_pos);
    /* 为了使sys_malloc创建的新的inode被所有任务共享，需要inode置于内核空间 */
    struct task_struct* cur = running_thread();
    uint32_t* cur_pagedir_bak = cur->pgdir;
    cur->pgdir = NULL;
    // 以上三行代码完成申请的内存将位于内核区
    inode_found = (struct inode*)sys_malloc(sizeof(struct inode));
    // 恢复pgdir
    cur->pgdir = cur_pagedir_bak;

    char* inode_buf;
    if (ionde_pos.two_sec)
    {
        inode_buf = (char*)sys_malloc(1024);
        ide_read(part->my_disk,ionde_pos.sec_lba,inode_buf,2);
    }
    else{ // 没有跨扇区
        inode_buf = (char*)sys_malloc(512);
        ide_read(part->my_disk,ionde_pos.sec_lba,inode_buf,1);
    }
    memcpy(inode_found, inode_buf + ionde_pos.off_size, sizeof(struct inode));
    // 因为一会很可能用到此inode，故将其插入到队首便于提前检索
    list_push(&part->open_inodes, &inode_found->inode_tag);
    inode_found->i_open_cnts = 1;
    sys_free(inode_buf);
    return inode_found;    
}

/* 关闭inode或减少inode的打开数 */
void inode_close(struct inode* inode){
    // 若没有进程再打开此文件（inode）,将此inode去掉并释放空间
    enum intr_status old_status = intr_disable();
    if (--inode->i_open_cnts == 0)
    {
        list_remove(&inode->inode_tag);
        // 是在内核空间申请的内存，所以释放需要释放掉内核空间
        struct task_struct* cur = running_thread();
        uint32_t* cur_pagedir_bak = cur->pgdir;
        cur->pgdir = NULL;
        sys_free(inode);
        cur->pgdir = cur_pagedir_bak;
    }
    intr_set_status(old_status);    
}

/* 将硬盘分区part上的inode清空 */
void inode_delete(struct partition* part, uint32_t inode_no, void* io_buf) {
   ASSERT(inode_no < 4096);
   struct inode_position inode_pos;
   inode_locate(part, inode_no, &inode_pos);     // inode位置信息会存入inode_pos
   ASSERT(inode_pos.sec_lba <= (part->start_lba + part->sec_cnt));
   
   char* inode_buf = (char*)io_buf;
   if (inode_pos.two_sec) {   // inode跨扇区,读入2个扇区
      /* 将原硬盘上的内容先读出来 */
      ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
      /* 将inode_buf清0 */
      memset((inode_buf + inode_pos.off_size), 0, sizeof(struct inode));
      /* 用清0的内存数据覆盖磁盘 */
      ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
   } else {    // 未跨扇区,只读入1个扇区就好
      /* 将原硬盘上的内容先读出来 */
      ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
      /* 将inode_buf清0 */
      memset((inode_buf + inode_pos.off_size), 0, sizeof(struct inode));
      /* 用清0的内存数据覆盖磁盘 */
      ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
   }
}

/* 回收inode的数据块和inode本身 */
void inode_release(struct partition* part, uint32_t inode_no) {
   struct inode* inode_to_del = inode_open(part, inode_no);
   ASSERT(inode_to_del->i_no == inode_no);

/* 1 回收inode占用的所有块 */
   uint8_t block_idx = 0, block_cnt = 12;
   uint32_t block_bitmap_idx;
   uint32_t all_blocks[140] = {0};	  //12个直接块+128个间接块

   /* a 先将前12个直接块存入all_blocks */
   while (block_idx < 12) {
      all_blocks[block_idx] = inode_to_del->i_sectors[block_idx];
      block_idx++;
   }

   /* b 如果一级间接块表存在,将其128个间接块读到all_blocks[12~], 并释放一级间接块表所占的扇区 */
   if (inode_to_del->i_sectors[12] != 0) {
      ide_read(part->my_disk, inode_to_del->i_sectors[12], all_blocks + 12, 1);
      block_cnt = 140;

      /* 回收一级间接块表占用的扇区 */
      block_bitmap_idx = inode_to_del->i_sectors[12] - part->sb->data_start_lba;
      ASSERT(block_bitmap_idx > 0);
      bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
      bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
   }
   
   /* c inode所有的块地址已经收集到all_blocks中,下面逐个回收 */
   block_idx = 0;
   while (block_idx < block_cnt) {
      if (all_blocks[block_idx] != 0) {
	 block_bitmap_idx = 0;
	 block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
	 ASSERT(block_bitmap_idx > 0);
	 bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
	 bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
      }
      block_idx++; 
   }

/*2 回收该inode所占用的inode */
   bitmap_set(&part->inode_bitmap, inode_no, 0);  
   bitmap_sync(cur_part, inode_no, INODE_BITMAP);

   /******     以下inode_delete是调试用的    ******
   * 此函数会在inode_table中将此inode清0,
   * 但实际上是不需要的,inode分配是由inode位图控制的,
   * 硬盘上的数据不需要清0,可以直接覆盖*/
   void* io_buf = sys_malloc(1024);
   inode_delete(part, inode_no, io_buf);
   sys_free(io_buf);
   /***********************************************/
    
   inode_close(inode_to_del);
}

/* 初始化new_inode */
void inode_init(uint32_t inode_no, struct inode* new_inode){
    new_inode->i_no = inode_no;
    new_inode->i_size = 0;
    new_inode->i_open_cnts = 0;
    new_inode->write_deny = false;

    uint8_t sec_idx = 0;
    while (sec_idx < 13)
    {
        new_inode->i_sectors[sec_idx] = 0;
        sec_idx++;
    }
}