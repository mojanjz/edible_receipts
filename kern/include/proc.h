/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */
#define	AVAILABLE	0 /* PID available */
#define	OCCUPIED	1 /* PID is in use by a running process */
#define ORPHAN		2 /* PID of a running process with no parent */
#define ZOMBIE		3 /* PID of a process that has finished and is waiting to be cleaned up */


#include <spinlock.h>
#include <thread.h> /* required for struct threadarray */
#include <filetable.h>
#include <types.h>

struct addrspace;
struct vnode;

/*
 * Process structure.
 */
struct proc {
	char *p_name;			/* Name of this process */
	struct spinlock p_lock;		/* Lock for this structure */
	struct threadarray p_threads;	/* Threads in this process */
	pid_t p_pid;	/* The process' PID */

	struct array *p_children;	/* An array of the process' children's PIDs */

	/* VM */
	struct addrspace *p_addrspace;	/* virtual address space */

	/* VFS */
	struct vnode *p_cwd;		/* current working directory */

	/* Filetable */
	struct filetable *p_filetable;
};

/* 
 * PID table structure.
 */
struct pid_table{
	struct lock *pid_table_lk; /* lock to synchronize children table */
	/* Note: Index number in the arrays represents the corresponding PID of entry*/
	/* Array of process statuses, as defined above */
	int process_statuses[__PID_MAX];
	/* Array of processes */
	struct proc *processes[__PID_MAX];
	/* Array of process exit codes */
	int process_exitcodes[__PID_MAX];
	/* CV used to implement wait cv */
	struct cv *pid_table_cv;

};

/* This is the globally accessible pid_table */
extern struct pid_table *pid_table;

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Create a new forked child process */
struct proc *proc_create_fork(const char *name, int *err);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);

pid_t issue_pid(void);
void configure_pid_fields(struct proc *child_proc);
void init_pid_table(void);
void delete_pid_entry(pid_t pid);
#endif /* _PROC_H_ */
