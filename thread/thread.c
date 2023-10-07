#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "debug.h"
#include "interrupt.h"
#include "print.h"
#include "../userprog/process.h"
#include "sync.h"
#include "../fs/file.h"

#define PG_SIZE 4096

struct task_struct* idle_thread;    // idle线程
struct task_struct* main_thread; // 主线程pcb
struct list thread_ready_list;   //就绪队列
struct list thread_all_list;    //所有任务队列
static struct list_elem* thread_tag;    //同于保存队列中的线程结点
struct lock pid_lock;

extern void switch_to(struct task_struct* cur, struct task_struct* next);
extern void init(void);

/* 系统空闲时运行的线程 void* arg UNUSED */ 
static void idle(void* arg UNUSED){
    while (1){
        thread_block(TASK_BLOCKED);
        //asm volatile("sti; hlt": : : "memory");
        asm volatile ("sti;hlt" : : :"memory");
    }
}

/* 获取当前线程pcb指针 */
struct task_struct* running_thread(){
    uint32_t esp;
    asm ("mov %%esp, %0" : "=g" (esp));
    /* 取esp整数部分，即pcb起始地址 */
    return (struct task_struct*)(esp & 0xfffff000);
}

/**  由kernel_thread去执行function(func_arg) ***/
static void kernel_thread(thread_func* function, void* func_arg){
    /* 执行function前要开中断，避免后面的时钟中断被屏蔽，而无法调度其他线程 */
    intr_enable();
    function(func_arg);
}

/* 分配pid */
static pid_t allocate_pid(void){
    static pid_t next_pid = 0;
    lock_acquire(&pid_lock);
    next_pid++;
    lock_release(&pid_lock);
    return next_pid;
}


/* fork进程时为其分配pid,因为allocate_pid已经是静态的,别的文件无法调用.
不想改变函数定义了,故定义fork_pid函数来封装一下。*/
pid_t fork_pid(void) {
   return allocate_pid();
}

/* 初始化线程栈thread_stack，将待执行的函数和参数放到thread_stack中相应的位置 */
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg){
    /* 先预留中断使用栈的空间，可见thread.h中定义的结构 */
    pthread->self_kstack -= sizeof(struct intr_stack);

    /* 再留出线程栈空间，可见thread.h中定义 */
    pthread->self_kstack -= sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0;
}

/*  初始化线程基本信息  */
void init_thread(struct task_struct* pthread, char* name, int prio){
    memset(pthread, 0, sizeof(*pthread));
    pthread->pid = allocate_pid();
    strcpy(pthread->name, name);
    if (pthread == main_thread)
    {
        pthread->status = TASK_RUNNING;
    }else{
        pthread->status = TASK_READY;
    }
    // pthread->status = TASK_RUNNING;
    /*  self_kstack 是线程自己再内核态下使用的栈顶地址 */
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
    pthread->priority = prio;
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;
    pthread->fd_table[0] = 0;
    pthread->fd_table[1] = 0;
    pthread->fd_table[2] = 0;
    uint8_t fd_idx = 3;
    while (fd_idx < MAX_FILES_OPEN_PER_PROC)
    {
        pthread->fd_table[fd_idx] = -1;
        fd_idx++;
    }
    pthread->cwd_inode_nr = 0;
    pthread->parent_pid = -1;
    pthread->stack_magic = 0x19870916; //自定义的魔数
}

/* 创建一优先级为prio的线程，线程名为name，线程所执行的函数是function(func_arg) */
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg){
    /* pcb都位于内核空间，包括用户进程的pcb也是再内核空间 */
    struct task_struct* thread = get_kernel_pages(1);

    init_thread(thread, name, prio);
    thread_create(thread, function, func_arg);

    /* 确保之前不在队列中 */
    ASSERT(!elem_find(&thread_ready_list,&thread->general_tag));
    /* 加入就绪线程队列中 */
    list_append(&thread_ready_list, &thread->general_tag);

    /* 确保之前不在队列中 */
    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    /* 加入全部线程队列 */
    list_append(&thread_all_list, &thread->all_list_tag);

    return thread;
    // asm volatile ("movl %0, %%esp; pop %%ebp; pop %%ebx; pop %%edi; pop %%esi; ret": : "g"(thread->self_kstack) : "memory");
    // return thread;
}

/* 将kernel中的main函数完善为主线程 */
static void make_main_thread(void){
    /* 因为main线程早已运行
     * 咱们再loader.S中进入内核时是mov esp, 0xc009f000
     * 就是为其预留pcb的，因此pcb地址为0xc009e000
     * 不需要通过get_kernel_page另外分配一页
     */
    main_thread = running_thread();
    init_thread(main_thread,"main",31);

    /* main函数是当前线程，当前线程不在thread_ready_list中
     * 所以只将其加再thread_all_list中
     */
    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}
/* 实现任务调度 */
void schedule(){
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();

    if (cur->status == TASK_RUNNING){
        ASSERT(!elem_find(&thread_ready_list,&cur->general_tag));
        list_append(&thread_ready_list, &cur->general_tag);
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    }else{
        /* 若此线程需要某事件发生后才能继续上cpu运行，不需要将其加入队列，因为当前线程不在就绪队列中 */

    }
    // 如果就绪队列中没有可运行的任务，就唤醒idle
    if (list_empty(&thread_ready_list))
    {
       thread_unblock(idle_thread);
    }
    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL;
    thread_tag = list_pop(&thread_ready_list);
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
    next->status = TASK_RUNNING;

    /* 激活任务页等 */
    process_activate(next);

    switch_to(cur, next);

}

/* 当前线程将自己阻塞，标志位状态位stat */
void thread_block(enum task_status stat){
    /* stat取值为TASK_BLOCKED,TASK_WAITING,TASK_HANGING只有这三种状态菜不会被调度 */
    ASSERT((stat == TASK_BLOCKED) || (stat == TASK_WAITING) || (stat == TASK_HANGING));
    // 获取中断状态（关还是开）
    enum intr_status old_status = intr_disable();
    struct task_struct* cur_thread = running_thread();
    // 将其状态设为阻塞
    cur_thread->status = stat;
    // 任务切换
    schedule();
    intr_set_status(old_status);
}
/* 将线程解除阻塞 */
void thread_unblock(struct task_struct* pthread){
    // 获取中断状态（开还是关）
    enum intr_status old_status = intr_disable();
    // 验证线程状态
    ASSERT((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TASK_HANGING));
    if (pthread->status != TASK_READY)
    {
        // 严重线程是否在就绪队列
        ASSERT(!elem_find(&thread_ready_list,&pthread->general_tag));
        if (elem_find(&thread_ready_list,&pthread->general_tag))
        {
            PANIC("thread_unblock:blocked thread in ready_list\n");
        }
        list_push(&thread_ready_list,&pthread->general_tag);
        pthread->status = TASK_READY;
    }
    intr_set_status(old_status);  
}
// 主动让出cpu，换其他线程运行
void thread_yield(void){
    struct task_struct* cur = running_thread();
    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
    list_append(&thread_ready_list, &cur->general_tag);
    cur->status = TASK_READY;
    schedule();
    intr_set_status(old_status);
}

static void pad_print(char* buf,int32_t buf_len,void* ptr,char format)
{
    memset(buf,0,buf_len);
    uint8_t out_pad_0idx = 0;
    switch(format)
    {
        case 's':
            out_pad_0idx = springtf(buf,"%s",ptr);
            break;
        case 'd':
            out_pad_0idx = springtf(buf,"%d",*((int16_t*)ptr));
            break;
        case 'x':
            out_pad_0idx = springtf(buf,"%x",*((uint32_t*)ptr));   
    }
    while(out_pad_0idx < buf_len)
    {
        buf[out_pad_0idx] = ' ';
        out_pad_0idx++;
    }
    sys_write(stdout_no,buf,buf_len-1);
}

static bool elem2thread_info(struct list_elem* pelem,int arg UNUSED)
{
    struct task_struct* pthread = elem2entry(struct task_struct,all_list_tag,pelem);
    char out_pad[16] = {0};
    pad_print(out_pad,16,&pthread->pid,'d');
    
    if(pthread->parent_pid == -1)
    	pad_print(out_pad,16,"NULL",'s');
    else
        pad_print(out_pad,16,&pthread->parent_pid,'d');
        
    switch(pthread->status)
    {
        case 0:
            pad_print(out_pad,16,"RUNNING",'s');
            break;
        case 1:
            pad_print(out_pad,16,"READY",'s');
            break;
        case 2:
            pad_print(out_pad,16,"BLOCKED",'s');
            break;
        case 3:
            pad_print(out_pad,16,"WAITING",'s');
            break;
        case 4:
            pad_print(out_pad,16,"HANGING",'s');
            break;
        case 5:
            pad_print(out_pad,16,"DIED",'s');
            break;
    }
    pad_print(out_pad,16,&pthread->elapsed_ticks,'x');
    
    memset(out_pad,0,16);
    ASSERT(strlen(pthread->name) < 17);
    memcpy(out_pad,pthread->name,strlen(pthread->name));
    strcat(out_pad,"\n");
    sys_write(stdout_no,out_pad,strlen(out_pad));
    return false;
}
//打印任务列表
void sys_ps(void)
{
    char* ps_title = "PID             PPID            STAT             TICKS            COMMAND\n";
    sys_write(stdout_no,ps_title,strlen(ps_title));
    list_traversal(&thread_all_list,elem2thread_info,0);
}

void thread_init(void){
    put_str("thread_init start\n");
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    lock_init(&pid_lock);
    process_execute(init, "init");
    make_main_thread();
    


    // 创建idle线程
    idle_thread = thread_start("idle", 10, idle, NULL);

    put_str("thread_init done\n");
}