#ifndef __DEVICE_IOQUEUE_H
#define __DEVICE_IOQUEUE_H
#include "stdint.h"
#include "../thread/thread.h"
#include "../thread/sync.h"

#define bufsize 64

/* 环形队列 */
struct ioqueue {
    // 生产者消费者问题
    struct lock lock;
    struct task_struct* producer;
    struct task_struct* consumer;
    char buf[bufsize];  // 缓冲区大小
    int32_t head;   // 队头 写入数据
    int32_t tail;   // 队尾 读取数据
};

void ioqueue_init(struct ioqueue* ioq);
bool ioq_full(struct ioqueue* ioq);
bool ioq_empty(struct ioqueue* ioq);
char ioq_getchar(struct ioqueue* ioq);
void ioq_putchar(struct ioqueue* ioq, char byte);

#endif