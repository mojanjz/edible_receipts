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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <limits.h>
#include <filetable.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 *The global table for all processes and their PIDs.
 */
struct pid_table *pid_table;

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}
	proc->p_filetable = filetable_init();
	if (proc->p_filetable == NULL) {
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}
	proc->p_children = array_create();
	if (proc->p_children == NULL) {
		kfree(proc->p_filetable);
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	/* PID Initialization */
	proc->p_pid = 1; /* Kernel thread has special pid value 1, set this as default pid value */

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}
	/* Process PID fields */
	int p_children_size = array_num(proc->p_children);
	for (int i = 0; i < p_children_size; i++){
		array_remove(proc->p_children, 0); /* Empty the array, one element at a time */
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);

	}
	filetable_destroy(proc->p_filetable);
	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	array_destroy(proc->p_children);

	kfree(proc->p_name);
	// kfree(proc->p_addrspace);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* Initialize STDIN, STDOUT, STDERR */
	int err = filetable_init_std(newproc->p_filetable);
	if (err) {
		kfree(newproc);
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	/* PID Fields */
	configure_pid_fields(newproc);

	return newproc;
}

struct proc * 
proc_create_fork(const char *name){
	struct proc *child_proc;
	int err = 0;
	
	child_proc = proc_create(name);
	if(child_proc == NULL){
		return NULL;//TODO: improve error handling
	}
	
	/* Copy the address space of the parent */
    struct addrspace *child_as = (struct addrspace *)kmalloc(sizeof(struct addrspace));
    if (child_as == NULL) {
		kprintf("Error in proc_create_fork");
        return NULL;//TODO: improve error handling
    }
    err = as_copy(curproc->p_addrspace, &child_as);
    if(err) {
		kprintf("Error in proc_create_fork");
        return NULL; //TODO: improve error handling
    }
	child_proc->p_addrspace = child_as;
    // proc_setas(child_as);

	/* PID Fields */
	configure_pid_fields(child_proc);
	// kfree(child_as);

	return child_proc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	spl = splhigh();
	t->t_proc = proc;
	splx(spl);
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			spl = splhigh();
			t->t_proc = NULL;
			splx(spl);
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}


/* Functions related to PID management */

pid_t
issue_pid()
{
	pid_t new_pid = 0; /* If new_pid isn't assigned, return zero to signify error */
	
	//TODO: Test synchronization of this method!
	lock_acquire(pid_table->pid_table_lk); /* Lock whole PID table so that statuses dont change during check */
	
	for (int i = __PID_MIN; i < __PID_MAX; i++){ /* Make sure not to assign special PIDs */
		if (pid_table->process_statuses[i] == AVAILABLE){
			new_pid = i;
			pid_table->process_statuses[i] = OCCUPIED;
			break;
		}
	}

	lock_release(pid_table->pid_table_lk);
	
	/* Check that PID was correctly assigned */
	if(new_pid == 0){
		//THERE ARE NO AVAILABLE PIDs, HANDLE ERROR HERE! ENPROC
		kprintf("There are no available PIDS");
		panic("There are no available PIDs");
	}
	
	
	return new_pid;
}

void
configure_pid_fields(struct proc *child_proc)
{
	//TODO: test synchronization, change name to add pid entry
	child_proc->p_pid = issue_pid(); //Already locked
	child_proc->p_ppid = curproc->p_pid;
	spinlock_acquire(&curproc->p_lock);
	array_add(curproc->p_children, (void *)child_proc->p_pid, NULL);
	pid_table->processes[child_proc->p_pid] = child_proc;
	spinlock_release(&curproc->p_lock);
}

/* Deletes an entry in the PID table */
void
delete_pid_entry(pid_t pid)
{
	pid_table->process_statuses[pid] = AVAILABLE;
	pid_table->processes[pid] = NULL;
	pid_table->process_exitcodes[pid] = (int) NULL;
}

void 
init_pid_table()
{
	pid_table = kmalloc(sizeof(struct pid_table));
	if (pid_table == NULL){
		//TODO: check that panic is the right way to handle 
		panic("Error trying to initialize pid table.\n");
	}

	pid_table->pid_table_lk = lock_create("PID-table-lock");
	if (pid_table->pid_table_lk == NULL){
		panic("Error trying to make pid table lock.\n");
	}

	pid_table->pid_table_cv = cv_create("PID-table-cv");
	if (pid_table->pid_table_cv == NULL){
		panic("Error trying to make pid table condition variable.\n");
	}

	/* Assign special PID values to be occupied */
	pid_table->process_statuses[0] = OCCUPIED;
	pid_table->process_statuses[1] = OCCUPIED;
	
	/* Loop over PIDs and make them available */
	for (int i = __PID_MIN; i < __PID_MAX; i++){ /* Make sure not to assign special PIDs */
		pid_table->process_statuses[i] = AVAILABLE;
	}
}
