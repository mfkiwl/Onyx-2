/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/

#ifndef _IRQ_H
#define _IRQ_H

#include <stdbool.h>
#include <stdint.h>

#include <onyx/registers.h>

#ifdef __x86_64__
#include <onyx/apic.h>
#include <onyx/x86/irq.h>
#endif


#define IRQ_HANDLED	0
#define IRQ_UNHANDLED	-1

#define IRQ_FLAG_REGULAR	0

typedef int irqstatus_t;
typedef irqstatus_t (*irq_t)(struct irq_context *context, void *cookie);

struct interrupt_handler
{
	irq_t handler;
	struct device *device;
	void *cookie;
	unsigned long handled_irqs;
	unsigned int flags;
	struct interrupt_handler *next;
};

struct irqstats
{
	unsigned long handled_irqs;
	unsigned long spurious;
};

struct irq_line
{
	struct interrupt_handler *irq_handlers;
	/* Here to stop race conditions with uninstalling and installing irq handlers */
	struct spinlock list_lock;
	struct irqstats stats;
};

#ifdef __cplusplus
extern "C" {
#endif

bool is_in_interrupt(void);
void dispatch_irq(unsigned int irq, struct irq_context *context);
int install_irq(unsigned int irq, irq_t handler, struct device *device,
	unsigned int flags, void *cookie);
void free_irq(unsigned int irq, struct device *device);
void irq_init(void);

#ifdef __cplusplus
}
#endif
#endif
