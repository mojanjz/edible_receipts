/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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

#include <types.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <kern/errno.h>
#include <kern/limits.h>
#include <kern/fcntl.h>
#include <filetable.h>
#include <lib.h>
#include <uio.h>
#include <current.h>
#include <vnode.h>
#include <kern/seek.h>
#include <stat.h>
#include <file_entry.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <limits.h>
#include <addrspace.h>
#include <mips/trapframe.h>

int copy_in_args(userptr_t args, char **kargs);

int
sys_fork(struct trapframe *tf, pid_t *retval)
{
    int err = 0;
    // size_t child_name_size = 100;
    (void) retval;
    (void) tf;
    struct proc *child_proc;
    pid_t child_pid;

    // /* Get the next available PID for the parent */
    // child_pid = issue_pid(); // TODO change such that pid issued when proc created not here
    // /* Create child process */
    // char child_name[child_name_size];
    // snprintf(child_name, child_name_size, "%s-pid:%d", curproc->p_name, (int)child_pid);
    // child_proc = proc_create_fork(child_name);

    child_proc = proc_create_fork("child-process"); //TODO: confirm process name doesnt have to be unique
    if(child_proc == NULL){
        return ENOMEM;
    }
    child_pid = child_proc->p_pid;

    /* Copy the parent filetable */
    filetable_copy(child_proc->p_filetable, curproc->p_filetable);
    kprintf("parent first filename %d, child first filename %d\n", curproc->p_filetable->ft_file_entries[0]->fe_status, child_proc->p_filetable->ft_file_entries[0]->fe_status);

    /* Copy the parent's trapframe */
    struct trapframe *child_tf = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if(child_tf == NULL){
        return ENOMEM;
    }
    memcpy((void *)child_tf, (const void *)tf, sizeof(struct trapframe));
    /* Update v0, v1, a3, prog counter for child */
    child_tf->tf_v0 = 0;
    child_tf->tf_v1 = 0;
    child_tf->tf_a3 = 0;
    child_tf->tf_epc = child_tf->tf_epc + 4;

    /* Make kernel thread for child */
    // char child_thread_name[12];
    // snprintf(child_thread_name, 12, "%d-thread", (int)child_pid);
    
    // err = thread_fork(child_thread_name, child_proc, enter_new_forked_process, child_tf, 0);
    err = thread_fork("child-thread", child_proc, enter_new_forked_process, child_tf, 0);

    if(err){
        kfree(child_tf);
        proc_destroy(child_proc);
        return err;
    }
    /* Update parent's retval */
    tf->tf_v0 = child_pid;

    return err; 
}

void
enter_new_forked_process(void *data1, unsigned long data2){
    (void)data2;

    void * tf = (void *) curthread->t_stack + 16; //TODO: BRUH WHAT!
    memcpy(tf, (const void *)data1, sizeof(struct trapframe));
    kfree((struct trapframe *)data1);

    // kprintf("In enter new forked process");
    // struct trapframe *tf = data1;



    as_activate();
    kprintf("Current process is: %s\n", curproc->p_name);
    mips_usermode(tf);
}

pid_t
sys_getpid(){
    // kprintf("In sys_getpid, returning !");
    return curproc->p_pid;
}

int 
sys_execv(userptr_t program, userptr_t args)
{
    int err = 0;
    (void)args;
    /* Copy program name in*/
    char *kernel_progname;

    kernel_progname = (char *)kmalloc(__PATH_MAX);
    if (kernel_progname == NULL) {
        return ENOMEM;
    }

    err = copyinstr(program, kernel_progname, __PATH_MAX, NULL);
    if (err) {
        kfree(kernel_progname);
        return err;
    }

    /* copy arguements in */
    char **kargs;

    kargs = kmalloc(__ARG_MAX*(sizeof(char *))); // TODO: can get the actual number of arguements instead of using ARGMAX
    err = copy_in_args(args, kargs);
    

    
    /* Load the executable and run it */
    /* Copy arguments from kernel to user stack */
    /* Return to user mode */
    return -1; // should never get here
}

int
copy_in_args(userptr_t args, char **kargs)
{
    (void)args;
    (void)kargs;

    int i = 0;
    do {

    } while (kargs[i]!= NULL);
    return 0;
}