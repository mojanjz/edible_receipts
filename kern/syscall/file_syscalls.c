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

int
sys_open(userptr_t filename, int flags, mode_t mode, int *retval)
{
    int err = 0;
    char *kernel_filename;

    kprintf("made it to sys_open\n");

    kernel_filename = (char *)kmalloc(__PATH_MAX);
    if (kernel_filename == NULL)
        return ENOMEM;

    kprintf("sys_open: allocated kernel space for file name\n");
    /* copyin the filename */
    err = copyinstr(filename, kernel_filename, __PATH_MAX, NULL);
    if (err) {
        kfree(kernel_filename);
        return err;
    }

    kprintf("sys_open: got the filename %s\n", kernel_filename);

    err = file_open(kernel_filename, flags, mode, retval);

    kfree(kernel_filename);
    return 0;
}

int
sys_lseek(int fd, int higher_pos, int lower_pos, int whence, int *retval)
{
    (void)fd;
    (void)higher_pos;
    (void)lower_pos;
    (void)whence;
    (void)retval;

    // off_t pos = tf->tf_a2 << 32 | tf->tf_a3;
    // int whence;
    // size_t size = sizeof(whence);
    // copyin((const_userptr_t) tf->tf_sp+16, *whence, size);
    return 4;
}

int
sys_write(int fd, userptr_t buf, size_t nbytes, int *retval)
{   
    int err = 0;
    struct filetable *ft = curthread->t_filetable;
    struct iovec iov;
    struct uio ku;
    char *kernel_buf; // where buf is copied to

    kprintf("made it to sys_write\n");

    /* Check for invalid file descriptor or unopened files */

    if(fd < 0 || fd > __OPEN_MAX-1 || ft->ft_file_entries[fd]->fe_vn == NULL ) {
        return EBADF;
    }

    kprintf("made it to sys_write\n");

    struct file_entry *fe = ft->ft_file_entries[fd];
    int how = fe->fe_status & O_ACCMODE;

    /* File has not been opened for writing */
    if (how != O_WRONLY || how != O_RDWR) {
        return EBADF;
    }

    kernel_buf = (char *)kmalloc(__PATH_MAX);
    if (kernel_buf == NULL)
        return ENOMEM;

    kprintf("sys_write: flags weren't bad, file was open\n");
    /* copyin the buffer to kernel buffer */
    err = copyinstr(buf, kernel_buf, nbytes, NULL);
    if (err) {
        kfree(kernel_buf);
        return err;
    }

    off_t pos = fe->fe_offset;
    uio_kinit(&iov, &ku, kernel_buf, nbytes, pos, UIO_WRITE);
    err = VOP_WRITE(fe->fe_vn, &ku);
    if (err) {
        kprintf("%s: Write error: %s\n", fe->fe_filename, strerror(err));
        return err;
    }

    *retval = ku.uio_offset - pos;

    fe->fe_offset += *retval;
    return 0;
}