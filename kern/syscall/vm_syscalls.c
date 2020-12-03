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
#include <syscall.h>
#include <kern/errno.h>
#include <lib.h>
#include <current.h>
#include <proc.h>
#include <addrspace.h>
#include <vm_syscalls.h>
#include <vm.h>

void free_heap(vaddr_t start, vaddr_t stop);

/*
 * Adjust the end of the heap (break) by a certain amount.  Note that if amount is a negative amount that
 * the memory removed from the heap will be freed.
 * 
 * Parameters: amount (the amount by which to adjust the end of the heap by), retval (the pointer to the return value 
 * address)
 * Returns: the previous value of the end of the heap
 */
int
sys_sbrk(ssize_t amount, int *retval)
{
    struct addrspace *as = curproc->p_addrspace;
    if (as == NULL) {
        return ENOMEM;
    }
    /* Do checks to make sure that this new region is valid */
    /* First, make sure that the amount to increase is rounded to the nearest page */
    if (amount % PAGE_SIZE != 0) {
        amount += (PAGE_SIZE - (amount%PAGE_SIZE));
    }
    /* Make sure that if amount is negative, that it is a valid value */
    if (as->as_heapbase + as->as_heapsz + amount < as->as_heapbase) {
        return EINVAL;
    }
    /* Make sure that heap doesn't crash into stack */ 
    if (as->as_heapbase + as->as_heapsz >= as->as_stackbase) {
        return ENOMEM;
    }
    /*  Having concluded that amount is valid: */
    if (amount < 0) {
        /* Free all the pages that the heap will no longer contain */
        free_heap(as->as_heapbase + as->as_heapsz + amount, as->as_heapbase + as->as_heapsz);
    }

    /* Store the current (unchanged) value of break/end address of heap region */
    *retval = (int)(as->as_heapbase + as->as_heapsz);
    as->as_heapsz = as->as_heapsz + amount;

    return 0; 
}

/*
 * This function frees the physical memory in the vaddr range [start,stop]
 * 
 * Parameters: start (the address at which to start freeing memory), stop (the address at which to stop
 * freeing memory - this will be the last addrs freed)
 * Returns: void
 */
void free_heap(vaddr_t start, vaddr_t stop)
{
    vaddr_t free_addr = start;
    int n = 0;
    while (free_addr <= stop) {
        free_addr += PAGE_SIZE * n;
        free_vpage(free_addr);
        n += 1;
    }
}