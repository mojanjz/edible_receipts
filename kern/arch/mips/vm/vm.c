#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES    18
#define COREMAP_PAGES 2 // TODO: pick the right number

/* Function prototypes */
bool page_free(unsigned long cm_index);
paddr_t get_page_address(unsigned long cm_index);
paddr_t page_alloc(void); 
paddr_t page_nalloc(unsigned long npages);
unsigned long get_cm_index(paddr_t pa);
void coremap_bootstrap_test(void);

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct coremap *cm;
unsigned long first_page_index;
unsigned long total_num_pages;
bool cm_bootstrapped = false;

void
vm_bootstrap(void)
{
	coremap_bootstrap();
}

paddr_t
getppages(unsigned long npages) 
{
	// kprintf("stealing %ld pages\n", npages);
	// KASSERT(!cm_bootstrapped); //Shouldn't be able to steal memory after cm in place
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	paddr_t pa;
	if (cm_bootstrapped) {
		pa =page_nalloc(npages);
	}
	else {
		pa = getppages(npages);
	}
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr)
{	
	(void) addr;
	paddr_t pa = KVADDR_TO_PADDR(addr);
	// Translate physical address to a page index
	unsigned long index = get_cm_index(pa);

	KASSERT(cm->cm_entries[index].status !=CM_FREE);
	cm->cm_entries[index].status = CM_FREE;
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

void coremap_bootstrap_test(void)
{	
	paddr_t pa = ram_getsize();
	paddr_t fa = ram_getfirstfree();
	int max_page = (pa - fa) / PAGE_SIZE;
	kprintf("ram_getsize %d\n", pa);
	kprintf("ram_first_addr %d\n", fa);
	kprintf("# of available pages %d\n", max_page);
}

/* Coremap Functions */
void coremap_bootstrap(void)
{   
    /* Initialize data structures before calling ram functions */
	cm = kmalloc(sizeof(struct coremap));
	kprintf("the size of cm is %d", sizeof(struct coremap));
    cm->cm_lock = lock_create("cm_lock");
	// struct lock *cm_lock = lock_create("cm_lock");
	if(cm->cm_lock == NULL){
		kprintf("couldnt make the lock");
		panic("Couldn't make coremap lock");
	}
	// Stealing memory needed to represent our coremap
	// paddr_t coremap_addr = getppages(COREMAP_PAGES); //TODO: find real size of coremap
	// KASSERT(coremap_addr != 0); //Confirm first address is not zero based on requirements for using PADDR_TO_KVADDR
	// kprintf("The address of the coremap: %d\n", coremap_addr);
	// Get "base and bounds" of our remaining memory
	    
	paddr_t last_addr = ram_getsize(); // Must be called before ram_getfirstfree
	total_num_pages = last_addr / PAGE_SIZE; // Number of pages needed to represent all memory.
	/* Initialize coremap */
	cm->cm_entries = kmalloc(sizeof(struct coremap_entry)*total_num_pages);
	kprintf("the size of the cm_entry array is %ld", sizeof(struct coremap_entry)*total_num_pages);
	
    paddr_t first_addr = ram_getfirstfree(); //Note that calling this function means we can no longer use any functions in ram.c - can only be called once, ram_stealmem will no longer work.

	kprintf("first address %d\n",first_addr);
	kprintf(" and second address %d\n", last_addr);

	KASSERT(last_addr > first_addr);

    unsigned long max_page = (last_addr - first_addr) / PAGE_SIZE; //Should yeild truncated value to never overestimate the number of pages we have the memory for 
	first_page_index = total_num_pages - max_page; 
	kprintf("first page index is %ld", first_page_index);
	kprintf("max page is %ld", max_page);

	
	for (unsigned long i=0; i < total_num_pages; i++) {
		if (i < total_num_pages-max_page) {
			//Make sure that memory used to represent coremap is marked fixed (should never be swapped to disk) 
			cm->cm_entries[i].status = CM_FIXED;	
		} else {
			cm->cm_entries[i].status = CM_FREE;
		}
	}

	cm_bootstrapped = true;
}

paddr_t page_alloc() {
	kprintf("in page_alloc\n");
	paddr_t pa;
	for (unsigned long i = first_page_index; i < total_num_pages; i++) {
		if (page_free(i)) {
			kprintf("got a free page at index %ld\n", i);
			cm->cm_entries[i].status = CM_DIRTY;
			pa = get_page_address(i);
			kprintf("the physcal page is %d\n", pa);
			break;
		}
	}

	// TODO ADD FREEING PAGES IF NO PAGE IS AVAILABLE
	bzero((void *)PADDR_TO_KVADDR(pa), PAGE_SIZE);
	return pa;
}

paddr_t page_nalloc(unsigned long npages) {
	unsigned long i = first_page_index;
	bool enough_space = true;
	paddr_t pa = 0;

	if (npages == 1) {
		return page_alloc();
	}

	kprintf("number of requested pages are %ld\n", npages);

	KASSERT(i+npages <= total_num_pages);

	while (i + npages <= total_num_pages) {
		kprintf("index is %ld", i);
		kprintf("total num pages is %ld", total_num_pages);
		if (page_free(i)) {
			for (unsigned long j = i + 1; j < i + npages; j++) {
				if (!page_free(j)) {
					enough_space = false;
					break;
				}
			}
			if (enough_space) {
				pa = get_page_address(i);
				break;
			}
		}
		i++;
	}

	if (enough_space) {
		//Now update the status of the appropriate coremap entries
		for (unsigned long k = i; k < i + npages; k++) {
			cm->cm_entries[k].status = CM_DIRTY;
		}
		bzero((void *)PADDR_TO_KVADDR(pa), npages * PAGE_SIZE);
	}	
	KASSERT(pa != 0); //TODO: deal with running out of memory in table (probs swapping w disk or moving things around)
	return pa;
}

bool page_free(unsigned long cm_index) {
	return cm->cm_entries[cm_index].status == CM_FREE;
}

paddr_t get_page_address(unsigned long cm_index){
	paddr_t pa;
	pa = (paddr_t) cm_index*PAGE_SIZE;
	return pa;
}

unsigned long get_cm_index(paddr_t pa){
	unsigned long index;
	index =  pa / PAGE_SIZE;
	return index;
}

