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

/*
 * Copies the filename from userpointer buffer to a kernel buffer
 * Calls file_open that does the actual opening
 */
int
sys_open(userptr_t filename, int flags, mode_t mode, int *retval)
{
    int err = 0;
    char *kernel_filename;

    // kprintf("made it to sys_open\n");

    kernel_filename = (char *)kmalloc(__PATH_MAX);
    if (kernel_filename == NULL)
        return ENOMEM;

    /* copyin the filename */
    err = copyinstr(filename, kernel_filename, __PATH_MAX, NULL);
    if (err) {
        kfree(kernel_filename);
        return err;
    }

    kprintf("sys_open: got the filename %s\n", kernel_filename);

    err = file_open(kernel_filename, flags, mode, retval);

    kfree(kernel_filename);
    return err;
}

/* Checks for file descriptor to be in a valid range
 * Calls file_close that does that actual closing
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

int
sys_lseek(int fd, off_t higher_pos, off_t lower_pos, int whence, off_t *retval)
{
    off_t pos;
    struct filetable *ft = curthread->t_filetable;
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
    kprintf("The fd is %d ", fd);
    kprintf("Is the fd less than zero? %d", fd<0);
    kprintf("Is the fd greater than om? %d ", fd>= __OPEN_MAX);
    if ((fd < 0) | (fd >= __OPEN_MAX)){
        kprintf("Checkpoint 1");
        lock_release(ft->ft_lock);
        return EBADF;
    } else if (ft->ft_file_entries[fd] == NULL){
        lock_release(ft->ft_lock);
        return EBADF;
    }
    kprintf("Checkpoint 2");
    lock_release(ft->ft_lock); 
    /* Check if whence is valid */
    if ((*kernel_whence != SEEK_SET) && (*kernel_whence != SEEK_CUR) && (*kernel_whence != SEEK_END))
        return EINVAL;
    /* Check if file is seekable */
    struct file_entry *fe = ft->ft_file_entries[fd];
    kprintf("Checkpoint 3");
    lock_acquire(fe->fe_lock);
    if(!VOP_ISSEEKABLE(fe->fe_vn)){
        lock_release(fe->fe_lock);
        return ESPIPE;
    }
    kprintf("Checkpoint 4");
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
    kprintf("Checkpoint 5");
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

int
sys_read(int fd, userptr_t buf, size_t buflen, int *retval)
{
    int err = 0;
    struct filetable *ft = curthread->t_filetable;
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
        // kfree(kernel_buf);
        return EBADF;
    }

    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &ku, buf, buflen, pos, UIO_READ);
    err = VOP_READ(fe->fe_vn, &ku);
    if (err) {
        kprintf("%s: Read error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        // kfree(kernel_buf);
        return err;
    }

//     err = copyoutstr(kernel_buf, buf, buflen+1, NULL);
//     if (err) {
// //        kprintf("couldn't copy the buffer in\n");
//         lock_release(fe->fe_lock);
//         kfree(kernel_buf);
//         return err;
//     }

    // *retval = ku.uio_offset - pos -1; // -1 to avoid the zero
    *retval = ku.uio_offset - pos;
    // kprintf("successfully wrote to the file and ret val is now %d\n", *retval);

    fe->fe_offset += *retval;
    lock_release(fe->fe_lock);
    // kfree(kernel_buf);
    return 0;
}

int
sys_write(int fd, userptr_t buf, size_t nbytes, int *retval)
{   
    int err = 0;
    struct filetable *ft = curthread->t_filetable;
    struct iovec iov;
    struct uio ku;
    char *kernel_buf; // where buf is copied to

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

    kernel_buf = (char *)kmalloc(nbytes+1); // nbytes + 1 because it's 0 terminated. 
    if (kernel_buf == NULL)
        return ENOMEM;
    
     /* set kernel buf to null first */
    for (int i=0; i< (int)nbytes+1; i++) {
        kernel_buf[i] = 0;
    }

    // /* copyin the buffer to kernel buffer */
    // err = copyinstr(buf, kernel_buf, nbytes+1, NULL);
    // if (err) {
    //     kprintf("couldn't copy the buffer in with err %s\n", strerror(err));
    //     kfree(kernel_buf);
    //     return err;
    // }

    struct file_entry *fe = ft->ft_file_entries[fd];
    lock_acquire(fe->fe_lock);
    int how = fe->fe_status & O_ACCMODE;

    /* Check if file is opened for writing */
    if (how != O_WRONLY && how != O_RDWR) {
        lock_release(fe->fe_lock);
        kfree(kernel_buf);
        return EBADF;
    }

    off_t pos = fe->fe_offset;
    uio_uinit(&iov, &ku, buf, nbytes, pos, UIO_WRITE);
    err = VOP_WRITE(fe->fe_vn, &ku);
    if (err) {
        kprintf("%s: Write error: %s\n", fe->fe_filename, strerror(err));
        lock_release(fe->fe_lock);
        kfree(kernel_buf);
        return err;
    }

    *retval = ku.uio_offset - pos; // -1 to ignore the 0 
    // kprintf("successfully wrote to the file and ret val is now %d\n", *retval);

    fe->fe_offset += *retval;
    lock_release(fe->fe_lock);
    kfree(kernel_buf);
    return 0;
}

int
sys_dup2(int oldfd, int newfd, int *retval)
{   
    struct filetable *ft = curthread->t_filetable;
    int result = 0;

    // kprintf("made it to dup2\n");

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