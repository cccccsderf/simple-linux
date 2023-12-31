#include "tss.h"
#include "process.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "print.h"
#include "../thread/thread.h"
#include "../kernel/interrupt.h"
#include "debug.h"
#include "../device/console.h"
#include "../kernel/list.h"

extern void intr_exit(void);

/* 构建用户进程初始化上下文信息 */
void start_process(void* filename_){
    void* function = filename_;
    struct task_struct* cur = running_thread();
    cur->self_kstack += sizeof(struct thread_stack);
    struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;
    proc_stack->edi = proc_stack->esi = proc_stack->ebp = proc_stack->esp_dummy = 0;
    proc_stack->ebx = proc_stack->edx = proc_stack->ecx = proc_stack->eax = 0;
    proc_stack->gs = 0;
    proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA;
    proc_stack->eip = function;
    proc_stack->cs = SELECTOR_U_CODE;
    proc_stack->eflags = (EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1);
    proc_stack->esp = (void*)((uint32_t)get_a_page(PF_USER,USER_STACK3_VADDR) + PG_SIZE);
    proc_stack->ss = SELECTOR_U_DATA;
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (proc_stack) : "memory");
}

/* 激活页表 */
void page_dir_activate(struct task_struct* p_thread){
    /* 执行此函数时，当前任务可能是线程。
     * 之所以对线程也要重新安装页表，原因是上一次被调度的可能是进程
     * 否则不恢复页表的花，线程就会使用进程的页表了
     */
    /* 若为内核线程，需要重新填充页表为0x100000 */
    uint32_t pagedir_phy_addr = 0x100000;
    // 默认为内核的页目录物理地址，也就是内核线程所使用的页目录表
    if (p_thread->pgdir != NULL)
    {
        pagedir_phy_addr = addr_v2p((uint32_t)p_thread->pgdir);
    }
    /* 更新页目录寄存器cr3，使新页表生效 */
    asm volatile ("movl %0,%%cr3" : : "r" (pagedir_phy_addr) : "memory");
}

/* 激活线程或进程的页表，更新tss中的esp0为进程的特权级0的栈 */
void process_activate(struct task_struct* p_thread){
    ASSERT(p_thread != NULL);
    page_dir_activate(p_thread);

    if (p_thread->pgdir){
        updata_tss_esp(p_thread);
    }   

}

/* 创建页目录表，将当前页表的表示内核空间的pde复制
    成功则返回页目录的虚拟地址，否则返回-1
 */
uint32_t* create_page_dir(void){
    // 用户进程的页表不能让用户直接访问到，所以在内核空间来申请
    uint32_t* page_dir_vaddr = get_kernel_pages(1);
    if (page_dir_vaddr == NULL)
    {
        console_put_str("create_page_dir: get_kernel_page failed!");
        return NULL;
    }
    /* 先复制页表 */
    /* page_dir_vaddr + 0x300*4 是内核页目录的第768项 */
    memcpy((uint32_t*)((uint32_t)page_dir_vaddr + 0x300*4),(uint32_t*)(0xfffff000+0x300*4),1024);
    /* 更新页目录 */
    uint32_t new_page_dir_phy_vaddr = addr_v2p((uint32_t)page_dir_vaddr);
    page_dir_vaddr[1023] = new_page_dir_phy_vaddr | PG_US_U | PG_RW_W | PG_P_1;
    return page_dir_vaddr;
    
}

/* 创建用户进程虚拟地址位图 */
void create_user_vaddr_bitmap(struct task_struct* user_prog){
    user_prog->userprog_vaddr.vaddr_start = USER_VADDR_START;
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START)/PG_SIZE/8, PG_SIZE);
    user_prog->userprog_vaddr.vaddr_bitmap.bits = get_kernel_pages(bitmap_pg_cnt);
    user_prog->userprog_vaddr.vaddr_bitmap.btmp_byte_len = (0xc0000000 - USER_VADDR_START) / PG_SIZE / 8;
    bitmap_init(&user_prog->userprog_vaddr.vaddr_bitmap);
}

/* 创建用户进程 */
void process_execute(void* filename, char* name){
    struct task_struct* thread = get_kernel_pages(1);
    init_thread(thread, name, default_prio);
    create_user_vaddr_bitmap(thread);
    thread_create(thread, start_process, filename);
    thread->pgdir = create_page_dir();
    block_desc_init(thread->u_block_desc);

    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);
    intr_set_status(old_status);
}
