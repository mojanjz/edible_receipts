/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#define N_LORD_FLOWERKILLER 8
#define NROPES 16
static volatile int ropes_left = NROPES;

static volatile int active_threads = N_LORD_FLOWERKILLER + 3;

/* Data structures for rope mappings */
struct rope {
	volatile bool rp_cut;
	int rp_number;
};

/* Initialize array of ropes, hooks, and array of pointers to the ropes, stakes */
struct rope *ropes[NROPES];
/* volatile since multiple flowekiller threads can access the stakes array */
volatile struct rope *stakes[NROPES];

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

		ropes[i] = rp;
		stakes[i] = ropes[i];
	}

	return true;
}

/* Synchronization primitives */
static struct lock *active_thread_lk; // lock for locking the active_threads variable
static struct lock *ropes_lk; // lock for accessing the ropes

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
	(void)p;

	int index;

	kprintf("Dandelion thread starting\n");

	while(ropes_left > 0) {
		index = random() % NROPES;

		lock_acquire(ropes_lk);
		if (!ropes[index]->rp_cut) {
			ropes[index]->rp_cut = true;
			kprintf("Dandelion severed rope %d\n", index);
			ropes_left--;
		}
		lock_release(ropes_lk);
	}

	kprintf("Dandelion thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

static
void
marigold(void *p, unsigned long arg)
{
	(void)arg;
	(void)p;

	int index;

	kprintf("Marigold thread starting\n");

	while(ropes_left > 0) {
		index = random() % NROPES;

		lock_acquire(ropes_lk);
		if (!stakes[index]->rp_cut) {
			stakes[index]->rp_cut = true;
			kprintf("Marigold severed rope %d from stake %d\n", stakes[index]->rp_number,index);
			ropes_left--;
		}
		lock_release(ropes_lk);
	}

	kprintf("Marigold thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

static
void
switch_ropes(int stake_index_1, int stake_index_2){
	struct rope temp_rope = *stakes[stake_index_1];
	*stakes[stake_index_1] = *stakes[stake_index_2];
	*stakes[stake_index_2] = temp_rope;
}

static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	// the index of the stakes that will be swapped
	int sk_index_1, sk_index_2;

	kprintf("Lord FlowerKiller thread starting\n");

get_random_index:
	while(ropes_left > 1){
		sk_index_1 = random() % NROPES;
		sk_index_2 = random() % NROPES;

		if (sk_index_1 != sk_index_2) {
			lock_acquire(ropes_lk);
			/* check if ropes are severed */
			if (stakes[sk_index_1]->rp_cut && stakes[sk_index_1]->rp_cut) {
				lock_release(ropes_lk);
				goto get_random_index;
			}
			/* switch ropes */
			switch_ropes(sk_index_1, sk_index_2);
			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[sk_index_2]->rp_number, sk_index_1, sk_index_2);
			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[sk_index_1]->rp_number, sk_index_2, sk_index_1);
			lock_release(ropes_lk);
		}
		else
			goto get_random_index;
		
	}

	kprintf("Lord FlowerKiller thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");

	while(ropes_left > 0);

	kprintf("Balloon freed and Prince Dandelion escapes!\n");
	kprintf("Balloon thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
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

	ropes_lk = lock_create("ropes-array-lock");
	active_thread_lk = lock_create("active-thread-lock");

	while(!initialize_data());

	err = thread_fork("Marigold Thread",
			  NULL, marigold, NULL, 0);
	if(err)
		goto panic;

	err = thread_fork("Dandelion Thread",
			  NULL, dandelion, NULL, 0);
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
	while(active_threads > 0);
	kprintf("I'm stuck here\n");
	lock_destroy(ropes_lk);
	lock_destroy(active_thread_lk);

	/* free up rope variables */
	for (int i = 0; i < NROPES; i++) {
		KASSERT(ropes[i] != NULL);
		kfree(ropes[i]);
	}


	kprintf("Main thread done\n");
	return 0;
}
 /* 
 move ropes lock to the top
stakes has to be volatile
 lock shared counter
 more fine grained lock??
 clear out the kernel and variables
 */