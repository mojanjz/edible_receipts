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
#include <signal.h>

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
void free_cm_entries(unsigned long start_index, unsigned npages);
struct inner_pgtable * create_inner_pgtable(void);

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
struct coremap *cm;
unsigned long first_page_index;
unsigned long total_num_pages;
bool cm_bootstrapped = false;

/*
 * Bootstraps data structures relevant to the vm such as the coremap
 */ 
void
vm_bootstrap(void)
{
	coremap_bootstrap();
	vm_lock = lock_create("vm_lock");
	if (vm_lock == NULL) {
		panic("Not able to make vm_lock");
	}
}

/*
 * Steal physical pages before the VM has bootsrapped, can't be called after
 * The physical pages won't be tracked by the VM
 * Parameter: npages (number of physical pages required)
 * Returns: the starting physical address of the pages, (pages are contiguous)
 */
paddr_t
getppages(unsigned long npages) 
{

	KASSERT(!cm_bootstrapped); // Shouldn't be able to steal memory after cm has bootstrapped
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
}

/*
 * Allocates pages for the kernel space, should not be used for user allocation. 
 * Calls getppages() if vm has not bootstrapped and page_nalloc() otherwise.
 * Parameters: npages (number of requested pages)
 * Returns: the kernel virtual address that maps to the physical address (pages are contiguous)
 */
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

/* 
 * Frees physical page associated given the kernel virtual address (Kernel pages only)
 * Parameter: addr(virtual address corresponding to the physical address to be freed)
 */
void
free_kpages(vaddr_t addr)
{	
	paddr_t pa = KVADDR_TO_PADDR(addr);
	
	/* Translate physical address into page index */
	unsigned long index = get_cm_index(pa);
	cm->cm_entries[index].status = CM_FREE;
}

/*
 * Free a physical page given the virtual address (User space pages only)
 * Parameter: addr(virtual address to be freed)
 */
void
free_vpage(vaddr_t addr)
{
	struct addrspace *as = proc_getas();

	/* Get the indices of both layers of the page table */
	int inner_page_index = GET_OUTER_TABLE_INDEX(addr);
    int outer_page_index = GET_OUTER_TABLE_INDEX(addr);
	/* If the mapping exists in the page table then free the page */
	if (as->as_pgtable->inner_mapping != NULL && as->as_pgtable->inner_mapping[outer_page_index]->p_addrs[inner_page_index]!= 0) {
		int cm_index = get_cm_index(as->as_pgtable->inner_mapping[outer_page_index]->p_addrs[inner_page_index]);
		free_cm_entries(cm_index, 1);
	}
}

void
vm_tlbshootdown_all(void)
{
	panic("vm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

/*
 *	Handler for when address mapping does not exist in the TLB, called from trap handler
 *	Parameters: faulttype (the action caused the fault, read, write, readonly)
				faultadress (the virtual address for which the fault occured)
	Returns: On success, 0
			 On failure, error code
 */
int
vm_fault(int faulttype, vaddr_t faultaddress)
{	
	paddr_t paddr=0;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	
	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Check if fault address is valid */
	if (faultaddress < (as->as_stackbase) && faultaddress >= (as->as_heapbase + as->as_heapsz)) {
		return SIGSEGV;
	}
		
	faultaddress &= PAGE_FRAME;
	DEBUG(DB_VM, "vm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("vm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		// lock_release(vm_lock);
		return EINVAL;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_stackbase != 0);
	KASSERT(as->as_pgtable != NULL);

	/* Index into the page table */
	int outer_page_index = GET_OUTER_TABLE_INDEX(faultaddress);
	int inner_page_index = GET_INNER_TABLE_INDEX(faultaddress);
	struct inner_pgtable *inner_table = as->as_pgtable->inner_mapping[outer_page_index];

	/* The inner page table exists */
	if (inner_table != NULL) {
		/* The mapping exists */
		if (inner_table->p_addrs[inner_page_index] != 0) {
			paddr = inner_table->p_addrs[inner_page_index];
		}
		/* The mapping does not exist, allocate a page and add the mapping */
		else {
			paddr = page_alloc();
			as->as_pgtable->inner_mapping[outer_page_index]->p_addrs[inner_page_index] = paddr;
		}
	}
	
	/* The inner page table does not exist, create one */
	else {
		as->as_pgtable->inner_mapping[outer_page_index] = create_inner_pgtable();
		if (as->as_pgtable->inner_mapping[outer_page_index] == NULL) {
			return ENOMEM;
		}

		/* Add the page mapping to the page table */
		paddr = page_alloc();
		as->as_pgtable->inner_mapping[outer_page_index]->p_addrs[inner_page_index] = paddr;
	}

	/* make sure the physicial page is valid */
	KASSERT(paddr != 0);
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	bool in_tlb = false;

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}

		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "vm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		in_tlb = true;
		splx(spl);
		return 0;
	}

	/* If TLB was full, randomly delete an entry to make room */
	if (!in_tlb) {
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		tlb_random(ehi, elo);
	}

	splx(spl);
	return 0;
}

/*
 * Creates an inner page table and zeros all the physical address mappings
 * 
 */
struct inner_pgtable *
create_inner_pgtable() {
	struct inner_pgtable *inner_table = (struct inner_pgtable *)kmalloc(sizeof(struct inner_pgtable));
	if (inner_table == NULL) {
		return NULL;
	}

	/* Initialize the entires to be 0 to begin with */
	for (int i = 0; i < PG_TABLE_SIZE; i++) {
		inner_table->p_addrs[i] = 0;

	}

	return inner_table;
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
	paddr_t pa=0;
	lock_acquire(cm->cm_lock);
	for (unsigned long i = first_page_index; i < total_num_pages; i++) {
		if (page_free(i)) {
			// kprintf("got a free page at index %ld\n", i);
			KASSERT(cm->cm_entries[i].status != CM_FIXED);
			cm->cm_entries[i].status = CM_DIRTY;
			pa = get_page_address(i);
			// kprintf("the physical page is %d\n", pa);
			break;
		}
	}
	lock_release(cm->cm_lock);

	if (pa == 0){
		return ENOMEM;
	}

	// TODO ADD FREEING PAGES IF NO PAGE IS AVAILABLE
	bzero((void *)PADDR_TO_KVADDR(pa), PAGE_SIZE);
	return pa;
}

paddr_t page_nalloc(unsigned long npages) {
	unsigned long i = first_page_index;
	bool enough_space = true;
	paddr_t pa = 0;

	if (npages > total_num_pages) {
		return EINVAL;
	}

	if (npages == 1) {
		return page_alloc();
	}

	KASSERT(i+npages <= total_num_pages);

	lock_acquire(cm->cm_lock);
	while (i + npages <= total_num_pages) {
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

	// kprintf("the physical address for nalloc is %d", pa);
	lock_release(cm->cm_lock);
	KASSERT(pa != 0); //TODO: deal with running out of memory in table (probs swapping w disk or moving things around)
	return pa;
}

void free_cm_entries(unsigned long start_index, unsigned npages) {
	for (unsigned long i=start_index; i<start_index+npages; i++) {
		cm->cm_entries[i].status = CM_FREE;
	}
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


