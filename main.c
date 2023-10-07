#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"
#include "../thread/thread.h"
#include "interrupt.h"
#include "../device/console.h"
#include "../device/ioqueue.h"
#include "../device/keyboard.h"
#include "../userprog/process.h"
#include "syscall.h"
#include "../userprog/syscall-init.h"
#include "../lib/stdio.h"
#include "../lib/kernel/stdio-kernel.h"
#include "../fs/fs.h"
#include "../fs/file.h"
#include "../shell/shell.h"

void k_thread_a(void*);
void k_thread_b(void*);
void u_prog_a(void);
void u_prog_b(void);
int test_var_a = 0, test_var_b = 0;
int prog_a_pid = 0, prog_b_pid = 0;
void init(void);

int main(void){
    put_str("I am kernel\n");
    init_all();
    intr_enable();
/*************    写入应用程序    *************/
//   uint32_t file_size = 4777;
//   uint32_t sec_cnt = DIV_ROUND_UP(file_size, 512);
//   struct disk* sda = &channels[0].devices[0];
//   void* prog_buf = sys_malloc(file_size);
//   ide_read(sda, 300, prog_buf, sec_cnt);
//   int32_t fd = sys_open("/prog_no_arg", O_CREAT|O_RDWR);
//   if (fd != -1) {
//      if(sys_write(fd, prog_buf, file_size) == -1) {
//	 printk("file write error!\n");
//	 while(1);
//      }
//   }
/*************    写入应用程序结束   *************/
    cls_screen();
    console_put_str("[rabbit@localhost /]$ ");

    while(1);
    return 0;
}

void init(void)
{

    uint32_t ret_pid = fork();
    if(ret_pid)
        while(1);
    else
        my_shell();
    PANIC("init: should not be here");
}

// /* 在线程中运行的函数 */
// void k_thread_a(void* arg){

//     void* addr1 = sys_malloc(256);
//     void* addr2 = sys_malloc(255);
//     void* addr3 = sys_malloc(254);
//     console_put_str(" thread_a malloc addr:0x");
//     console_put_int((int)addr1);
//     console_put_char(',');
//     console_put_int((int)addr2);
//     console_put_char(',');
//     console_put_int((int)addr3);
//     console_put_char('\n');

//     int cpu_delay = 100000;
//     while (cpu_delay-- > 0);
//     sys_free(addr1);
//     sys_free(addr2);
//     sys_free(addr3);
//     while(1);  
// }

// void k_thread_b(void* arg){

//     void* addr1 = sys_malloc(256);
//     void* addr2 = sys_malloc(255);
//     void* addr3 = sys_malloc(254);
//     console_put_str(" thread_b malloc addr:0x");
//     console_put_int((int)addr1);
//     console_put_char(',');
//     console_put_int((int)addr2);
//     console_put_char(',');
//     console_put_int((int)addr3);
//     console_put_char('\n');
//     int cpu_delay = 100000;
//     while (cpu_delay-- > 0);
//     sys_free(addr1);
//     sys_free(addr2);
//     sys_free(addr3);
//     while(1);
// }

// /* 测试用户进程 */
// void u_prog_a(void){

//     void* addr1 = malloc(256);
//     void* addr2 = malloc(255);
//     void* addr3 = malloc(254);
//     printf(" prog_a malloc addr:0x%x,0x%x,0x%x\n",(int)addr1,(int)addr2,(int)addr3);

//     int cpu_delay = 100000;
//     while(cpu_delay-- > 0);
//     free(addr1);
//     free(addr2);
//     free(addr3);
//     while (1);
// }
// void u_prog_b(void){

//     void* addr1 = malloc(256);
//     void* addr2 = malloc(255);
//     void* addr3 = malloc(254);
//     printf(" prog_b malloc addr:0x%x,0x%x,0x%x\n",(int)addr1,(int)addr2,(int)addr3);

//     int cpu_delay = 100000;
//     while(cpu_delay-- > 0);
//     free(addr1);
//     free(addr2);
//     free(addr3);
//     while (1);
// }
