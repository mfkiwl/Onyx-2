/*
 * Copyright (c) 2016 - 2023 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _ONYX_SPINLOCK_H
#define _ONYX_SPINLOCK_H

#ifndef __cplusplus
#define __ONLY_INCLUDE_BASIC_C_DEFS
#endif

#ifndef __ONLY_INCLUDE_BASIC_C_DEFS
#include <assert.h>
#include <stdbool.h>

#include <onyx/compiler.h>
#include <onyx/preempt.h>
#include <onyx/smp.h>
#include <onyx/utils.h>

#include <platform/irq.h>

#endif

// #include <onyx/lock_annotations.h>
#define __ACQUIRE(...)
#define __RELEASE(...)
#define __CAPABILITY(...)

typedef unsigned int raw_spinlock_t;

struct __CAPABILITY("spinlock") spinlock
{
    /* TODO: Conditionally have these debug features, and have owner_cpu be in lock */
    raw_spinlock_t lock;
#ifdef CONFIG_SPINLOCK_DEBUG
    unsigned long holder;
#endif
};

#ifdef __cplusplus
extern "C"
{
#endif

void __spin_lock(struct spinlock *lock) __ACQUIRE(lock);
void __spin_unlock(struct spinlock *lock) __RELEASE(lock);
int spin_try_lock(struct spinlock *lock);

#ifndef __ONLY_INCLUDE_BASIC_C_DEFS
#ifdef __cplusplus
}
#endif

CONSTEXPR static inline void spinlock_init(struct spinlock *s)
{

#ifdef CONFIG_SPINLOCK_DEBUG
    s->holder = 0xDEADCAFEDEADCAFE;
#endif

    s->lock = 0;
}

#define STATIC_SPINLOCK_INIT \
    {                        \
    }

static inline FUNC_NO_DISCARD unsigned long spin_lock_irqsave(struct spinlock *lock) __ACQUIRE(lock)
{
    unsigned long flags = irq_save_and_disable();
    __spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(struct spinlock *lock, unsigned long old_flags)
    __RELEASE(lock)
{
    __spin_unlock(lock);
    irq_restore(old_flags);
}

static inline bool spin_lock_held(struct spinlock *lock)
{
    return lock->lock == get_cpu_nr() + 1;
}

static inline void spin_lock(struct spinlock *lock) __ACQUIRE(lock)
{
    sched_disable_preempt();

    __spin_lock(lock);
}

static inline void spin_unlock(struct spinlock *lock) __RELEASE(lock)
{
    __spin_unlock(lock);
    sched_enable_preempt();
}

#define MUST_HOLD_LOCK(lock) assert(spin_lock_held(lock) != false)

#ifdef __cplusplus

class Spinlock
{
private:
    struct spinlock lock;
    unsigned long cpu_flags;

public:
    constexpr Spinlock() : lock{}, cpu_flags{} {};
    ~Spinlock()
    {
        assert(lock.lock != 1);
    }
    void Lock() __ACQUIRE(lock)
    {
        spin_lock(&lock);
    }

    void LockIrqsave() __ACQUIRE(lock)
    {
        cpu_flags = spin_lock_irqsave(&lock);
    }

    void Unlock() __RELEASE(lock)
    {
        spin_unlock(&lock);
    }

    void UnlockIrqrestore() __RELEASE(lock)
    {
        spin_unlock_irqrestore(&lock, cpu_flags);
    }

    bool IsLocked()
    {
        return lock.lock == 1;
    }
};

#endif
#endif
#endif
