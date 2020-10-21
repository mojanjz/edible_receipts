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
* position argument ), whence (integer that specifies how to calculate new seek position), pointer to return value address.
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
        
    pos = ((off_t)higher_pos << 32 | lower_pos); //TODO: make sure this works for negative pos values too!!
    //try a full 32 bit roster and see if you get negative number
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
    /* Switch on whence for new position value */
    switch (*kernel_whence){
        case SEEK_SET:
        pos = pos;
        break;

        case SEEK_CUR:
        pos = fe->fe_offset + pos; 
        break;

        case SEEK_END:
        VOP_STAT(fe->fe_vn, &ft_stat); //TODO: come back to this
        pos = ft_stat.st_size + pos;
        break;
    }
    /* If valid, change the seek position */
    if(pos < 0){
        lock_release(fe->fe_lock);
        return EINVAL;
    } else{
        fe->fe_offset = pos;
    }
    lock_release(fe->fe_lock);
    //issue is retval is 32 bit here
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
    // char *kernel_buf;
    struct iovec iov;
    struct uio ku;

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

    /* actual read operation */
    struct file_entry *fe = ft->ft_file_entries[fd];
    lock_acquire(fe->fe_lock);
    int how = fe->fe_status & O_ACCMODE;

    /* Check if file is opened for reading */
    if (how != O_RDONLY && how != O_RDWR) {
        lock_release(fe->fe_lock);
        return EBADF;
    }

    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &ku, buf, buflen, pos, UIO_READ);
    err = VOP_READ(fe->fe_vn, &ku);
    if (err) {
        kprintf("%s: Read error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        return err;
    }

    *retval = ku.uio_offset - pos;

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
    struct uio ku;

    /* Check for invalid file descriptor or unopened files */
    if (fd < 0 || fd > __OPEN_MAX-1) { // TODO: POTENTIAL RACE CONDITION
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

    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &ku, buf, nbytes, pos, UIO_WRITE);
    err = VOP_WRITE(fe->fe_vn, &ku);
    if (err) {
        kprintf("%s: Write error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        return err;
    }

    *retval = ku.uio_offset - pos; // -1 to ignore the 0 

    fe->fe_offset += *retval;
    lock_release(fe->fe_lock);
    return 0;
}

/* 
 * Clones the file handle oldfd onto the file handle newfd. If newfd names an already-open file, that file is closed. 
 * Note that both file handles refer to the same open file entry.
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

    kprintf("new and old fd are good: %d, %d\n", newfd, oldfd);

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
    kprintf("ref counts is now %d\n", ft->ft_file_entries[oldfd]->fe_refcount);
    ft->ft_file_entries[newfd] = ft->ft_file_entries[oldfd];
    *retval = newfd;
    lock_release(ft->ft_lock);

    return 0;
}