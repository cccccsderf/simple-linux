#include "fs.h"
#include "stdint.h"
#include "global.h"
#include "inode.h"
#include "dir.h"
#include "super_block.h"
#include "stdio-kernel.h"
#include "string.h"
#include "debug.h"
#include "list.h"
#include "file.h"
#include "../device/console.h"
#include "../device/keyboard.h"
#include "../device/ioqueue.h"

struct partition* cur_part;	//默认操作分区
static bool mount_partition(struct list_elem* pelem, int arg){
    // 分区名称
    char* part_name = (char*) arg;
    struct partition* part = elem2entry(struct partition, part_tag, pelem);
    if (!strcmp(part->name, part_name))
    {
        cur_part = part;
        struct disk* hd = cur_part->my_disk;
        // sb_buf用来存储从硬盘上读取的超级快
        struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);
        // 在内存中创建当前操作分区的超级快
        cur_part->sb = (struct super_block*)sys_malloc(sizeof(struct super_block));
        if (cur_part->sb == 0)
        {
            PANIC("alloc memory failed!");
        }
        // 将sb_buf全部置0  防止脏数据
        memset(sb_buf, 0, SECTOR_SIZE);
        // 从硬盘读入超级快放到sb_buf缓冲区中
        ide_read(hd,cur_part->start_lba + 1,sb_buf,1);
        // 把sb_buf中的超级块信息复制到分区的超级块sb中
        memcpy(cur_part->sb, sb_buf,sizeof(struct super_block));

        // 将硬盘上的块位图读入内存
        cur_part->block_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);
        if (cur_part->block_bitmap.bits == NULL)
        {
            PANIC("alloc memory failed!");
        }
        cur_part->block_bitmap.btmp_byte_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
        ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits, sb_buf->block_bitmap_sects);

        // 将硬盘的inode位图读入内存
        cur_part->inode_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
        if (cur_part->inode_bitmap.bits == NULL)
        {
            PANIC("alloc memory failed!");
        }
        cur_part->inode_bitmap.btmp_byte_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
        ide_read(hd,sb_buf->block_bitmap_lba,cur_part->inode_bitmap.bits,sb_buf->inode_bitmap_sects);
        list_init(&cur_part->open_inodes);
        printk("mount %s done!\n",part->name);
        return true;
    }
    return false;
}
// 格式化分区，初始化分区的元信息
static void partition_format(struct disk* hd, struct partition* part){
    // hd：硬盘结构体   part：分区结构体
    // block_bitmap_init(为了方便实现，一个块就是一个扇区（512kb）) 一个块也可以事4kb
    uint32_t boot_sector_sects = 1; // 引导块一个块
    uint32_t super_block_sects = 1; // 超级块一个块
    uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART,BITS_PER_SECTOR); //inode位图占的块数

    //inode数组所占的块数
    uint32_t inode_table_sects = DIV_ROUND_UP((sizeof(struct inode) * MAX_FILES_PER_PART), SECTOR_SIZE);
    
    //注意这里的used_sects 肯定是不准确 差了那么一点点的 因为还没有包含block_bitmap_sects 但是为了简单处理 要先得到free_sects才能推  所以到后面block_bitmap_sects 要除两次
    uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
    uint32_t free_sects = part->sec_cnt - used_sects;
    
    uint32_t block_bitmap_sects = DIV_ROUND_UP(free_sects,BITS_PER_SECTOR);	//一位一块
    uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;	//再减去block_bitmap的
    block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len,BITS_PER_SECTOR);
    
    struct super_block sb;	        		//利用栈来初始化超级块 我们的栈此刻在
    sb.magic         = 0x23333333;			//魔数
    sb.sec_cnt       = part->sec_cnt; 		//该分区总扇区数
    sb.inode_cnt     = MAX_FILES_PER_PART;		//该分区总inode数
    sb.part_lba_base = part->start_lba;		//该分区lba起始扇区位置
    
    // 引导块 超级块 空闲块位图 inode位图 inode数组 根目录 空闲块区域
    // 挨着挨着顺序赋值即可
    sb.block_bitmap_lba   = part->start_lba + boot_sector_sects + super_block_sects;
    sb.block_bitmap_sects = block_bitmap_sects;
    
    sb.inode_bitmap_lba   = sb.block_bitmap_lba + block_bitmap_sects;
    sb.inode_bitmap_sects = inode_bitmap_sects;
    
    sb.inode_table_lba    = sb.inode_bitmap_lba + inode_bitmap_sects;
    sb.inode_table_sects  = inode_table_sects;
    
    sb.data_start_lba     = sb.inode_table_lba + inode_table_sects;
    sb.root_inode_no	  = 0;			//根目录inode起始编号 0 
    sb.dir_entry_size     = sizeof(struct dir_entry); //目录项大小
    
    printk("%s  info:\n",part->name);
    printk("    magic:0x%x\n    part_lba_base:0x%x\n    all_sectors:0x%x\n    \
                inode_cnt:0x%x\n    block_bitmap_lba:0x%x\n    block_bitmap_sectors:0x%x\n    \
                inode_bitmap_lba:0x%x\n    inode_bitmap_sectors:0x%x\n    \
                inode_table_lba:0x%x\n    inode_table_sectors:0x%x\n    \
                data_start_lba:0x%x\n", \
    sb.magic,sb.part_lba_base,sb.sec_cnt,sb.inode_cnt,sb.block_bitmap_lba,sb.block_bitmap_sects,\
    sb.inode_bitmap_lba,sb.inode_bitmap_sects,sb.inode_table_lba,\
    sb.inode_table_sects,sb.data_start_lba);   
    
    //把元信息挨个挨个写进硬盘
    ide_write(hd,part->start_lba + boot_sector_sects,&sb,super_block_sects);
    printk("    super_block_lba:0x%x\n",part->start_lba + 1);
    
    //找一个最大的数据缓冲区 我们的栈已经不足以满足我们的各种信息的储存了 之后还要把元信息给腾到硬盘中
    uint32_t buf_size = (sb.block_bitmap_sects >= sb.inode_bitmap_sects) ? sb.block_bitmap_sects : sb.inode_bitmap_sects;
    buf_size = ((buf_size >= inode_table_sects) ? buf_size : inode_table_sects) * SECTOR_SIZE;
    //申请缓冲空间 给元信息腾空间 设置成uint8_t* 原因是 先弄块位图的初始化
    uint8_t* buf = (uint8_t*)sys_malloc(buf_size);
    
    /* 初始化块位图了 */
    buf[0] |= 0x1;
    uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8; //先算算占用多少字节
    uint8_t block_bitmap_last_bit  = block_bitmap_bit_len % 8; //最后还有剩余多少位
    uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE); //先除余数 算出来多少字节空的
    
    //处理字节 把可能多的一字节全部置成1 这几步处理的很细节阿
    memset(&buf[block_bitmap_last_byte],0xff,last_size);	 //全部置1 保证不会被使用
    
    //处理最后的位 有效位变成0 用~来处理 真的很妙
    uint8_t bit_idx = 0;
    while(bit_idx <= block_bitmap_last_bit)
    	buf[block_bitmap_last_byte] &= ~(1 << (bit_idx++));	//有效位
    
    //把位图元信息给写到硬盘中 块位图的部分就结束了 还有inode位图 inode数组等着我们
    ide_write(hd,sb.block_bitmap_lba,buf,sb.block_bitmap_sects);
    
    /*初始化inode位图了*/
    memset(buf,0,buf_size);
    buf[0] |= 0x1;	                                          //第一个inode用于存根目录
    ide_write(hd,sb.inode_bitmap_lba,buf,sb.inode_bitmap_sects); //第一个inode初始化在后面
    
    /*初始化inode数组了*/
    memset(buf,0,buf_size);
    struct inode* i = (struct inode*)buf;			//先初始化第一个inode 根目录所在的
    i->i_size = sb.dir_entry_size * 2;			//. 和 .. 
    i->i_no   = 0;
    i->i_sectors[0]  = sb.data_start_lba;			//根目录所在扇区就是最开始的第一个扇区
    
    ide_write(hd,sb.inode_table_lba,buf,sb.inode_table_sects);
    
    /*写根目录文件进入 第一个扇区了*/
    memset(buf,0,buf_size);
    struct dir_entry* p_de = (struct dir_entry*)buf;
    
    memcpy(p_de->filename,".",1);				//名称
    p_de->i_no = 0;						//根目录. inode仍然是自己
    p_de->f_type = FT_DIRECTORY;
    p_de++;							//移动到下一条目录项
    
    memcpy(p_de->filename,"..",2);
    p_de->i_no = 0;						//根目录的父目录仍然是自己 因为自己是固定好的 根基
    p_de->f_type = FT_DIRECTORY;
    
    ide_write(hd,sb.data_start_lba,buf,1);			//把根目录文件写到第一个扇区中
    
    printk("    root_dir_lba:0x%x\n",sb.data_start_lba);
    printk("%s format done\n",part->name);
    sys_free(buf);						//临时借用的 现在得还回去了 
}

//解析路径 并把下一级路径的字符串赋值给name_store 返回现在已经解析完的指针位置
char* path_parse(char* pathname,char* name_store)
{
    if(pathname[0] == '/')
        while(*(++pathname) == '/');	//直到pathname位置不是

    while(*pathname != '/' && *pathname != '\0')
    	*(name_store++) = *(pathname++);
    	
    if(pathname[0] == 0)	return NULL;
    return pathname;
}

//返回路径的层数   比如/a/b/c为3
int32_t path_depth_cnt(char* pathname)
{
    ASSERT(pathname != NULL);
    char* p = pathname;
    char name[MAX_FILE_NAME_LEN];
    
    uint32_t depth = 0;
    
    p = path_parse(p,name);
    while(name[0])
    {
    	++depth;
    	memset(name,0,MAX_FILE_NAME_LEN);
    	if(p)
    	    p = path_parse(p,name);
    }
    return depth;
}

//搜索文件 找到则返回inode号 否则返回-1
int search_file(const char* pathname,struct path_search_record* searched_record)
{
    //如果是根目录 则直接判定返回即可 下面的工作就不需要做了
    if(!strcmp(pathname,"/") || !strcmp(pathname,"/.") || !strcmp(pathname,"/.."))
    {
    	searched_record->parent_dir = &root_dir;
    	searched_record->file_type  = FT_DIRECTORY;
    	searched_record->searched_path[0] = 0;		//置空
    	return 0;	//根目录inode编号为0
    }
    
    uint32_t path_len = strlen(pathname);
    
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);		
    char* sub_path = (char*)pathname;
    struct dir* parent_dir = &root_dir;			//每个刚开始都是从根目录开始
    struct dir_entry dir_e;					//存放目录项的临时变量
    
    char name[MAX_FILE_NAME_LEN] = {0};
    
    searched_record->parent_dir = parent_dir;
    searched_record->file_type  = FT_UNKNOWN;
    uint32_t parent_inode_no = 0;				//父目录的inode号
    
    sub_path = path_parse(sub_path,name);			//解析目录
    while(name[0])
    {
    	ASSERT(strlen(searched_record->searched_path) < 512);
    	strcat(searched_record->searched_path,"/");
    	strcat(searched_record->searched_path,name);
    	
    	if(search_dir_entry(cur_part,parent_dir,name,&dir_e))
    	{
    	    memset(name,0,MAX_FILE_NAME_LEN);
    	    if(sub_path)    sub_path = path_parse(sub_path,name);   
    	    
    	    if(FT_DIRECTORY == dir_e.f_type)	//打开的是目录继续解析即可
    	    {
    	    	
    	    	parent_inode_no = parent_dir->inode->i_no;
    	    	dir_close(parent_dir);
    	    	parent_dir = dir_open(cur_part,dir_e.i_no);
    	    	searched_record->parent_dir = parent_dir;
    	    }
    	    else if(FT_REGULAR == dir_e.f_type)
    	    {
    	    	searched_record->file_type = FT_REGULAR;
    	    	return dir_e.i_no;
    	    }
    	}
    	else 	return -1;
    }
    
    dir_close(searched_record->parent_dir);
    searched_record->parent_dir = dir_open(cur_part,parent_inode_no);
    searched_record->file_type = FT_DIRECTORY;
    return dir_e.i_no;
}
// 打开或创建文件成功后，返回文件描述符 
int32_t sys_open(const char* pathname,uint8_t flags)
{
    //最后一位是'/'则无法辨析 这里是打开文件
    if(pathname[strlen(pathname) - 1] == '/')
    {
    	printk("cant open a directory %s\n",pathname);
    	return -1;
    }
    ASSERT(flags <= 7);
    int32_t fd = -1;
    
    struct path_search_record searched_record;	     //记录访问记录
    memset(&searched_record,0,sizeof(struct path_search_record));
    
    uint32_t pathname_depth = path_depth_cnt((char*)pathname);
    
    int inode_no = search_file(pathname,&searched_record); //搜索文件
    bool found = (inode_no == -1) ? false : true;
    
    if(searched_record.file_type == FT_DIRECTORY)	     //如果是目录文件类型
    {
    	printk("cant open a directory with open(),use opendir() to instead\n");
    	dir_close(searched_record.parent_dir);
    	return -1;
    }
    
    uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
    
    if(pathname_depth != path_searched_depth)
    {
    	printk("cannot access %s:Not a directory,subpath %s isnt exist\n",pathname,searched_record.searched_path);
    	dir_close(searched_record.parent_dir);
    	return -1;	
    }
    
    if(!found && !(flags & O_CREAT))
    {
    	printk("in path %s: Not a directory, subpath %s isnt exist\n",pathname,searched_record.searched_path);
    	dir_close(searched_record.parent_dir);
    	return -1;
    }
    else if(found && (flags & O_CREAT))
    {
    	printk("%s has already exist!\n",pathname);
    	dir_close(searched_record.parent_dir);
    	return -1;
    }
    
    switch(flags & O_CREAT)
    {
    	case O_CREAT:
    	    printk("creating file\n");
    	    fd = file_create(searched_record.parent_dir,strrchr(pathname,'/') + 1,flags);
    	    dir_close(searched_record.parent_dir);
    	    break;
        default:
            fd = file_open(inode_no,flags);
    }
    
    return fd;
}


//局部pcb文件描述符表返回全局表下标号
uint32_t fd_local2global(uint32_t local_fd)
{
    struct task_struct* cur = running_thread();
    int32_t global_fd = cur->fd_table[local_fd];
    ASSERT(global_fd >= 0 && global_fd <= MAX_FILE_OPEN);
    return (uint32_t)global_fd;
}

//关闭pcb文件描述符fd的文件 成功返回0 失败-1
int32_t sys_close(int32_t fd)
{
    int32_t ret = -1;	//返回值默认为-1
    if(fd > 2)		// 0 1都是标准输入 输出
    {
    	uint32_t _fd = fd_local2global(fd);
    	ret = file_close(&file_table[_fd]);
    	running_thread()->fd_table[fd] = -1;	//pcb fd恢复可用
    }
    return ret;
}

//写入连续的count字节到文件描述符fd
int32_t sys_write(int32_t fd,const void* buf,uint32_t count)
{
    if(fd < 0)
    {
        printk("sys_write: fd error\n");
        return -1;
    }
    if(fd == stdout_no)
    {
        char tmp_buf[1024] = {0};
        memcpy(tmp_buf,buf,count);
        console_put_str(tmp_buf);
        return count;
    }
    
    uint32_t _fd = fd_local2global(fd);
    struct file* wr_file = &file_table[_fd];
    if(wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR)
    {
        uint32_t bytes_written = file_write(wr_file,buf,count);
        return bytes_written;
    }
    else
    {
        console_put_str("sys_write: not allowed to write file without flag O_RDWR or O_WRONLY\n");
        return -1;
    }
}

int32_t sys_read(int32_t fd,void* buf,uint32_t count)
{
    ASSERT(buf != -1);
    int32_t ret = -1;
    if(fd < 0 || fd == stdout_no || fd == stderr_no)
    {
    	printk("sys_read: fd error\n");
    }
    else if(fd == stdin_no)
    {
        char* buffer = buf;
        uint32_t bytes_read = 0;
        while(bytes_read < count)
        {
            // 错误
            *buffer = ioq_getchar(&kbd_buf);
            bytes_read++;
            buffer++;
        }
        ret = (bytes_read == 0 ? -1 : (int32_t)bytes_read);
    }
    else
    {
        uint32_t _fd = fd_local2global(fd);
        ret = file_read(&file_table[_fd],buf,count);
    }   
    return ret;
}

int32_t sys_lseek(int32_t fd,int32_t offset,uint8_t whence)
{
    if(fd < 0)
    {
        printk("sys_lseek: fd error\n");
        return -1;
    }
    ASSERT(whence > 0 && whence < 4);
    uint32_t _fd = fd_local2global(fd);
    struct file* file = &file_table[_fd];
    int32_t new_pos = 0;
    int32_t file_size = (int32_t)file->fd_inode->i_size;
    
    switch(whence)
    {
        case SEEK_SET:
            new_pos = offset; //相对文件开始的偏移
            break;
        case SEEK_CUR:
            new_pos = offset + (int32_t)file->fd_pos;
            break;
        case SEEK_END:
            new_pos = offset + (int32_t)file_size;
            break;    
    }
    if(new_pos < 0 || new_pos > (file_size -1))
    	return -1;
   
    file->fd_pos = new_pos;
    return file->fd_pos;
}

/* 删除文件(非目录),成功返回0,失败返回-1 */
int32_t sys_unlink(const char* pathname) {
   ASSERT(strlen(pathname) < MAX_PATH_LEN);

   /* 先检查待删除的文件是否存在 */
   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = search_file(pathname, &searched_record);
   ASSERT(inode_no != 0);
   if (inode_no == -1) {
      printk("file %s not found!\n", pathname);
      dir_close(searched_record.parent_dir);
      return -1;
   }
   if (searched_record.file_type == FT_DIRECTORY) {
      printk("can`t delete a direcotry with unlink(), use rmdir() to instead\n");
      dir_close(searched_record.parent_dir);
      return -1;
   }

   /* 检查是否在已打开文件列表(文件表)中 */
   uint32_t file_idx = 0;
   while (file_idx < MAX_FILE_OPEN) {
      if (file_table[file_idx].fd_inode != NULL && (uint32_t)inode_no == file_table[file_idx].fd_inode->i_no) {
	 break;
      }
      file_idx++;
   }
   if (file_idx < MAX_FILE_OPEN) {
      dir_close(searched_record.parent_dir);
      printk("file %s is in use, not allow to delete!\n", pathname);
      return -1;
   }
   ASSERT(file_idx == MAX_FILE_OPEN);
   
   /* 为delete_dir_entry申请缓冲区 */
   void* io_buf = sys_malloc(SECTOR_SIZE + SECTOR_SIZE);
   if (io_buf == NULL) {
      dir_close(searched_record.parent_dir);
      printk("sys_unlink: malloc for io_buf failed\n");
      return -1;
   }

   struct dir* parent_dir = searched_record.parent_dir;  
   delete_dir_entry(cur_part, parent_dir, inode_no, io_buf);
   inode_release(cur_part, inode_no);
   sys_free(io_buf);
   dir_close(searched_record.parent_dir);
   return 0;   // 成功删除文件 
}

/* 创建目录pathname,成功返回0,失败返回-1 */
int32_t sys_mkdir(const char* pathname) {
   uint8_t rollback_step = 0;	       // 用于操作失败时回滚各资源状态
   void* io_buf = sys_malloc(SECTOR_SIZE * 2);
   if (io_buf == NULL) {
      printk("sys_mkdir: sys_malloc for io_buf failed\n");
      return -1;
   }

   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = -1;
   inode_no = search_file(pathname, &searched_record);
   if (inode_no != -1) {      // 如果找到了同名目录或文件,失败返回
      printk("sys_mkdir: file or directory %s exist!\n", pathname);
      rollback_step = 1;
      goto rollback;
   } else {	     // 若未找到,也要判断是在最终目录没找到还是某个中间目录不存在
      uint32_t pathname_depth = path_depth_cnt((char*)pathname);
      uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
      /* 先判断是否把pathname的各层目录都访问到了,即是否在某个中间目录就失败了 */
      if (pathname_depth != path_searched_depth) {   // 说明并没有访问到全部的路径,某个中间目录是不存在的
	 printk("sys_mkdir: can`t access %s, subpath %s is`t exist\n", pathname, searched_record.searched_path);
	 rollback_step = 1;
	 goto rollback;
      }
   }

   struct dir* parent_dir = searched_record.parent_dir;
   /* 目录名称后可能会有字符'/',所以最好直接用searched_record.searched_path,无'/' */
   char* dirname = strrchr(searched_record.searched_path, '/') + 1;

   inode_no = inode_bitmap_alloc(cur_part); 
   if (inode_no == -1) {
      printk("sys_mkdir: allocate inode failed\n");
      rollback_step = 1;
      goto rollback;
   }

   struct inode new_dir_inode;
   inode_init(inode_no, &new_dir_inode);	    // 初始化i结点

   uint32_t block_bitmap_idx = 0;     // 用来记录block对应于block_bitmap中的索引
   int32_t block_lba = -1;
/* 为目录分配一个块,用来写入目录.和.. */
   block_lba = block_bitmap_alloc(cur_part);
   if (block_lba == -1) {
      printk("sys_mkdir: block_bitmap_alloc for create directory failed\n");
      rollback_step = 2;
      goto rollback;
   }
   new_dir_inode.i_sectors[0] = block_lba;
   /* 每分配一个块就将位图同步到硬盘 */
   block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
   ASSERT(block_bitmap_idx != 0);
   bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
   
   /* 将当前目录的目录项'.'和'..'写入目录 */
   memset(io_buf, 0, SECTOR_SIZE * 2);	 // 清空io_buf
   struct dir_entry* p_de = (struct dir_entry*)io_buf;
   
   /* 初始化当前目录"." */
   memcpy(p_de->filename, ".", 1);
   p_de->i_no = inode_no ;
   p_de->f_type = FT_DIRECTORY;

   p_de++;
   /* 初始化当前目录".." */
   memcpy(p_de->filename, "..", 2);
   p_de->i_no = parent_dir->inode->i_no;
   p_de->f_type = FT_DIRECTORY;
   ide_write(cur_part->my_disk, new_dir_inode.i_sectors[0], io_buf, 1);

   new_dir_inode.i_size = 2 * cur_part->sb->dir_entry_size;

   /* 在父目录中添加自己的目录项 */
   struct dir_entry new_dir_entry;
   memset(&new_dir_entry, 0, sizeof(struct dir_entry));
   create_dir_entry(dirname, inode_no, FT_DIRECTORY, &new_dir_entry);
   memset(io_buf, 0, SECTOR_SIZE * 2);	 // 清空io_buf
   if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)) {	  // sync_dir_entry中将block_bitmap通过bitmap_sync同步到硬盘
      printk("sys_mkdir: sync_dir_entry to disk failed!\n");
      rollback_step = 2;
      goto rollback;
   }

   /* 父目录的inode同步到硬盘 */
   memset(io_buf, 0, SECTOR_SIZE * 2);
   inode_sync(cur_part, parent_dir->inode, io_buf);

   /* 将新创建目录的inode同步到硬盘 */
   memset(io_buf, 0, SECTOR_SIZE * 2);
   inode_sync(cur_part, &new_dir_inode, io_buf);

   /* 将inode位图同步到硬盘 */
   bitmap_sync(cur_part, inode_no, INODE_BITMAP);

   sys_free(io_buf);

   /* 关闭所创建目录的父目录 */
   dir_close(searched_record.parent_dir);
   return 0;

/*创建文件或目录需要创建相关的多个资源,若某步失败则会执行到下面的回滚步骤 */
rollback:	     // 因为某步骤操作失败而回滚
   switch (rollback_step) {
      case 2:
	 bitmap_set(&cur_part->inode_bitmap, inode_no, 0);	 // 如果新文件的inode创建失败,之前位图中分配的inode_no也要恢复 
      case 1:
	 /* 关闭所创建目录的父目录 */
	 dir_close(searched_record.parent_dir);
	 break;
   }
   sys_free(io_buf);
   return -1;
}

/* 目录打开成功后返回目录指针,失败返回NULL */
struct dir* sys_opendir(const char* name) {
   ASSERT(strlen(name) < MAX_PATH_LEN);
   /* 如果是根目录'/',直接返回&root_dir */
   if (name[0] == '/' && (name[1] == 0 || name[0] == '.')) {
      return &root_dir;
   }

   /* 先检查待打开的目录是否存在 */
   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = search_file(name, &searched_record);
   struct dir* ret = NULL;
   if (inode_no == -1) {	 // 如果找不到目录,提示不存在的路径 
      printk("In %s, sub path %s not exist\n", name, searched_record.searched_path); 
   } else {
      if (searched_record.file_type == FT_REGULAR) {
	 printk("%s is regular file!\n", name);
      } else if (searched_record.file_type == FT_DIRECTORY) {
	 ret = dir_open(cur_part, inode_no);
      }
   }
   dir_close(searched_record.parent_dir);
   return ret;
}

/* 成功关闭目录dir返回0,失败返回-1 */
int32_t sys_closedir(struct dir* dir) {
   int32_t ret = -1;
   if (dir != NULL) {
      dir_close(dir);
      ret = 0;
   }
   return ret;
}

/* 读取目录dir的1个目录项,成功后返回其目录项地址,到目录尾时或出错时返回NULL */
struct dir_entry* sys_readdir(struct dir* dir) {
   ASSERT(dir != NULL);
   return dir_read(dir);
}

/* 把目录dir的指针dir_pos置0 */
void sys_rewinddir(struct dir* dir) {
   dir->dir_pos = 0;
}

/* 删除空目录,成功时返回0,失败时返回-1*/
int32_t sys_rmdir(const char* pathname) {
   /* 先检查待删除的文件是否存在 */
   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = search_file(pathname, &searched_record);
   ASSERT(inode_no != 0);
   int retval = -1;	// 默认返回值
   if (inode_no == -1) {
      printk("In %s, sub path %s not exist\n", pathname, searched_record.searched_path); 
   } else {
      if (searched_record.file_type == FT_REGULAR) {
	 printk("%s is regular file!\n", pathname);
      } else { 
	 struct dir* dir = dir_open(cur_part, inode_no);
	 if (!dir_is_empty(dir)) {	 // 非空目录不可删除
	    printk("dir %s is not empty, it is not allowed to delete a nonempty directory!\n", pathname);
	 } else {
	    if (!dir_remove(searched_record.parent_dir, dir)) {
	       retval = 0;
	    }
	 }
	 dir_close(dir);
      }
   }
   dir_close(searched_record.parent_dir);
   return retval;
}

/* 获得父目录的inode编号 */
static uint32_t get_parent_dir_inode_nr(uint32_t child_inode_nr, void* io_buf) {
   struct inode* child_dir_inode = inode_open(cur_part, child_inode_nr);
   /* 目录中的目录项".."中包括父目录inode编号,".."位于目录的第0块 */
   uint32_t block_lba = child_dir_inode->i_sectors[0];
   ASSERT(block_lba >= cur_part->sb->data_start_lba);
   inode_close(child_dir_inode);
   ide_read(cur_part->my_disk, block_lba, io_buf, 1);   
   struct dir_entry* dir_e = (struct dir_entry*)io_buf;
   /* 第0个目录项是".",第1个目录项是".." */
   ASSERT(dir_e[1].i_no < 4096 && dir_e[1].f_type == FT_DIRECTORY);
   return dir_e[1].i_no;      // 返回..即父目录的inode编号
}

/* 在inode编号为p_inode_nr的目录中查找inode编号为c_inode_nr的子目录的名字,
 * 将名字存入缓冲区path.成功返回0,失败返-1 */
static int get_child_dir_name(uint32_t p_inode_nr, uint32_t c_inode_nr, char* path, void* io_buf) {
   struct inode* parent_dir_inode = inode_open(cur_part, p_inode_nr);
   /* 填充all_blocks,将该目录的所占扇区地址全部写入all_blocks */
   uint8_t block_idx = 0;
   uint32_t all_blocks[140] = {0}, block_cnt = 12;
   while (block_idx < 12) {
      all_blocks[block_idx] = parent_dir_inode->i_sectors[block_idx];
      block_idx++;
   }
   if (parent_dir_inode->i_sectors[12]) {	// 若包含了一级间接块表,将共读入all_blocks.
      ide_read(cur_part->my_disk, parent_dir_inode->i_sectors[12], all_blocks + 12, 1);
      block_cnt = 140;
   }
   inode_close(parent_dir_inode);

   struct dir_entry* dir_e = (struct dir_entry*)io_buf;
   uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
   uint32_t dir_entrys_per_sec = (512 / dir_entry_size);
   block_idx = 0;
  /* 遍历所有块 */
   while(block_idx < block_cnt) {
      if(all_blocks[block_idx]) {      // 如果相应块不为空则读入相应块
	 ide_read(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
	 uint8_t dir_e_idx = 0;
	 /* 遍历每个目录项 */
	 while(dir_e_idx < dir_entrys_per_sec) {
	    if ((dir_e + dir_e_idx)->i_no == c_inode_nr) {
	       strcat(path, "/");
	       strcat(path, (dir_e + dir_e_idx)->filename);
	       return 0;
	    }
	    dir_e_idx++;
	 }
      }
      block_idx++;
   }
   return -1;
}

/* 把当前工作目录绝对路径写入buf, size是buf的大小. 
 当buf为NULL时,由操作系统分配存储工作路径的空间并返回地址
 失败则返回NULL */
char* sys_getcwd(char* buf, uint32_t size) {
   /* 确保buf不为空,若用户进程提供的buf为NULL,
   系统调用getcwd中要为用户进程通过malloc分配内存 */
   ASSERT(buf != NULL);
   void* io_buf = sys_malloc(SECTOR_SIZE);
   if (io_buf == NULL) {
      return NULL;
   }

   struct task_struct* cur_thread = running_thread();
   int32_t parent_inode_nr = 0;
   int32_t child_inode_nr = cur_thread->cwd_inode_nr;
   ASSERT(child_inode_nr >= 0 && child_inode_nr < 4096);      // 最大支持4096个inode
   /* 若当前目录是根目录,直接返回'/' */
   if (child_inode_nr == 0) {
      buf[0] = '/';
      buf[1] = 0;
      return buf;
   }

   memset(buf, 0, size);
   char full_path_reverse[MAX_PATH_LEN] = {0};	  // 用来做全路径缓冲区

   /* 从下往上逐层找父目录,直到找到根目录为止.
    * 当child_inode_nr为根目录的inode编号(0)时停止,
    * 即已经查看完根目录中的目录项 */
   while ((child_inode_nr)) {
      parent_inode_nr = get_parent_dir_inode_nr(child_inode_nr, io_buf);
      if (get_child_dir_name(parent_inode_nr, child_inode_nr, full_path_reverse, io_buf) == -1) {	  // 或未找到名字,失败退出
	 sys_free(io_buf);
	 return NULL;
      }
      child_inode_nr = parent_inode_nr;
   }
   ASSERT(strlen(full_path_reverse) <= size);
/* 至此full_path_reverse中的路径是反着的,
 * 即子目录在前(左),父目录在后(右) ,
 * 现将full_path_reverse中的路径反置 */
   char* last_slash;	// 用于记录字符串中最后一个斜杠地址
   while ((last_slash = strrchr(full_path_reverse, '/'))) {
      uint16_t len = strlen(buf);
      strcpy(buf + len, last_slash);
      /* 在full_path_reverse中添加结束字符,做为下一次执行strcpy中last_slash的边界 */
      *last_slash = 0;
   }
   sys_free(io_buf);
   return buf;
}

/* 更改当前工作目录为绝对路径path,成功则返回0,失败返回-1 */
int32_t sys_chdir(const char* path) {
   int32_t ret = -1;
   struct path_search_record searched_record;  
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = search_file(path, &searched_record);
   if (inode_no != -1) {
      if (searched_record.file_type == FT_DIRECTORY) {
	 running_thread()->cwd_inode_nr = inode_no;
	 ret = 0;
      } else {
	 printk("sys_chdir: %s is regular file or other!\n", path);
      }
   }
   dir_close(searched_record.parent_dir); 
   return ret;
}

//在buf中填充文件结构相关信息 成功返回0 失败返回-1
int32_t sys_stat(const char* path,struct stat* buf)
{
    //如果是根目录
    if(!strcmp(path,"/") || !strcmp(path,"/.") || !strcmp(path,"/.."))
    {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_ino = 0;
        buf->st_size = root_dir.inode->i_size;
    }
    
    int32_t ret = -1;
    struct path_search_record searched_record;
    memset(&searched_record,0,sizeof(struct path_search_record));
    
    int inode_no = search_file(path,&searched_record);
    if(inode_no != -1)
    {
        struct inode* obj_inode = inode_open(cur_part,inode_no);
        buf->st_size = obj_inode->i_size;
        buf->st_filetype = searched_record.file_type;
        buf->st_ino = inode_no;
        inode_close(obj_inode);
        ret = 0;
    }
    else
        printk("sys_stat: %s not found\n",path);
    
    dir_close(searched_record.parent_dir);
    return ret;
}


//文件系统初始化 磁盘上搜索 如果没有则格式化分区 并创建文件系统
void filesys_init(void)
{
    uint8_t channel_no = 0,dev_no,part_idx = 0;
    struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);
    
    if(sb_buf == NULL)	PANIC("alloc memory failed!");
    printk("searching filesysteam......\n");
    while(channel_no < channel_cnt)
    {
    	dev_no = 1;
    	while(dev_no < 2)
    	{
    	    if(!dev_no)	//跳过hd60M.img主盘
    	    {
    	    	++dev_no;	
    	    	continue;
    	    }
    	    struct disk* hd = &channels[0].devices[1];		//得到硬盘指针
    	    struct partition* part = hd->prim_parts;				//先为主区创建文件系统
    	    while(part_idx < 12)		//4个主区 + 8个逻辑分区
    	    {
    	    	if(part_idx == 4)
    	    	    part = hd->logic_parts;	
    	    	if(part->sec_cnt != 0)		//分区存在 如果没有初始化 即所有成员都为0
    	    	{
    	    	    memset(sb_buf,0,SECTOR_SIZE);
    	    	    ide_read(hd,part->start_lba +1,sb_buf,1);	//读取超级块的扇区
    	    	    
    	    	    if(sb_buf->magic != 0x23333333)			//还没有创建文件系统
    	    	    {
    	    	    	printk("formatting %s's partition %s......\n",\
    	    	    	hd->name,part->name);
    	    	    	partition_format(hd,part);
    	    	    }
    	    	    else
    	    	    	printk("%s has filesystem\n",part->name);
    	    	}
    	    	++part_idx;
    	    	++part;	//到下一个分区看
    	    }
    	    ++dev_no;		//切换盘号
    	}
    	++channel_no;		//增加ide通道号
    }
    sys_free(sb_buf);
    char default_part[8] = "sdb1";	//参数为int 4字节字符串指针传的进去
    list_traversal(&partition_list,mount_partition,(int)default_part);

    open_root_dir(cur_part);
    
    uint32_t fd_idx = 0;
    while(fd_idx < MAX_FILE_OPEN)
    	file_table[fd_idx++].fd_inode = NULL;
}