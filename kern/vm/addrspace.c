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
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <vm.h>

void as_destroy_pgtable(struct addrspace *as);
void as_copy_inner_pgtable(struct inner_pgtable *old, struct inner_pgtable *new);
void invalidate_tlb(void);

/* For documentation on the following functions see addrspace.h */

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/* Configure stack and heap  */
	as->as_stackbase = USERSTACK - STACK_SIZE;
	as->as_heapbase = 0;
	as->as_heapsz = 0;

	as->as_pgtable = kmalloc(sizeof(struct outer_pgtable));
	if (as->as_pgtable == NULL){
		kfree(as);
		return NULL;
	}
	/* Initialize all inner mappings to initially be NULL */
	for (int i = 0; i < PG_TABLE_SIZE; i++) {
		as->as_pgtable->inner_mapping[i] = NULL; 
	}

	return as;
}

void
as_destroy(struct addrspace *as)
{
	lock_acquire(vm_lock);
	as_destroy_pgtable(as);
	kfree(as);
	lock_release(vm_lock);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);
}

void
as_deactivate(void)
{
	/* Do Nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	lock_acquire(vm_lock);

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	/* Check that segment and heap have not collided  */ 
	vaddr_t end_of_region = vaddr + sz;
	if (end_of_region > as->as_heapbase) {
		/* Get the base of the end of region page */
		vaddr_t page_aligned_eor = 0xfffff000 & end_of_region;
		page_aligned_eor += PAGE_SIZE;
		/* Actually move the heap base */
		as->as_heapbase = page_aligned_eor;
	}

	/* Make sure that heap has not collided into stack */
	KASSERT(as->as_heapbase + as->as_heapsz < as->as_stackbase);
	lock_release(vm_lock);
	return 0;
}


void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	/* Do Nothing */
	(void) as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/* Do Nothing */
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	(void) as;
	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret, pid_t child_pid)
{
	struct proc *child_proc = get_process_from_pid(child_pid);

	KASSERT(child_proc != NULL);
	KASSERT(child_proc->p_pid == child_pid);
	KASSERT(child_proc->p_addrspace == *ret);

	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	lock_acquire(vm_lock);
	/* Copy over values from existing address space */
	new->as_heapbase = old->as_heapbase;
	new->as_heapsz = old->as_heapsz;
	new->as_stackbase = old->as_stackbase;

	for (int i = 0; i < PG_TABLE_SIZE; i++) {
		if (old->as_pgtable->inner_mapping[i] != NULL) {
			new->as_pgtable->inner_mapping[i] = kmalloc(sizeof(struct inner_pgtable));
			if (new->as_pgtable->inner_mapping[i] == NULL){
				lock_release(vm_lock);
				return ENOMEM;
			}
			as_copy_inner_pgtable(old->as_pgtable->inner_mapping[i], new->as_pgtable->inner_mapping[i]);
		}
	}
	invalidate_tlb();

	child_proc = get_process_from_pid(child_pid);
	KASSERT(child_proc != NULL);
	KASSERT(child_proc->p_pid == child_pid);
	KASSERT(child_proc->p_addrspace == *ret);
	
	KASSERT(new->as_heapbase == old->as_heapbase);
	KASSERT(new->as_heapsz == old->as_heapsz);
	KASSERT(new->as_pgtable != NULL);
	
	lock_release(vm_lock);

	*ret = new;
	return 0;
}

void
invalidate_tlb()
{
	int spl = splhigh();
	uint32_t index;

	for (index=0; index<NUM_TLB; index++) {
		tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
	}

	splx(spl);
}

/* --------------------------------------------------------------------------- */

/* 
 *
 */
void
as_destroy_pgtable(struct addrspace *as) {
	for (int i = 0; i < PG_TABLE_SIZE; i++) {
		if (as->as_pgtable->inner_mapping[i] != NULL) {
			kfree(as->as_pgtable->inner_mapping[i]); 
		}
	}
	
}

/* 
 * Iterate over old inner pg table and copy over entries to new one.
 * 
 * Parameters: old (pointer to inner_pgtable to be copied), new (pointer to
 * inner_pgtable to propagate with info)
 * Returns: void
 */
void
as_copy_inner_pgtable(struct inner_pgtable *old, struct inner_pgtable *new)
{
	
	for(int i = 0; i < PG_TABLE_SIZE; i++){
		if (old->p_addrs[i] != 0) {
			/* Allocate a new page */ 
			new->p_addrs[i] = page_alloc();
			memmove((void *)PADDR_TO_KVADDR(new->p_addrs[i]),(const void *)PADDR_TO_KVADDR(old->p_addrs[i]), PAGE_SIZE);
		}
	}
}
