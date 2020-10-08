/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>
#include <current.h>

#define N_LORD_FLOWERKILLER 8
#define NROPES 16
static volatile int ropes_left = NROPES;

static volatile int active_threads = N_LORD_FLOWERKILLER + 3;

/* Data structures for rope mappings */

/* 
 * Struct that refers to a rope
 * rp_cut: True for severed rope
 *         False for unsevered rope
 * rp_number: Fixed number that refers to the rope
 * rp_lock: Lock associated with each rope
 * This lock is used everytime the rope is accessed by the hook or the stake for read or write
 * This is because read from or write to each rope should be atomic
 */
struct rope {
	volatile bool rp_cut;
	int rp_number;
	struct lock *rp_lock;
};

/* Synchronization primitives */

/* 
 * active_thread_lk is used everytime active_threads is decremented
 * This is used to ensure no race condition will lead to an incorrect value for this variable
 * Reading from this variable however does not require a lock primitive
 */
static struct lock *active_thread_lk;

/* 
 * ropes_lk is used everytime rope_left variable is decremented
 * This is used to ensure that no race condition will lead to an incorrect number of ropes left
 * Reading from ropes_left variable however does not require a lock primitive
 */
static struct lock *ropes_lk;


/* Pointer array of ropes, aka hooks, accessed by Dandelion  */
struct rope *ropes[NROPES];
/* Pointer array that points to the ropes, accessed by Marigold and Flowerkillers  */
volatile struct rope *stakes[NROPES];

/*
 * Initializes the *ropes and *stakes with struct ropes with fixed rp_number and rp_cut set to false
 * rp_cut is initially false for all ropes since no rope is severed
 * Returns: true if initialization is successful
 *          false if initialization failed  
 */
static
bool initialize_data(){

	for (int i=0; i < NROPES; i++){
		struct rope *rp;

		rp = kmalloc(sizeof(struct rope));
		if (rp == NULL) {
			return false;
		}
		/* give each lock an appropriate name */
		char rp_lock_name[10];
		snprintf(rp_lock_name, 10, "rp-lock-%d", i);
		rp->rp_lock = lock_create(rp_lock_name);

		rp->rp_number = i;
		rp->rp_cut = false; // Initially no rope has been severed

		ropes[i] = rp;
		stakes[i] = ropes[i];
	}

	return true;
}

/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 */

/*
 * Dandelion checks ropes_left to ensure not all ropes have been severed
 * It then randomly checks a certain rope and severs it
 * If it has been severed, it does nothing
 * Otherwise it severs the rope, decrements ropes_left
 * Exit condition: All ropes are severed, ropes_left = 0
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

		lock_acquire(ropes[index]->rp_lock);

		/* I was having KASSERT issues where lock_release was being executed by a thread
		 * that was not the owner of the thread. To avoid that, I added lock_do_i_hold to all
		 * threads that use the locks on the ropes. I'm guessing somehow a race condition causes this
		 * thread to be run non atomically as soon as flowerkiller is introduced
		 */
		if(!lock_do_i_hold(ropes[index]->rp_lock)) {
			goto yield_thread;
		}

		if (!ropes[index]->rp_cut) {
			ropes[index]->rp_cut = true;
			kprintf("Dandelion severed rope %d\n", index);
			lock_release(ropes[index]->rp_lock);
	
			lock_acquire(ropes_lk);
			ropes_left--;  // changing ropes_left must be atomic
			lock_release(ropes_lk);
		}
		else
			lock_release(ropes[index]->rp_lock);

yield_thread:
		thread_yield();
	}

	kprintf("Dandelion thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

/*
 * Marigold checks ropes_left for any unsevered rope
 * It then randomly checks a certain rope from the stakes array
 * if it has been severed, it does nothing
 * otherwise it severs the rope and decrements ropes_left
 * Exit condition: All ropes are severed, ropes_left = 0
 */
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


		lock_acquire(stakes[index]->rp_lock);
		/* Added to avoid KASSERT error on lock release similar to the comment in Dandelion */
		if (!lock_do_i_hold(stakes[index]->rp_lock))
			goto yield_thread;

		if (!stakes[index]->rp_cut) {
			stakes[index]->rp_cut = true;
			kprintf("Marigold severed rope %d from stake %d\n", stakes[index]->rp_number,index);
			lock_release(stakes[index]->rp_lock);

			lock_acquire(ropes_lk);
			ropes_left--;
			lock_release(ropes_lk);
		}
		else
			lock_release(stakes[index]->rp_lock);
yield_thread:
		thread_yield();
	}

	kprintf("Marigold thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

/*
 * Switches ropes attached to two given stakes in *stakes
 */
static
void
switch_ropes(int stake_index_1, int stake_index_2){
	struct rope temp_rope = *stakes[stake_index_1];
	*stakes[stake_index_1] = *stakes[stake_index_2];
	*stakes[stake_index_2] = temp_rope;
}

/*
 * Flowerkiller randomly picks two stakes, checks if the ropes are severed
 * If at least one of them is severed, it does nothing
 * Otherwise, it locks both ropes and switches the stakes
 * The locks are necessary since Marigold accesses ropes via stakes
 * Exit condition: At least two ropes are left to be switched
 */
static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	int sk_index_1, sk_index_2; // index of two stakes to be swapped

	kprintf("Lord FlowerKiller thread starting\n");

	while(ropes_left > 1){
		sk_index_1 = random() % NROPES;
		sk_index_2 = random() % NROPES;
		/* The second condition is added to avoid a deadlock situation where two flowerkiller threads are waiting on each others lock */
		if (sk_index_1 != sk_index_2 && stakes[sk_index_1]->rp_number < stakes[sk_index_2]->rp_number) { // optimization, don't switch if the two indices are the same
			lock_acquire(stakes[sk_index_1]->rp_lock);
			lock_acquire(stakes[sk_index_2]->rp_lock);

			/* Check if ropes are severed, if yes yield, if no swap them */
			if (!stakes[sk_index_1]->rp_cut && !stakes[sk_index_1]->rp_cut) {
				/* Switch ropes */
				switch_ropes(sk_index_1, sk_index_2);
				kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[sk_index_2]->rp_number, sk_index_1, sk_index_2);
				kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[sk_index_1]->rp_number, sk_index_2, sk_index_1);

				lock_release(stakes[sk_index_1]->rp_lock);
				lock_release(stakes[sk_index_2]->rp_lock);
				goto yield_thread;

			} 
			else {
				lock_release(stakes[sk_index_2]->rp_lock);
				lock_release(stakes[sk_index_1]->rp_lock);
				kprintf("second lock released %d\n", stakes[sk_index_1]->rp_number);
			}
		}
yield_thread:		
		thread_yield();
	}
	kprintf("Lord FlowerKiller thread done\n");
	lock_acquire(active_thread_lk);
	active_threads--;
	lock_release(active_thread_lk);
	thread_yield();
}

/*
 * Balloon just waits for Marigold and Dandelion to sever all the ropes
 * Exit condition: All ropes are severed, ropes_left = 0
 */
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

/*
 * Airballon creates ropes_lk and active_thread_lk, initializes data and runs all threads
 * Handles errors in case the threads fail
 * Clears rope structs after all threads are done
 * Exit condtion: All threads are done, active_thread = 0
 */
int
airballoon(int nargs, char **args)
{
	(void)nargs;
	(void)args;
	(void)ropes_left;

	int err = 0, i;

	ropes_lk = lock_create("ropes-array-lock"); // TODO
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
	/* Destroy all locks in use after all threads are done */
	lock_destroy(ropes_lk);
	lock_destroy(active_thread_lk);

	for (int i =0; i < NROPES; i++) {
		lock_destroy(ropes[i]->rp_lock);
	}

	/* Free up kernel memory after all threads are done */
	for (int i = 0; i < NROPES; i++) {
		KASSERT(ropes[i] != NULL);
		kfree(ropes[i]);
	}

	kprintf("Main thread done\n");
	return 0;
}
