#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H

#include "stdint.h"
#include "bitmap.h"
#include "list.h"
#include "../thread/sync.h"
#include "list.h"
#include "print.h"
#include "../fs/fs.h"
#include "../fs/super_block.h"

//分区结构
struct partition
{
    uint32_t start_lba;          //起始扇区
    uint32_t sec_cnt;            //扇区数
    struct disk* my_disk;        //分区所属硬盘
    struct list_elem part_tag;   //所在队列中的标记
    char name[8];                //分区名字
    struct super_block* sb;	  //本分区 超级块
    struct bitmap block_bitmap;  //块位图
    struct bitmap inode_bitmap;  //i结点位图
    struct list open_inodes;     //本分区打开
};
//硬盘
struct disk
{
    char name[8];		      //本硬盘的名称
    struct ide_channel* my_channel;    //这块硬盘归属于哪个ide通道
    uint8_t dev_no;		      //0表示主盘 1表示从盘
    struct partition prim_parts[4];  //主分区顶多是4个
    struct partition logic_parts[8]; //逻辑分区最多支持8个
};

// ata 通道结构
struct ide_channel
{
    char name[8];		      //ata通道名称
    uint16_t port_base;	      //本通道的起始端口号
    uint8_t  irq_no;		      //本通道所用的中断号
    struct lock lock;                //通道锁 一个硬盘一通道 不能同时
    bool expecting_intr;	      //期待硬盘中断的bool
    struct semaphore disk_done;      //用于阻塞 唤醒驱动程序  和锁不一样 把自己阻塞后 把cpu腾出来
    struct disk devices[2];	      //一通道2硬盘 1主1从
};


extern uint8_t channel_cnt;
extern struct ide_channel channels[2];
extern struct list partition_list;

void ide_init(void);
void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
void intr_hd_handler(uint8_t irq_no);

#endif