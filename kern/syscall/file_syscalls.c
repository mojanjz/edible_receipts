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
#include <file_syscalls.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <limits.h>
#include <addrspace.h>
#include <mips/trapframe.h>

/*
 * Opens a file, device, or other kernel device.
 * 
 * Parameters: filename (the pathname), flags (specifies how to open file),
 * mode (optional argument that provides file permissions), pointer to return value address.
 * Returns: On success, the non-negative file handle of the opened file.  On error, -1
 * and errno is set.
 */
int
sys_open(userptr_t filename, int flags, mode_t mode, int *retval)
{
    int err = 0;
    char *kernel_filename;

    kernel_filename = (char *)kmalloc(__PATH_MAX);
    if (kernel_filename == NULL)
        return ENOMEM;
    
    /* copyin the filename */
    err = copyinstr(filename, kernel_filename, __PATH_MAX, NULL);
    if (err) {
        kfree(kernel_filename);
        return err;
    }

    err = file_open(kernel_filename, flags, mode, retval);
    kfree(kernel_filename);
    return err;
}

/* 
 * Closes the file entry corresponding to file handle fd.
 * 
 * Parameters: fd (the file handle corresponding to the file entry to be closed)
 * Returns: 0 on success.  On error, -1 and errno is set.
 */
int sys_close(int fd)
{
    int result = 0;

    /* Check for invalid file descriptor or unopened files */
    if(fd < 0 || fd > __OPEN_MAX-1) {
        return EBADF;
    }

    result = file_close(fd);
    return result;
}

/*
* Alters the current seek position of the the file entry corresponding to fd.  Seek positions less than zero
* are invalid, and seek positions beyond EOF are legal.
* 
* Parameters: fd (file handle corresponding to file entry who's seek position is to be changed),
* higher_pos (the high 32-bits of the 64-bit position argument), lower_pos (the low 32-bits of the 64-bit
* position argument ), whence (integer that specifies how to calculate new seek position), pointer to 64-bit 
* return value address.
* Returns: On success, the new position.  On error, -1 and errno is set.
*/
int
sys_lseek(int fd, off_t higher_pos, off_t lower_pos, int whence, off_t *retval)
{
    off_t pos;
    struct filetable *ft = curproc->p_filetable;
    struct stat ft_stat;
    int *kernel_whence;

    kernel_whence = (int *)kmalloc(sizeof(SEEK_END));
    if (kernel_whence == NULL){
        return ENOMEM;
    }
    
    /* Concatenate the higher and lower 32-bits into 64-bit variable pos */
    pos = ((off_t)higher_pos << 32 | lower_pos); 
    copyin((const_userptr_t) whence, kernel_whence, sizeof(kernel_whence)); 
    
    /* Check if fd is a valid file handle */
    lock_acquire(ft->ft_lock);
    if ((fd < 0) | (fd >= __OPEN_MAX)){
        lock_release(ft->ft_lock);
        return EBADF;
    } else if (ft->ft_file_entries[fd] == NULL){
        lock_release(ft->ft_lock);
        return EBADF;
    }
    lock_release(ft->ft_lock); 

    /* Check if whence is valid */
    if ((*kernel_whence != SEEK_SET) && (*kernel_whence != SEEK_CUR) && (*kernel_whence != SEEK_END))
        return EINVAL;

    /* Check if file is seekable */
    struct file_entry *fe = ft->ft_file_entries[fd];
    lock_acquire(fe->fe_lock);
    if(!VOP_ISSEEKABLE(fe->fe_vn)){
        lock_release(fe->fe_lock);
        return ESPIPE;
    }

    switch (*kernel_whence){
        /* The new position is pos */
        case SEEK_SET:
        pos = pos;
        break;
        
        /* The new position is the current position plus pos */
        case SEEK_CUR:
        pos = fe->fe_offset + pos; 
        break;
        
        /* The new position is the position of end-of-file plus pos */
        case SEEK_END:
        /* Get file's EOF */
        VOP_STAT(fe->fe_vn, &ft_stat);
        pos = ft_stat.st_size + pos;
        break;
    }
    /* Check that the new seek position is valid */
    if(pos < 0){
        lock_release(fe->fe_lock);
        return EINVAL;
    } else{
        fe->fe_offset = pos;
    }
    lock_release(fe->fe_lock);

    *retval = pos;
    return 0;
}

/* 
* Reads up to buflen bytes from the file entry specified by fd, at the 
* location specified by the file entry's seek position. The file must be open
* for reading. The current seek position of the file is advanced by the number of 
* bytes read.
* 
* Parameters: fd (file handle of file to be read from), buf (pointer to location where 
* read values are to be stored), buflen (number of bytes to read from file entry), pointer to
* return value address.
* Returns: On success, count of bytes read (positive) & 0 if at end of file.  On failure, -1
* and errno set.
*/
int
sys_read(int fd, userptr_t buf, size_t buflen, int *retval)
{
    int err = 0;
    struct filetable *ft = curproc->p_filetable;
    struct iovec iov;
    struct uio user_uio;

    /* Check for invalid file descriptor */
    if (fd < 0 || fd > __OPEN_MAX-1)
        return EBADF;
    
    lock_acquire(ft->ft_lock);

    if (ft->ft_file_entries[fd] == NULL || ft->ft_file_entries[fd]->fe_vn == NULL)
    {
        lock_release(ft->ft_lock);
        return EBADF;
    }
    lock_release(ft->ft_lock);

    /* Perform actual read operation */
    struct file_entry *fe = ft->ft_file_entries[fd];
    lock_acquire(fe->fe_lock);
    int how = fe->fe_status & O_ACCMODE;

    /* Check if file is opened for reading */
    if (how != O_RDONLY && how != O_RDWR) {
        lock_release(fe->fe_lock);
        return EBADF;
    }

    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &user_uio, buf, buflen, pos, UIO_READ);
    err = VOP_READ(fe->fe_vn, &user_uio);
    if (err) {
        kprintf("%s: Read error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        return err;
    }

    *retval = user_uio.uio_offset - pos;

    fe->fe_offset += *retval;
    lock_release(fe->fe_lock);

    return 0;
}

/* 
 * Writes up to nbytes bytes to the file specified by fd , at the location in the file specified 
 * by the current seek position of the file, taking the data from the space pointed to by buf.  Note 
 * that the file must be open for writing.
 * 
 * Parameters: fd (file handle corresponding to file entry to write to), buf (pointer
 * to space to store values to be written), nbytes (number of bytes to write), pointer to return value address
 * Return: On succes, the number of bytes written (positive).  On failure, -1 and errno set.
 */
int
sys_write(int fd, userptr_t buf, size_t nbytes, int *retval)
{   
    int err = 0;
    struct filetable *ft = curproc->p_filetable;
    struct iovec iov;
    struct uio user_uio;

    /* Check for invalid file descriptor or unopened files */
    if (fd < 0 || fd > __OPEN_MAX-1) { 
        return EBADF;
    }

    lock_acquire(ft->ft_lock);
    if (ft->ft_file_entries[fd] == NULL || ft->ft_file_entries[fd]->fe_vn == NULL)
    {
        lock_release(ft->ft_lock);
        return EBADF;
    }
    lock_release(ft->ft_lock);

    struct file_entry *fe = ft->ft_file_entries[fd];
    lock_acquire(fe->fe_lock);
    int how = fe->fe_status & O_ACCMODE;

    /* Check if file is opened for writing */
    if (how != O_WRONLY && how != O_RDWR) {
        lock_release(fe->fe_lock);
        return EBADF;
    }

    /* Perform actual write opperation */
    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &user_uio, buf, nbytes, pos, UIO_WRITE);
    err = VOP_WRITE(fe->fe_vn, &user_uio);
    if (err) {
        kprintf("%s: Write error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        return err;
    }

    *retval = user_uio.uio_offset - pos;

    fe->fe_offset += *retval;
    lock_release(fe->fe_lock);
    return 0;
}

/* 
 * Clones the file handle oldfd onto the file handle newfd. If newfd names an already-open file, that file is closed. 
 * Note that both file handles refer to the same open file entry. If newfd and old fd are the same, nothing happens.
 * Parameters: oldfd (file handle to be cloned), newfd (file handle to be cloned onto), pointer to return val address.
 * Returns: On success, newfd.  On failure, -1 and errno set.
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{   
    struct filetable *ft = curproc->p_filetable;
    int result = 0;

    /* Check for invalid file descriptors */
    if (oldfd < 0 || oldfd > __OPEN_MAX-1)
        return EBADF;
    
    if (newfd < 0 || newfd > __OPEN_MAX-1)
        return EBADF;

    if (newfd == oldfd){
        *retval = newfd;
        return 0;
    }
    
    lock_acquire(ft->ft_lock);
    if (ft->ft_file_entries[oldfd] == NULL || ft->ft_file_entries[oldfd]->fe_vn == NULL)
    {
        lock_release(ft->ft_lock);
        return EBADF;
    }
    lock_release(ft->ft_lock);

    /* If newfd is open close it */
    lock_acquire(ft->ft_lock);
    if (ft->ft_file_entries[newfd] != NULL){
        result = dup_file_close(newfd);
        if(result) {
            lock_release(ft->ft_lock);
            return result;
        }
    }

    ft->ft_file_entries[oldfd]->fe_refcount += 1;
    ft->ft_file_entries[newfd] = ft->ft_file_entries[oldfd];
    *retval = newfd;
    lock_release(ft->ft_lock);

    return 0;
}

int
sys_fork(struct trapframe *tf, pid_t *retval)
{
    int err = 0;
    size_t child_name_size = 12;
    (void) retval;
    (void) tf;
    struct proc *child_proc;
    pid_t child_pid;

    /* Get the next available PID for the parent */
    child_pid = issue_pid(); // TODO CHANGE issue_pid

    /* Create child process */
    char child_name[child_name_size];
    snprintf(child_name, child_name_size, "%s-pid:%d", curproc->p_name, (int)child_pid);
    child_proc = proc_create(child_name);

    /* Copy the address space of the parent */
    struct addrspace *child_as = (struct addrspace *)kmalloc(sizeof(struct addrspace));
    if (child_as == NULL) {
        return ENOMEM;
    }

    err = as_copy(curproc->p_addrspace, &child_as);
    if(err) {
        return err;
    }

    proc_setas(child_as);

    kprintf("the parent address space npages1 %zu and child is %zu\n", curproc->p_addrspace->as_npages1, child_proc->p_addrspace->as_npages1);

    /* Copy the parent filetable */
    filetable_copy(child_proc->p_filetable, curproc->p_filetable);

    kprintf("parent first filename %d, child first filename %d\n", curproc->p_filetable->ft_file_entries[0]->fe_status, child_proc->p_filetable->ft_file_entries[0]->fe_status);

    /* Copy the parent's trapframe */


    return err; 
}