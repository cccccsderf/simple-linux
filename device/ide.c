#include "ide.h"
#include "stdint.h"
#include "debug.h"
#include "stdio-kernel.h"
#include "../lib/stdio.h"
#include "global.h"
#include "../thread/sync.h"
#include "io.h"
#include "timer.h"
#include "interrupt.h"
#include "memory.h"
#include "list.h"

/* 定义硬盘各寄存器得端口号 */
#define reg_data(channel) 	  (channel->port_base + 0)
#define reg_error(channel) 	  (channel->port_base + 1)
#define reg_sect_cnt(channel)    (channel->port_base + 2)
#define reg_lba_l(channel)	  (channel->port_base + 3)
#define reg_lba_m(channel)	  (channel->port_base + 4)
#define reg_lba_h(channel)	  (channel->port_base + 5)
#define reg_dev(channel)	  (channel->port_base + 6)
#define reg_status(channel)	  (channel->port_base + 7)
#define reg_cmd(channel)	  (reg_status(channel))
#define reg_alt_status(channel)  (channel->port_base + 0x206)
#define reg_ctl(channel)	  reg_alt_status(channel)

#define BIT_STAT_BSY	  	  0X80		//硬盘忙
#define BIT_STAT_DRDY	  	  0X40		//驱动器准备好啦
#define BIT_STAT_DRQ 	  	  0x8		//数据传输准备好了
#define BIT_DEV_MBS		  0XA0		
#define BIT_DEV_LBA		  0X40
#define BIT_DEV_DEV		  0X10

#define CMD_IDENTIFY		  0XEC		//identify指令
#define CMD_READ_SECTOR	      0X20		//读扇区指令
#define CMD_WRITE_SECTOR	  0X30		//写扇区指令

#define max_lba		  ((80*1024*1024/512) - 1) //调试用

uint8_t channel_cnt;		  //通道数
struct ide_channel channels[2];  //两个ide通道

int32_t ext_lba_base = 0;	  //记录总拓展分区lba 初始为0
uint8_t p_no = 0,l_no = 0;	  //记录硬盘主分区下标 逻辑分区下标
struct list partition_list;	  //分区队列

struct partition_table_entry
{
    uint8_t bootable;		  //是否可引导
    uint8_t start_head;	  //开始磁头号
    uint8_t start_sec;		  //开始扇区号
    uint8_t start_chs;		  //起始柱面号
    uint8_t fs_type;		  //分区类型
    uint8_t end_head;		  //结束磁头号
    uint8_t end_sec;		  //结束扇区号
    uint8_t end_chs;		  //结束柱面号
    uint32_t start_lba;	  //本分区起始的lba地址
    uint32_t sec_cnt;		  //本扇区数目
} __attribute__ ((packed));

struct boot_sector
{
    uint8_t other[446];	  			//446 + 64 + 2 446是拿来占位置的
    struct partition_table_entry partition_table[4];  //分区表中4项 64字节
    uint16_t signature;				//最后的标识标志 魔数0x55 0xaa				
} __attribute__ ((packed));

// 选择读写的硬盘
static void select_disk(struct disk* hd){
    uint8_t reg_device = BIT_DEV_MBS | BIT_DEV_LBA;
    // 若是从盘就置dev为1
    if (hd->dev_no == 1)
    {
        reg_device |= BIT_DEV_DEV;
    }
    outb(reg_dev(hd->my_channel),reg_device);
}

// 向硬盘控制器写入起始扇区地址及要读写的扇区数
static void select_sector(struct disk* hd, uint32_t lba, uint8_t sec_cnt){
    ASSERT(lba <= max_lba);
    struct ide_channel* channel = hd->my_channel;
    // 写入要读写的扇区数
    outb(reg_sect_cnt(channel), sec_cnt);
    // 写入lba地址，即扇区号
    outb(reg_lba_l(channel),lba);
    outb(reg_lba_m(channel),lba >> 8);
    outb(reg_lba_h(channel),lba >> 16);

    outb(reg_dev(channel), BIT_DEV_MBS | BIT_DEV_LBA | (hd->dev_no == 1 ? BIT_DEV_DEV : 0) | lba >> 24);
}

// 向通道channel发命令cmd
static void cmd_out(struct ide_channel* channel, uint8_t cmd){
    channel->expecting_intr = true;
    outb(reg_cmd(channel), cmd);
}

// 硬盘读入sec_cnt个扇区的数据到buf
static void read_from_sector(struct disk* hd, void* buf, uint8_t sec_cnt){
    uint32_t size_in_byte;
    if (sec_cnt == 0)
    {
        size_in_byte = 256 * 512;
    }else{
        size_in_byte = sec_cnt * 512;
    }
    insw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

// 将buf中sec_cnt个扇区数据写入硬盘

static void write2sector(struct disk* hd, void* buf, uint8_t sec_cnt){
    uint32_t size_in_byte;
    if (sec_cnt == 0)
    {
        size_in_byte = 256 * 512;
    }else{
        size_in_byte = sec_cnt * 512;
    }
    outsw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

/* 等待30s */
static bool busy_wait(struct disk* hd){
    struct ide_channel* channel = hd->my_channel;
    uint16_t time_limit = 30 * 1000;
    while (time_limit -= 10 >= 0)
    {
        if (!(inb(reg_status(channel)) & BIT_STAT_BSY)){
            return (inb(reg_status(channel)) & BIT_STAT_DRQ);
        }else
        {
            mtime_sleep(10);
        }
    }
    return false;
}

//从硬盘读取sec_cnt扇区到buf
void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt)
{

    ASSERT(lba <= max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);
    select_disk(hd);
    uint32_t secs_op;
    uint32_t secs_done = 0;
    //  
    while(secs_done < sec_cnt)
    {
        if((secs_done + 256) <= sec_cnt)    secs_op = 256;
        else	secs_op = sec_cnt - secs_done;
        select_sector(hd,lba + secs_done, secs_op);
        cmd_out(hd->my_channel, CMD_READ_SECTOR); //执行命令
        
        // 问题出在这里

        /*在硬盘开始工作时 阻塞自己 完成读操作后唤醒自己*/
        //  线程走到这里被阻塞了
        sema_down(&hd->my_channel->disk_done);
        /*检测是否可读*/
        if(!busy_wait(hd))
        {
            char error[64];
            springtf(error,"%s read sector %d failed!!!!\n",hd->name,lba);
            PANIC(error);
        }
      	read_from_sector(hd,(void*)((uint32_t)buf +secs_done * 512),secs_op);
      	secs_done += secs_op;
    }
    lock_release(&hd->my_channel->lock);   
}
//从buf中读取sec_cnt扇区写入硬盘
void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt)
{
    ASSERT(lba <= max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);
    
    select_disk(hd);
    uint32_t secs_op;
    uint32_t secs_done = 0;
    while(secs_done < sec_cnt)
    {
        if((secs_done + 256) <= sec_cnt)    secs_op = 256;
        else	secs_op = sec_cnt - secs_done;
        
    	select_sector(hd,lba+secs_done,secs_op);
    	cmd_out(hd->my_channel,CMD_WRITE_SECTOR);
    	
    	if(!busy_wait(hd))
    	{
    	    char error[64];
    	    springtf(error,"%s write sector %d failed!!!!!!\n",hd->name,lba);
    	    PANIC(error);
    	}
    	
    	write2sector(hd,(void*)((uint32_t)buf + secs_done * 512),secs_op);
    	
    	//硬盘响应期间阻塞
    	sema_down(&hd->my_channel->disk_done);
    	secs_done += secs_op;
    }
    lock_release(&hd->my_channel->lock);
}

//硬盘结束任务中断程序
void intr_hd_handler(uint8_t irq_no)
{
    ASSERT(irq_no == 0x2e || irq_no == 0x2f);
    uint8_t ch_no = irq_no - 0x20 - 0xe;
    struct ide_channel* channel = &channels[ch_no];
    ASSERT(channel->irq_no == irq_no);
    if(channel->expecting_intr)
    {
	channel->expecting_intr = false;//结束任务了
	sema_up(&channel->disk_done);
	inb(reg_status(channel));
    }   
}

//将dst中len个相邻字节交换位置存入buf 因为读入的时候字节顺序是反的 所以我们再反一次即可
static void swap_pairs_bytes(const char* dst,char* buf,uint32_t len)
{
    uint8_t idx;
    for(idx = 0;idx < len;idx += 2)
    {
    	buf[idx+1] = *(dst++);
    	buf[idx]   = *(dst++);
    }
}

static void identify_disk(struct disk* hd)
{
    char id_info[512];
    select_disk(hd);
    cmd_out(hd->my_channel,CMD_IDENTIFY);
    
    if(!busy_wait(hd))
    {
    	char error[64];
    	springtf(error,"%s identify failed!!!!!!\n");
    	PANIC(error);
    }
    read_from_sector(hd,id_info,1);//现在硬盘已经把硬盘的参数准备好了了 我们把参数读到自己的缓冲区中
    
    char buf[64] = {0};
    uint8_t sn_start = 10 * 2,sn_len = 20,md_start = 27*2,md_len = 40;
    swap_pairs_bytes(&id_info[sn_start],buf,sn_len);
    printk("    disk %s info:        SN: %s\n",hd->name,buf);
    swap_pairs_bytes(&id_info[md_start],buf,md_len);
    printk("    MODULE: %s\n",buf);
    uint32_t sectors = *(uint32_t*)&id_info[60*2];
    printk("    SECTORS: %d\n",sectors);
    printk("    CAPACITY: %dMB\n",sectors * 512 / 1024 / 1024);
}

/* 扫描硬盘hd中地址为ext_lba的扇区中的所有分区 */
static void partition_scan(struct disk* hd,uint32_t ext_lba)
{
    struct boot_sector* bs = sys_malloc(sizeof(struct boot_sector));
    ide_read(hd,ext_lba,bs,1);
    uint8_t part_idx = 0;
    struct partition_table_entry* p = bs->partition_table; //p为分区表开始的位置
    while((part_idx++) < 4)
    {
    	if(p->fs_type == 0x5) //拓展分区
    	{
    	    
    	    if(ext_lba_base != 0)
    	    {
    	    	partition_scan(hd,p->start_lba + ext_lba_base);	//继续递归转到下一个逻辑分区再次得到表
    	    }
    	    else //第一次读取引导块
    	    {
    	    	ext_lba_base = p->start_lba;
    	    	partition_scan(hd,ext_lba_base);
    	    }
    	}
    	else if(p->fs_type != 0)
    	{
    	    if(ext_lba == 0)	//主分区
    	    {
    	    	hd->prim_parts[p_no].start_lba = ext_lba + p->start_lba;
    	    	hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
    	    	hd->prim_parts[p_no].my_disk = hd;
    	    	list_append(&partition_list,&hd->prim_parts[p_no].part_tag);
    	    	springtf(hd->prim_parts[p_no].name,"%s%d",hd->name,p_no+1);
    	    	p_no++;
    	    	ASSERT(p_no<4);	//0 1 2 3 最多四个
    	    }
    	    else		//其他分区
    	    {
    	    	hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
    	    	hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
    	    	hd->logic_parts[l_no].my_disk = hd;
    	    	list_append(&partition_list,&hd->logic_parts[l_no].part_tag);
    	    	springtf(hd->logic_parts[l_no].name,"%s%d",hd->name,l_no+5); //从5开始
    	    	l_no++;
    	    	if(l_no >= 8)	return; //只支持8个
    	    }
    	}
    	++p;
    }
    sys_free(bs);
}

static bool partition_info(struct list_elem* pelem,int arg UNUSED)
{
    struct partition* part = elem2entry(struct partition,part_tag,pelem);
    printk("    %s  start_lba:0x%x,sec_cnt:0x%x\n",part->name,part->start_lba,part->sec_cnt);
    return false; //list_pop完
}

/* 硬盘数据结构初始化 */
void ide_init() {
   printk("ide_init start\n");
   uint8_t hd_cnt = *((uint8_t*)(0x475));	      // 获取硬盘的数量
   ASSERT(hd_cnt > 0);
   list_init(&partition_list);
   channel_cnt = DIV_ROUND_UP(hd_cnt, 2);	   // 一个ide通道上有两个硬盘,根据硬盘数量反推有几个ide通道
   struct ide_channel* channel;
   uint8_t channel_no = 0,dev_no = 0; 
   /* 处理每个通道上的硬盘 */
   while (channel_no < channel_cnt) {
      channel = &channels[channel_no];
      springtf(channel->name, "ide%d", channel_no);
      /* 为每个ide通道初始化端口基址及中断向量 */
      switch (channel_no) {
	  case 0:
	    channel->port_base	 = 0x1f0;	   // ide0通道的起始端口号是0x1f0
	    channel->irq_no	 = 0x20 + 14;	   // 从片8259a上倒数第二的中断引脚,温盘,也就是ide0通道的的中断向量号
	    break;
	  case 1:
	    channel->port_base	 = 0x170;	   // ide1通道的起始端口号是0x170
	    channel->irq_no	 = 0x20 + 15;	   // 从8259A上的最后一个中断引脚,我们用来响应ide1通道上的硬盘中断
	    break;
      }
      channel->expecting_intr = false;		   // 未向硬盘写入指令时不期待硬盘的中断
      lock_init(&channel->lock);		     

   /* 初始化为0,目的是向硬盘控制器请求数据后,硬盘驱动sema_down此信号量会阻塞线程,
   直到硬盘完成后通过发中断,由中断处理程序将此信号量sema_up,唤醒线程. */
      sema_init(&channel->disk_done, 0);

      register_handler(channel->irq_no, intr_hd_handler);
      /* 分别获取两个硬盘的参数及分区信息 */
      while (dev_no < 2) {
	    struct disk* hd = &channel->devices[dev_no];
	    hd->my_channel = channel;
	    hd->dev_no = dev_no;
	    springtf(hd->name, "sd%c", 'a' + channel_no * 2 + dev_no);
	    identify_disk(hd);	 // 获取硬盘参数
	    if (dev_no != 0) {	 // 内核本身的裸硬盘(hd60M.img)不处理
	       partition_scan(hd, 0);  // 扫描该硬盘上的分区  
	    }
	    p_no = 0, l_no = 0;
	    //dev_no++; 
        dev_no = dev_no + 1;
      }
      dev_no = 0;			  	   // 将硬盘驱动器号置0,为下一个channel的两个硬盘初始化。
      channel_no++;				   // 下一个channel
   }
   printk("\n   all partition info\n");
   /* 打印所有分区信息 */
   list_traversal(&partition_list, partition_info, (int)NULL);
   printk("ide_init done\n");

}