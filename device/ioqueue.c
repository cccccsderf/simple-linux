#include "ioqueue.h"
#include "interrupt.h"
#include "global.h"
#include "debug.h"
#include "../lib/kernel/print.h"
#include "print.h"

/* 初始化io队列 */
void ioqueue_init(struct ioqueue* ioq){
    lock_init(&ioq->lock);
    ioq->producer = ioq->consumer = NULL;
    ioq->head = ioq->tail = 0;
}

/* 返回pos在缓冲区中的下一个位置值 */
static int32_t next_pos(int32_t pos){
    return (pos + 1) % bufsize;
}

/* 判断队列是否已满 */
bool ioq_full(struct ioqueue* ioq){
    ASSERT(intr_get_status() == INTR_OFF);
    return next_pos(ioq->head) == ioq->tail;
}

/* 判断队列是否为空 */
bool ioq_empty(struct ioqueue* ioq){
    ASSERT(intr_get_status() == INTR_OFF);
    return ioq->head == ioq->tail;
}

/* 使当前生产者和消费者在此缓冲区上等待 */
static void ioq_wait(struct task_struct** waiter){
    ASSERT(*waiter == NULL && waiter != NULL);
    *waiter = running_thread();
    thread_block(TASK_BLOCKED);
}

/* 唤醒waiter */
static void wakeup(struct task_struct** waiter){
    ASSERT(*waiter != NULL);
    thread_unblock(*waiter);
    *waiter = NULL;
}

/* 消费者从ioq队列中获取一个字符 */
char ioq_getchar(struct ioqueue* ioq){
    ASSERT(intr_get_status() == INTR_OFF);
    while (ioq_empty(ioq))
    {
        lock_acquire(&ioq->lock);
        ioq_wait(&ioq->consumer);
        lock_release(&ioq->lock);
    }
    char byte = ioq->buf[ioq->tail];    // 从缓冲区中取出
    ioq->tail = next_pos(ioq->tail);    // 游标移动到下一个位置

    if (ioq->producer != NULL)
    {
        wakeup(&ioq->producer);
    }
    return byte;
}

/* 生产者往ioq队列中写入一个字符byte */
void ioq_putchar(struct ioqueue* ioq, char byte){
    ASSERT(intr_get_status() == INTR_OFF);
    while (ioq_full(ioq))
    {
        lock_acquire(&ioq->lock);
        ioq_wait(&ioq->producer);
        lock_release(&ioq->lock);
    }
    ioq->buf[ioq->head] = byte;
    ioq->head = next_pos(ioq->head);

    if (ioq->consumer != NULL)
    {
        wakeup(&ioq->consumer);
    }
    
    
}