#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"
#include "../thread/thread.h"
#include "../device/console.h"
#include "../device/keyboard.h"
#include "../userprog/tss.h"
#include "../userprog/syscall-init.h"
#include "../device/ide.h"
#include "../fs/fs.h"

void init_all(){
    put_str("init_all\n");
    idt_init(); // 初始化中断
    mem_init(); // 初始化内存管理系统
    thread_init(); // 初始化线程相关结构
    timer_init(); // 初始化PIT
    console_init(); // 控制台初始化最好放在开中断之前
    keyboard_init(); // 键盘
    tss_init();
    syscall_init();
    ide_init();
    filesys_init();
}