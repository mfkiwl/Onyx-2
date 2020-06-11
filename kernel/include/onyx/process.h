/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _ONYX_PROCESS_H
#define _ONYX_PROCESS_H

#include <sys/types.h>

#include <onyx/vm.h>
#include <onyx/mutex.h>
#include <onyx/ioctx.h>
#include <onyx/spinlock.h>
#include <onyx/signal.h>
#include <onyx/registers.h>
#include <onyx/list.h>
#include <onyx/scheduler.h>
#include <onyx/condvar.h>
#include <onyx/semaphore.h>
#include <onyx/elf.h>
#include <onyx/syscall.h>
#include <onyx/cred.h>
#include <onyx/itimer.h>

#include <onyx/vm_layout.h>

struct proc_event_sub;

struct process
{
	unsigned long refcount;

	/* The next process in the linked list */
	struct process *next;

	unsigned long nr_threads;
	
	struct list_head thread_list;
	struct spinlock thread_list_lock;

	struct mm_address_space address_space;
	/* Program name*/
	char *cmd_line;

	/* IO Context of the process */
	struct ioctx ctx;

	/* Process ID */
	pid_t pid;
	
	/* exit(2) specific flags */
	int has_exited;

	struct semaphore wait_sem;
	int exit_code;
	
	/* Process' UID and GID */
	struct creds cred;

	/* Pointer to the VDSO */
	void *vdso;

	/* Signal tables */
	struct spinlock signal_lock;
	struct k_sigaction sigtable[_NSIG];
	unsigned int signal_group_flags;

	/* Process personality */
	unsigned long personality;

	/* This process' parent */
	struct process *parent;
	
	/* Linked list to the processes being traced */
	struct extrusive_list_head tracees;

	/* User time and system time consumed by the process */
	clock_t user_time;
	clock_t system_time;

	/* proc_event queue */
	struct spinlock sub_queue_lock;
	struct proc_event_sub *sub_queue;
	unsigned long nr_subs;
	unsigned long nr_acks;

	void *interp_base;
	void *image_base;

	struct elf_info info;

	struct cond syscall_cond;
	struct mutex condvar_mutex;

	struct spinlock children_lock;
	struct process *children, *prev_sibbling, *next_sibbling;

	struct itimer timers[ITIMER_COUNT];
};

#ifdef __cplusplus
extern "C" {
#endif

struct process *process_create(const char *cmd_line, struct ioctx *ctx, struct process *parent);

struct thread *process_create_main_thread(struct process *proc, thread_callback_t callback, void *sp);

struct process *get_process_from_pid(pid_t pid);
struct thread *process_fork_thread(thread_t *src, struct process *dest, struct syscall_frame *ctx);
void process_destroy_aspace(void);
int process_attach(struct process *tracer, struct process *tracee);
struct process *process_find_tracee(struct process *tracer, pid_t pid);
void process_exit_from_signal(int signum);
char **process_copy_envarg(char **envarg, bool to_kernel, int *count);
void process_increment_stats(bool is_kernel);
void process_end(struct process *p);
void process_add_thread(struct process *process, thread_t *thread);

static inline void process_get(struct process *process)
{
	__atomic_add_fetch(&process->refcount, 1, __ATOMIC_ACQUIRE);
}

static inline void process_put(struct process *process)
{
	if(__atomic_sub_fetch(&process->refcount, 1, __ATOMIC_ACQUIRE) == 0)
		process_end(process);
}

struct stack_info
{
	void *base;
	void *top;
	size_t length;
};

int process_alloc_stack(struct stack_info *info);

void process_put_entry_info(struct stack_info *info, char **argc, char **envp);

#ifdef __cplusplus
}
#endif

static inline struct process *get_current_process()
{
	thread_t *thread = get_current_thread();
	return (thread == NULL) ? NULL : (struct process *) thread->owner;
}

static inline struct mm_address_space *get_current_address_space()
{
	struct process *proc = get_current_process();
	return proc ? &proc->address_space : NULL;
}

#endif
