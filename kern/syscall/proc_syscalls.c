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
int copy_out_args(char **kargs, vaddr_t *stackptr, int argc, int *size_arr);
int get_argc(char **args, int *argc);
int pad_argument(char *arg, int size);
int total_size_args(int *size_arr, int argc);
int arg_length(const char *arg,size_t max_size, size_t *size);

int
sys_fork(struct trapframe *tf, int *retval)
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
    // tf->tf_v0 = child_pid;
    *retval = (int) child_pid;
    return 0; 
}

void
enter_new_forked_process(void *data1, unsigned long data2){
    (void)data2;

    struct trapframe *tf = curthread->t_stack+16; //trapframe should be on the stack
    memcpy(tf, (const void *)data1, sizeof(struct trapframe));
    kfree((struct trapframe *)data1);

    as_activate();
    mips_usermode(tf);
}

int
sys_getpid(int *retval){
    lock_acquire(pid_table->pid_table_lk);
    *retval = (int) curproc->p_pid;
    lock_release(pid_table->pid_table_lk);
    return 0;
}

/* 
 * Waits for a specific process to exit, and return an encoded exit status.  If that process does not 
 * exist, waitpid fails. Note that status == NULL is expressly allowed and indicates that waitpid 
 * operates as normal but doesn't produce a status value.
 * 
 * Parameters: pid (the pid of the process on which to wait), *status (the pointer to integer storing the processes
 * exitcode), options (should always be zero, not implementing options)
 * Returns: the process id whose exit status is reported in status (pid)
 */
pid_t
sys_waitpid(pid_t pid, int *status, int options)
{
    int exitcode;

    /* Options are not supported */
    if (options != 0){
        return EINVAL;
    }
    /* Make sure waitpid being called on an existant process */
    if (pid > __PID_MAX || pid < __PID_MIN || pid_table->process_statuses[pid] == AVAILABLE){
        return ESRCH;
    }
    /* Make sure that pid argument names a process that is a child of curent process */
    if (!isChild(pid)){
        return ECHILD;
    }
    lock_acquire(pid_table->pid_table_lk);
    while (pid_table->process_statuses[pid] != ZOMBIE){
        cv_wait(pid_table->pid_table_cv, pid_table->pid_table_lk);
    }
    exitcode = pid_table->process_exitcodes[pid];

    lock_release(pid_table->pid_table_lk);

    if (status != NULL){
        int retval = copyout(&exitcode, (userptr_t)status, sizeof(int32_t)); //TODO: is int32 the right thing to take the size of? should be size of exitcode
        if (retval){
            return retval;
        }
    }
    return pid;
}

/* Checks if process with PID pid is a child of curent process */
bool
isChild(pid_t pid)
{
    bool is_child = false;
    int num_children = array_num(curproc->p_children);
    
    for (int i = 0; i < num_children; i++){
        if ((pid_t)array_get(curproc->p_children,i) == pid){ /* TODO: Check that this cast does what is expected */
            is_child = true;
            break;
        }
    }

    return is_child;
}

void
sys__exit(int exitcode)
{
    lock_acquire(pid_table->pid_table_lk);

    /* Update children */
    for (unsigned i = 0; i < array_num(curproc->p_children); i++){
        /* If child is an running, make an orphan */
        pid_t child_pid = (int)array_get(curproc->p_children,i);
        if (pid_table->process_statuses[child_pid] == OCCUPIED){
            pid_table->process_statuses[child_pid] = ORPHAN;
        } 
        /* If child is a zombie, destroy it */
        else if (pid_table->process_statuses[child_pid] == ZOMBIE){ 
            proc_destroy(pid_table->processes[child_pid]);
            delete_pid_entry(child_pid);
        } else {
            //TODO: fix error handling
            kprintf("CHILD STATUS IS INVALID IN SYS__EXIT");
        }  
    }

    /* Update process */
    /* Process is orphan: no parent waiting on it, proceed by destroying */
    if (pid_table->process_statuses[curproc->p_pid] == ORPHAN){
        delete_pid_entry(curproc->p_pid);
        proc_destroy(curproc); //TODO: check order of these two operations
    }
    /* Process has a parent: signal to parent that the process has finished & don't destroy yet*/
    else if (pid_table->process_statuses[curproc->p_pid] == OCCUPIED){
        pid_table->process_exitcodes[curproc->p_pid] = exitcode;
        pid_table->process_statuses[curproc->p_pid] = ZOMBIE;
    } else {
        //TODO: fix error handling
        kprintf("PROCESS STATUS IS INVALID IN SYS__EXIT");
    }

    //TODO: implement condition variable and broadcast here
    cv_broadcast(pid_table->pid_table_cv, pid_table->pid_table_lk);

    lock_release(pid_table->pid_table_lk);
    /* Last command that should run, shouldn't return */
    thread_exit();
    /*  
     * ------------------------------
     *process should never get this far
     */
    //TODO: add panic?
}

int 
sys_execv(userptr_t program, char **args)
{
    kprintf("in execv for process with pid %d\n", curproc->p_pid);
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
    kargs = kmalloc(argc*(sizeof(char *))); // TODO: can optimize by kmallocing each pointer, check if kargs is null
    result = copy_in_args(args, kargs, argc, size_arr);
    if (result) {
        kfree(kargs);
        kfree(size_arr);
        kfree(kernel_progname);
        return result;
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
        goto fail;
    }

    /* Switch to it and activate it. */
    struct addrspace *old_as = proc_setas(as); //TODO: restore if failure happens
    as_activate();

    /* Load the executable and run it */
    result = load_elf(v, &entrypoint);
    (void)entrypoint;
    if (result) {
        vfs_close(v);
        proc_setas(old_as);
        as_activate();
        goto fail;
    }

    vfs_close(v);

    /* Define the user stack in the address space */
    result = as_define_stack(as, &stackptr);
    if (result) {
        proc_setas(old_as);
        as_activate();
        goto fail;
    }

    /* Copy arguments from kernel to user stack */
    result = copy_out_args(kargs, &stackptr, argc, size_arr);
    if (result) {
        proc_setas(old_as);
        as_activate();
        goto fail;
    }

    /* Clean up before user mode*/
    for(int i=0; i<argc; i++) {
        kfree(kargs[i]);
    }
    as_destroy(old_as);
    kfree(kernel_progname);
    kfree(kargs);
    kfree(size_arr);
    kprintf("execv: we made it! going to user mode!\n");
    /* Return to user mode */
    enter_new_process(argc, (userptr_t)stackptr, NULL, stackptr, entrypoint);
    /* enter process does not return. */
    panic("enter_new_process returned \n");
    return EINVAL; // should never get here

fail:
    for(int i=0; i<argc; i++) {
        kfree(kargs[i]);
    }
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
    char err = 0;

    do {
        err = copyin((const_userptr_t) &args[i], (void *) &copied_val, (size_t) (sizeof(char *)));
        if (err) {
            return err;
        }
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
    size_t max_size = ARG_MAX;

    for (int i=0; i<argc; i++) {
        err = arg_length((const char *) args[i], max_size, &actual_size);
        if (err) {
            return err;
        }

        max_size-=actual_size;
        size_t pad_room = 4 - (actual_size % 4);

        kargs[i] = kmalloc((actual_size+pad_room)*sizeof(char)); // TODO have the actual size
        // kargs[i] = kmalloc(__PATH_MAX*sizeof(char));
        // err = copyin((const_userptr_t)&args[i], (void *)&kargs[i], actual_size);
        err = copyinstr((const_userptr_t) args[i], kargs[i], actual_size, NULL); // TODO: what should the size be here

        if(err) {
            for (int j=0; j<i; j++) {
                kprintf("copy in failed:(\n");
                kfree(kargs[i]);
            }
            return err;
        }

        int padded_size = pad_argument(kargs[i], (int) actual_size);
        size_arr[i] = padded_size;
    }

    return 0;
}

int
copy_out_args(char **kargs, vaddr_t *stackptr, int argc, int *size_arr)
{
    int result =0;

    userptr_t arg_addr = (userptr_t) (*stackptr);
    userptr_t *arg_pointer = (userptr_t *) (arg_addr-total_size_args(size_arr,argc));
    // should be null terminated
    arg_pointer --;
    *arg_pointer = NULL;

    for (int i=argc-1; i>=0; i--) {
        arg_pointer --;
        // kprintf("size_arr %d is %d\n", i, size_arr[i]);
        arg_addr -= size_arr[i];
        *arg_pointer = arg_addr;
        result = copyout((void *)kargs[i], arg_addr, size_arr[i]);

        if(result) {
            kprintf("copy out error with error %s\n", strerror(result));
            return result;
        }
    }

    *stackptr = (vaddr_t)arg_pointer;
    
    return result;
}

// pads each argument with null terminators, returns the new size
int
pad_argument(char *arg, int size)
{
    int pad_count = 4 - (size % 4);
    for (int i=size; i<size+pad_count; i++) {
        arg[i] = '\0';
    }

    return size+pad_count;
}

int
total_size_args(int *size_arr, int argc)
{
    int total_size = 0;
    for (int i=0; i<argc; i++) {
        total_size+=size_arr[i];
    }

    return total_size;
}

int
arg_length(const char *arg, size_t max_size, size_t *size)
{
    char next_char;
    size_t i=0;
    int err =0;

    do {
        err = copyin((const_userptr_t)&arg[i], (void *)&next_char, (size_t)sizeof(char));
        if (err) {
            return err;
        }
        i++;

    }while(next_char != 0 && i < max_size);

    if(next_char != 0) {
        return E2BIG;
    }

    *size=i;
    return 0;
}