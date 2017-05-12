/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _TSS_H
#define _TSS_H
#include <stdint.h>

typedef struct tss_entry
{
	uint32_t reserved0;
	uint64_t stack0; /* This is not naturally aligned, so packed is needed. */
	uint64_t stack1;
	uint64_t stack2;
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

void init_tss();
void set_kernel_stack(uintptr_t stack0);
#endif
