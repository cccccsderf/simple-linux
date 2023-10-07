#ifndef __LIB_STDIO_KERNEL_H
#define __LIB_STDIO_KERNEL_H

#include "stdint.h"
void printk(const char* format, ...);
void sys_putchar(const char chr);
#endif