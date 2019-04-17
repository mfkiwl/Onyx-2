/*
* Copyright (c) 2018 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/

#ifndef _IGPU_DRV_H
#define _IGPU_DRV_H

#include <stdbool.h>
#include <stdio.h>

#include <onyx/i2c.h>

#include <pci/pci.h>

#define MPRINTF(...)	printk("ihdgpu: " __VA_ARGS__)

struct igpu_driver_data
{
	/* Turns out, we need to know this, because gmch uses the normal
	 * gpio regs while PCH has an offset.
	 * Pretty sure everything we support is PCH only, but lets keep this
	 * around for older hardware support
	 * Note that we can only detect if we have a GMCH vs PCH with PCI ids
	*/
	bool has_gmch_display;
	/* TODO: Add more stuff */
};

struct igpu_gmbus
{
	uint32_t gmbus0;
	uint32_t gmbus1;
	uint32_t gmbus2;
	uint32_t gmbus3;
	uint32_t gmbus4;
	uint32_t gmbus5;
};

#define IGPU_NR_GMBUS		6
#define NR_DISPLAY_PORTS	4

struct igd_opregion;
struct vbt_header;

struct igd_displayport;

struct igpu_device
{
	volatile void *mmio_regs;
	volatile void *gpu_memory;
	struct pci_device *device;
	struct igpu_gmbus gmbus;
	uint32_t gpio_regs_off;
	struct i2c_adapter i2c_adapter;
	volatile struct igd_opregion *opregion;
	struct vbt_header *igd_vbt;
	struct igd_displayport *dports[NR_DISPLAY_PORTS];
};

struct igd_displayport
{
	const char *name;
	struct igpu_device *device;
	struct i2c_adapter ddaux;
	unsigned int index;
	uint32_t ctl_reg;
	uint32_t data_base_reg;
};


#define HAS_GMCH_DISPLAY(dev) (((struct igpu_driver_data *) dev->device->driver_data)->has_gmch_display)

uint32_t igpu_mmio_read(struct igpu_device *dev, uint32_t offset);
void igpu_mmio_write(struct igpu_device *dev, uint32_t offset, uint32_t data);

int igpu_i2c_init(struct igpu_device *dev);

int igpu_wait_bit(struct igpu_device *dev, uint32_t reg, uint32_t mask,
		  unsigned long timeout, bool clear);

int igd_init_displayport(struct igpu_device *dev);
int igd_enable_power(struct igpu_device *dev);

#endif