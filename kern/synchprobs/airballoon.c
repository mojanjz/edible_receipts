/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#define N_LORD_FLOWERKILLER 1
#define NROPES 16
static volatile int ropes_left = NROPES;

/* Data structures for rope mappings */
struct rope {
	volatile bool rp_cut;
	int rp_number;
};

/* Initialize array of ropes  and array of pointers to the ropes, stakes */
struct rope ropes[NROPES];
struct rope *stakes[NROPES];

static
bool initialize_data(){

	for (int i=0; i < NROPES; i++){
		struct rope *rp;

		rp = kmalloc(sizeof(struct rope));
		if (rp == NULL) {
			return false;
		}

		rp->rp_number = i;
		rp->rp_cut = false;

		ropes[i] = *rp;
		stakes[i] = &ropes[i];
	}

	return true;
}

/* Synchronization primitives */

/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 */

static
void
dandelion(void *p, unsigned long arg)
{

	(void)arg;

	int index;
	struct lock *ropes_lk = p;

	kprintf("Dandelion thread starting\n");

	while(ropes_left > 0) {
		index = random() % NROPES;

		lock_acquire(ropes_lk);
		if (!ropes[index].rp_cut) {
			ropes[index].rp_cut = true;
			ropes_left--;

			kprintf("Dandelion severed rope %d\n", index);
		}
		lock_release(ropes_lk);
	}

	kprintf("Dandelion thread done\n");
	thread_yield();
}

static
void
marigold(void *p, unsigned long arg)
{
	(void)arg;
	(void)p;

	struct lock *ropes_lk = p;
	int index;

	kprintf("Marigold thread starting\n");

	while(ropes_left > 0) {
		index = random() % NROPES;

		lock_acquire(ropes_lk);
		if (!stakes[index]->rp_cut) {
			stakes[index]->rp_cut = true;
			ropes_left--;

			kprintf("Marigold severed rope %d from stake %d\n", stakes[index]->rp_number,index);
		}
		lock_release(ropes_lk);
	}

	kprintf("Marigold thread done\n");
	thread_yield();
}

static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");

	/* Implement this function */
}

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");

	while(ropes_left>0);
	kprintf("Balloon freed and Prince Dandelion escapes!\n");
	kprintf("Balloon thread done\n");
	thread_yield();
}


// Change this function as necessary
int
airballoon(int nargs, char **args)
{
	(void)nargs;
	(void)args;
	(void)ropes_left;

	int err = 0, i;

	struct lock *ropes_lk;
	ropes_lk = lock_create("ropes-array-lock");

	while(!initialize_data());

	err = thread_fork("Marigold Thread",
			  NULL, marigold, (struct lock *)ropes_lk, 0);
	if(err)
		goto panic;

	err = thread_fork("Dandelion Thread",
			  NULL, dandelion, (struct lock *)ropes_lk, 0);
	if(err)
		goto panic;

	for (i = 0; i < N_LORD_FLOWERKILLER; i++) {
		err = thread_fork("Lord FlowerKiller Thread",
				  NULL, flowerkiller, NULL, 0);
		if(err)
			goto panic;
	}

	err = thread_fork("Air Balloon",
			  NULL, balloon, NULL, 0);
	if(err)
		goto panic;

	goto done;
panic:
	panic("airballoon: thread_fork failed: %s)\n",
	      strerror(err));

done:
	// lock_destroy(ropes_lk);
	return 0;
}
