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

#ifndef _VM_H_
#define _VM_H_

#include <machine/vm.h>

/*
 * VM system-related definitions.
 */
#define CM_FREE 0 // page avaialble
#define CM_DIRTY 1 // page not written to disk 
#define CM_CLEAN 2 // page written to disk
#define CM_FIXED 3 // page cannot be freed, used for core data structures

#define PG_TABLE_SIZE PAGE_SIZE/4

#define OUTER_TABLE_INDEX 0xffc00000 /* mask for getting the outer page index from addr */
#define INNER_TABLE_INDEX 0x3ff000 /* mask for getting the inner page index from addr */

// Macros to get outer and inner page index from addr
#define GET_OUTER_TABLE_INDEX(vaddr) (((vaddr) & OUTER_TABLE_INDEX) >> 22)
#define GET_INNER_TABLE_INDEX(vaddr) (((vaddr) & INNER_TABLE_INDEX) >> 12)

/* Lock used for address space functions */
struct lock *vm_lock;

/* Represents physical pages, state could be CM_FREE, CM_DIRTY, CM_CLEANED, CM_FIXED */
struct coremap_entry {
	int status;
};

/* Data structure to keep track of all physical pages and their state
 * All interactions with this data structure must be synchronized
 */
struct coremap {
    struct coremap_entry *cm_entries;
    struct lock *cm_lock;
};

/* Two-layer pagetable style
 * Keeps a mapping of physical pages for a given process 
 */
struct inner_pgtable{
    paddr_t p_addrs[PG_TABLE_SIZE];
};

/* Two-layer pagetable style
 * Keeps a mapping of all inner_pgtable for a given process 
 */
struct outer_pgtable{
    struct inner_pgtable *inner_mapping[PG_TABLE_SIZE];
};

/* Coremap functions */
void coremap_bootstrap(void);
unsigned long get_cm_index(paddr_t pa);


/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/


/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* For details on these functions refer to vm.c */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);
void free_vpage(vaddr_t addr);
paddr_t getppages(unsigned long npages);
paddr_t page_alloc(void);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown_all(void);
void vm_tlbshootdown(const struct tlbshootdown *);

#endif /* _VM_H_ */
