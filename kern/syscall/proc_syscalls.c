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
#include <vfs.h>


int copy_in_args(char **args, char **kargs, int argc, int *size_arr);
int copy_out_args(char **kargs, userptr_t argv, vaddr_t *stackptr, int argc, int *size_arr);
int get_argc(char **args, int *argc);

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
    // kprintf("parent first filename %d, child first filename %d\n", curproc->p_filetable->ft_file_entries[0]->fe_status, child_proc->p_filetable->ft_file_entries[0]->fe_status);

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
    // V on the parent ready
    // P child ready 
    /* Update parent's retval */
    tf->tf_v0 = child_pid;

    return err; 
}

void
enter_new_forked_process(void *data1, unsigned long data2){
    (void)data2;

    struct trapframe *tf = curthread->t_stack+16; //trapframe should be on the stack
    memcpy(tf, (const void *)data1, sizeof(struct trapframe));
    kfree((struct trapframe *)data1);

    as_activate();
    // kprintf("Current process is: %s\n", curproc->p_name);
    mips_usermode(tf);
}

pid_t
sys_getpid(){
    // kprintf("In sys_getpid, returning !");
    return curproc->p_pid;
}

int 
sys_execv(userptr_t program, char **args)
{
    kprintf("in execv\n");
    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    int result;
    int argc = 0;

    /* Copy program name in*/
    char *kernel_progname;

    kernel_progname = (char *)kmalloc(__PATH_MAX);
    if (kernel_progname == NULL) {
        return ENOMEM;
    }

    result = copyinstr(program, kernel_progname, __PATH_MAX, NULL);
    if (result) {
        kfree(kernel_progname);
        return result;
    }

    kprintf("got the program name %s\n", kernel_progname);

    /* Get the size of arguments array */
    result = get_argc(args, &argc);
    if (result) {
        kfree(kernel_progname);
        return result;
    }
    kprintf("got the size of the arguments %d\n", argc);
    /* Copy in the arguments */
    char **kargs; 
    int *size_arr = kmalloc(argc*(sizeof(int))); // array to store size of all arguments
    kargs = kmalloc(argc*(sizeof(char *))); // TODO: can optimize by kmallocing each pointer
    result = copy_in_args(args, kargs, argc, size_arr);
    if (result) {
        goto fail;
    }

    /* Create a new address space */
    as = as_create();
    if (as == NULL) {
        result = ENOMEM;
        goto fail;
    }
    
    /* Open the file */
    result = vfs_open(kernel_progname, O_RDONLY, 0, &v);
    if (result) {
        vfs_close(v);
        goto fail;
    }

    /* Switch to it and activate it. */
    struct addrspace *old_as = proc_setas(as); //TODO: restore if failure happens
    (void) old_as;
    as_activate();

    /* Load the executable and run it */
    result = load_elf(v, &entrypoint);
    (void)entrypoint;
    if (result) {
        vfs_close(v);
        goto fail;
    }

    vfs_close(v);

    /* Define the user stack in the address space */
    kprintf("stack ptr address before as_define_stack is %p\n", &stackptr);
    result = as_define_stack(as, &stackptr);
    kprintf("stackptr address after as_define_stack is %p\n", &stackptr);
    if (result) {
        goto fail;
    }

    /* Copy arguments from kernel to user stack */
    userptr_t argv_addr=(userptr_t) stackptr;
    result = copy_out_args(kargs, argv_addr, &stackptr, argc, size_arr);
    if (result) {
        goto fail;
    }

    /* Clean up the old as */
    /* Return to user mode */
    enter_new_process(argc, (userptr_t)stackptr, NULL, stackptr, entrypoint);
    /* enter process does not return. */
    panic("enter_new_process returned \n");
    return EINVAL; // should never get here

fail:
    kfree(kernel_progname);
    kfree(kargs);
    kfree(size_arr);
    return result;
}

int
get_argc(char **args, int *argc)
{
    char *copied_val;
    int i = 0;
    do {
        copyin((const_userptr_t) &args[i], (void *) &copied_val, (size_t) (sizeof(char *)));
        i++;
    } 
    while(copied_val != NULL && i < ARG_MAX);

    if(i==ARG_MAX) {
        return E2BIG; // too many arguments
    }

    *argc = i-1; // null is not included
    return 0; // size of the args array

}

int
copy_in_args(char **args, char **kargs, int argc, int *size_arr)
{
    int err = 0;
    size_t actual_size;

    for (int i=0; i<argc; i++) {

        err = copyinstr((const_userptr_t) args[i], kargs[i], (size_t) (sizeof(char *)), &actual_size);
        kprintf("the actual size after copy in is %d\n", (int)actual_size);
        kprintf("the argument is %s\n", kargs[i]);
        size_arr[i] = (int) actual_size;

        if(err) {
            return err;
        }
    }

    return 0;
}

int
copy_out_args(char **kargs, userptr_t argv, vaddr_t *stackptr, int argc, int *size_arr)
{
    int result =0;
    size_t actual_size = 0;
    (void)stackptr;
    (void) kargs;
    (void)actual_size; 
    (void)argv;

    for (int i=0; i<argc; i++) {
        kprintf("size_arr %d is %d\n", i, size_arr[i]);
        *stackptr = *stackptr-size_arr[i];
        result = copyoutstr((char *) kargs[i], (userptr_t)*stackptr, (size_t) size_arr[i], &actual_size);

        if(result) {
            kprintf("copy out error with error %s\n", strerror(result));
            return result;
        }
    }
    return 0;
}
