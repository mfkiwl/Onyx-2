/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _PMM_H
#define _PMM_H

#include <stdint.h>
#include <stddef.h>
/* block size (4KiB) */
#define PMM_BLOCK_SIZE	4096
typedef struct stack_entry
{
	uintptr_t base;
	size_t size;
	size_t magic;
}stack_entry_t;
typedef struct stack
{
	stack_entry_t* next;
}stack_t;

size_t pmm_get_used_mem();
void pmm_push(uintptr_t base,size_t size,size_t kernel_space_size);
void pmm_pop();
void pmm_init(size_t memory_size,uintptr_t stack_space);
void *bootmem_alloc(size_t blocks);
void  pfree(size_t blocks,void* ptr);

#endif
