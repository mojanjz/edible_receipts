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
#include <uio.h>
#include <vfs.h>
#include <limits.h>
#include <kern/errno.h>
#include <copyinout.h>


/* 
 * Gets the name of the current working directory and stores it in buf.
 * 
 * Parameters: buf (pointer to where cwd should be stored), buflen (size of buf),
 * pointer to address of return val.
 * Returns: On succes, the length of data stored (non-negative)
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{   
    struct iovec user_iov;
    struct uio user_uio;

    uio_uinit(&user_iov, &user_uio, buf, buflen, 0, UIO_READ);
    int result = vfs_getcwd(&user_uio);
   
   if (result) {
       return result;
   }

    *retval = buflen - user_uio.uio_resid;
    return 0;
}
/* 
 * Changes the current directory
 * 
 * Parameters: pathname (the name of directory that current process is to be set to)
 * Returns: On succes, 0.  On failure, -1 and errno set.
 */
int
sys_chdir(userptr_t pathname)
{  
    int result = 0;
    char *k_pathname;

    k_pathname = (char *)kmalloc(__PATH_MAX);
    if (k_pathname == NULL)
        return ENOMEM;
    
    /* set k_pathname to null initially */
    for (int i=0; i< __PATH_MAX; i++) {
        k_pathname[i] = '\0';
    }

    result = copyinstr(pathname, k_pathname, __PATH_MAX, NULL);
    if (result) {
        kfree(k_pathname);
        return result;
    }

    result = vfs_chdir(k_pathname);
    kfree(k_pathname);
    return result;
}