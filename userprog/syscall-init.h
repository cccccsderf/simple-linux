#ifndef __USERPROG_SYSCALL_INIT_H
#define __USERPROG_SYSCALL_INIT_H
#include "stdint.h"

uint32_t sys_getpid(void);
// void sys_write(char* str);
void syscall_init(void);

#endif