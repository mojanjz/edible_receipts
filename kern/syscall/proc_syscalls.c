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

/* 
 * Duplicates the currently running process as the child of the current process
 * 
 * Paramters: trapframe (the trapframe of the process before entering the system call handler)
 *            retval (the return value), for the parent process
 * Returns: On success: 0, enters the child process, updated retval to child process's PID
 *          On failure: error code
 */
int
sys_fork(struct trapframe *tf, int *retval)
{
    int err = 0;
    struct proc *child_proc;
    pid_t child_pid;

    /* Create and setup the new process */
    child_proc = proc_create_fork("child-process", &err);
    if(err) {
        return err;
    }
    child_pid = child_proc->p_pid;

    /* Copy the parent's filetable */
    filetable_copy(child_proc->p_filetable, curproc->p_filetable);

    /* Copy the parent's trapframe */
    struct trapframe *child_tf = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if(child_tf == NULL) {
        return ENOMEM;
    }
    memcpy((void *)child_tf, (const void *)tf, sizeof(struct trapframe));
    /* Update v0, v1, a3, prog counter for child */
    child_tf->tf_v0 = 0;
    child_tf->tf_v1 = 0;
    child_tf->tf_a3 = 0;
    child_tf->tf_epc = child_tf->tf_epc + 4;

    /* Make kernel thread for child */
    err = thread_fork("child-thread", child_proc, enter_new_forked_process, child_tf, 0);

    if(err) {
        kfree(child_tf);
        proc_destroy(child_proc);
        return err;
    }
    
    /* Update the return value for the parent fork */
    KASSERT(child_pid == child_proc->p_pid);
    *retval = (int) child_pid;
    return 0; 
}

/* 
 * Used by sys_fork, copies the trapframe of the child to the stack, and enters usermode
 * Parameters: data1: used to store the trapframe
 *             data2: unused, for convention
 * Returns: this function should not return
 */
void
enter_new_forked_process(void *data1, unsigned long data2){
    (void)data2;

    /* Copy the trapframe onto the stack */
    struct trapframe *tf = curthread->t_stack+16;
    memcpy(tf, (const void *)data1, sizeof(struct trapframe));
    kfree((struct trapframe *)data1);

    /* Activate the address space and enter user mode */
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
sys_waitpid(pid_t pid, int *status, int options, int *retval)
{
    int exitcode;

    /* Options are not supported */
    if (options != 0){
        return EINVAL;
    }
    /* Make sure waitpid being called on an existant process */
    if (pid > __PID_MAX || pid < __PID_MIN || array_get(pid_table->process_statuses, pid) == AVAILABLE || array_get(pid_table->process_statuses, pid) == NULL) {
        return ESRCH;
    }
    /* Make sure that pid argument names a process that is a child of curent process */
    if (!is_child(pid)){
        return ECHILD;
    }
    lock_acquire(pid_table->pid_table_lk);
    while ((int)array_get(pid_table->process_statuses, pid) != ZOMBIE){
        cv_wait(pid_table->pid_table_cv, pid_table->pid_table_lk);
    }
    exitcode = (int)array_get(pid_table->process_exitcodes, pid);

    lock_release(pid_table->pid_table_lk);

    if (status != NULL){
        int retval = copyout(&exitcode, (userptr_t)status, sizeof(exitcode));
        if (retval){
            return retval;
        }
    }
    *retval = pid;
    return 0;
}

/* Checks if process with PID pid is a child of curent process.
 *
 * Parameters: pid (the pid of the child process to verify)
 * Returns: true if process is a child or curproc, false otherwise
 */
bool
is_child(pid_t pid)
{
    bool is_child = false;
    int num_children = array_num(curproc->p_children);
    
    for (int i = 0; i < num_children; i++){
        if ((pid_t)array_get(curproc->p_children,i) == pid){
            is_child = true;
            break;
        }
    }

    return is_child;
}

/* 
 * Causes the current process to exit
 *
 * Parameters: exitcode (exitcode to report back to other processes via waitpid)
 * Returns: Exit does not return!
 */
void
sys__exit(int exitcode)
{
    lock_acquire(pid_table->pid_table_lk);

    /* Update statuses of exiting process' children */
    for (unsigned i = 0; i < array_num(curproc->p_children); i++){
        /* If child is still running, make an orphan */
        pid_t child_pid = (int)array_get(curproc->p_children,i);
        if ((int)array_get(pid_table->process_statuses, child_pid) == OCCUPIED) {
            array_set(pid_table->process_statuses, child_pid, (void *)ORPHAN);
        } 
        /* If child is already a zombie, destroy it */
        else if ((int)array_get(pid_table->process_statuses, child_pid) == ZOMBIE) { 
            proc_destroy(array_get(pid_table->processes, child_pid));
            delete_pid_entry(child_pid);
        } else {
            /* Child process has an invalid status */
            lock_release(pid_table->pid_table_lk);
            panic("Exiting process' child has invalid status"); /* Can't return an error code here since exit should not return */
        }  
    }

    /* Update process: */
    /* Process is orphan - no parent waiting on it, proceed by destroying */
    if ((int)array_get(pid_table->process_statuses, curproc->p_pid) == ORPHAN) {
        delete_pid_entry(curproc->p_pid);
        proc_destroy(curproc);
    }
    /* Process has a parent - signal to parent that the process has finished & don't destroy yet*/
    else if ((int)array_get(pid_table->process_statuses, curproc->p_pid) == OCCUPIED){
        array_set(pid_table->process_exitcodes, curproc->p_pid, (void *)exitcode);
        array_set(pid_table->process_statuses, curproc->p_pid, (void *)ZOMBIE);
    } else {
        /* Parent process has invalid status */
        lock_release(pid_table->pid_table_lk);
        panic("Exiting process status is invalid"); /* Can't return an error code here since exit should not return */
    }

    cv_broadcast(pid_table->pid_table_cv, pid_table->pid_table_lk);

    lock_release(pid_table->pid_table_lk);

    /* Last command that should run, shouldn't return */
    thread_exit();
    /*  
     * ------------------------------
     *process should never get this far
     */
    panic("Process did not exit correctly");
}

/* Runs the given program within the current process.
 * Usually used after sys_fork, for running a new process
 * Parameters: program (user pointer to the name of the program)
 *             args (arguments of the given program, also in userspace)
 * Returns: On success, it does not return and enters the new process.
 *          On failure, returns the error code.
 */
int 
sys_execv(userptr_t program, char **args)
{

    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    int result;
    int argc = 0;

    /* Copy program name in*/
    char *kernel_progname;

    kernel_progname = (char *)kmalloc(PATH_MAX);
    if (kernel_progname == NULL) {
        return ENOMEM;
    }

    result = copyinstr(program, kernel_progname, PATH_MAX, NULL);
    if (result) {
        kfree(kernel_progname);
        return result;
    }

    /* Get the size of the arguments array */
    result = get_argc(args, &argc);
    if (result) {
        kfree(kernel_progname);
        return result;
    }

    /* Malloc the kernel argument array */ 
    char **kargs;
    kargs = kmalloc(argc*(sizeof(char *)));
    if (kargs == NULL) {
        kfree(kernel_progname);
        return ENOMEM;
    }

    /* Malloc a size array to store the size of all arguments */
    int *size_arr = kmalloc(argc*(sizeof(int)));
    if (size_arr == NULL) {
        kfree(kargs);
        kfree(kernel_progname);
        return ENOMEM;
    }

    /* Copy in arguments into the kernel buffer */
    result = copy_in_args(args, kargs, argc, size_arr);
    if (result) {
        kfree(kargs);
        kfree(size_arr);
        kfree(kernel_progname);
        return result;
    }

    /* Open the file */
    result = vfs_open(kernel_progname, O_RDONLY, 0, &v);
    if (result) {
        goto fail;
    }

    /* Create a new address space */
    as = as_create();
    if (as == NULL) {
        result = ENOMEM;
        goto fail;
    }
    
    /* Switch to it and activate it. */
    struct addrspace *old_as = proc_setas(as);
    as_activate();

    /* Load the executable and run it */
    result = load_elf(v, &entrypoint);
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

    /* Copy arguments from kernel buffer to user stack */
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

/* execv helper functions */

/* Returns the total number of arguments in the userspace args array by copying a few characters 
 * Parameters: args (array of arguments in the userspace)
 *             argc (the variable in which the number will be stored)
 * Returns: On success: 0, and argc is updated
 *          On failure: error code
 */
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
    return 0;

}

/* Copies in the arguments from userspace to the kernel buffer
 * Parameters: args (array of arguments in the userspace)
 *             kargs (array in kernel space to store the arguments)
 *             argc (the number of arguments to be copied in)
 *             size_arr (the array that stores the size of all arguments)
 * Returns: On success: 0, and kargs, size_arr are updated
 *          On failure: error code
 */
int
copy_in_args(char **args, char **kargs, int argc, int *size_arr)
{
    int err = 0;
    size_t actual_size;
    size_t max_size = ARG_MAX;

    for (int i=0; i<argc; i++) {
        /* get the actual size of the argument while making sure we don't exceed ARG_MAX */
        err = arg_length((const char *) args[i], max_size, &actual_size);
        if (err) {
            return err;
        }

        max_size-=actual_size;
        size_t pad_room = 4 - (actual_size % 4); // each argument is padded so the length is divisble by 4

        kargs[i] = kmalloc((actual_size+pad_room)*sizeof(char));
        if (kargs[i] == NULL) {
            return ENOMEM;
        }

        err = copyinstr((const_userptr_t) args[i], kargs[i], actual_size, NULL);

        if(err) {
            for (int j=0; j<i; j++) {
                kfree(kargs[i]);
            }
            return err;
        }

        /* Pad the arguments after copy in and store the padded_size in size_arr */
        int padded_size = pad_argument(kargs[i], (int) actual_size);
        size_arr[i] = padded_size;
    }

    return 0;
}

/* Copies out the arguments from ukernel buffer to user stack.
 *
 * Parameters: kargs (array in kernel space that has the arguments)
 *             stackptr (the current address of the stackpointer)
 *             argc (the number of arguments to be copied in)
 *             size_arr (the array that has the size of all arguments)
 * Returns: On success: 0, and stackptr is updated
 *          On failure: error code
 */
int
copy_out_args(char **kargs, vaddr_t *stackptr, int argc, int *size_arr)
{
    int result =0;

    /* Setup pointers to the address of arguments (arg_pointer) and the pointer (arg_addr) that points to the start of the argument */
    userptr_t arg_addr = (userptr_t) (*stackptr);
    userptr_t *arg_pointer = (userptr_t *) (arg_addr-total_size_args(size_arr,argc));

    /* Manually set the last pointer to NULL */
    arg_pointer --;
    *arg_pointer = NULL;

    /* Decrement arg_addr by the size of the argument, copyout the argument, and store the address in arg_pointer */
    for (int i=argc-1; i>=0; i--) { // store from last to first to keep consistency between arg_pointer and the arguments
        arg_pointer --; // decrement since we are going down the stack
        arg_addr -= size_arr[i];
        *arg_pointer = arg_addr;
        result = copyout((void *)kargs[i], arg_addr, size_arr[i]);
        if(result) {
            return result;
        }
    }
    /* Update stack pointer */
    *stackptr = (vaddr_t)arg_pointer;
    
    return result;
}

/* 
 * Pads arguments for the size to be divisible by 4 to align before copying to user stack
 *
 * Paramters: arg (the argument to be padded), size (size of the argument before padding)
 * Returns: padded_size (size of the array after padding), updates the actual argument
 */
int
pad_argument(char *arg, int size)
{
    int pad_count = 4 - (size % 4);
    for (int i=size; i<size+pad_count; i++) {
        arg[i] = '\0';
    }

    return size+pad_count;
}

/* 
 * Returns the total size of the arguments
 * 
 * Parameter: size_arr (the array the stores the size of all the arguments)
 *            argc (the number of arguments)
 * Returns: ttotal_size (the sum of the size of all arguments)
 */
int
total_size_args(int *size_arr, int argc)
{
    int total_size = 0;
    for (int i=0; i<argc; i++) {
        total_size+=size_arr[i];
    }

    return total_size;
}
/* 
 * Calculates the length of a given argument in userspace by copying in one character at a time until it hits NULL
 * 
 * Parameter: arg (the argument)
 *            max_size (maximum allowed size to ensure the total length of all arguments does not exceed ARGMAX)
 *            size (the variable to store the actual size)
 * Returns: On success: 0, updated the size variable
 *          On failure: error code
 */
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