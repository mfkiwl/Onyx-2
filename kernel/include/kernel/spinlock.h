/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

typedef struct spinlock
{
	unsigned long lock;
	unsigned long waiters;
} spinlock_t;

extern void acquire_spinlock(spinlock_t*);
extern void release_spinlock(spinlock_t*);
void wait_spinlock(spinlock_t*);
#endif
