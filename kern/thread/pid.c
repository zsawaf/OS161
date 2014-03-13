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

/*
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <kern/signal.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>

/*
 * Global pid and exit data.
 *
 *
 *
 *
 */

 /* The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids



/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */
	pi->pi_signal = 0;			 /* ASST2: No signal has been received */

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}


/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;

	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so 
 * it can be freed as soon as it exits. May only be called by the
 * parent thread.
 */
int
pid_detach(pid_t childpid)
{
	DEBUG(DB_THREADS, "\npid_detach: parent=%d, child=%d\n",
			curthread->t_pid, childpid);

    struct pidinfo* child;

    KASSERT(curthread->t_pid != INVALID_PID);

    lock_acquire(pidlock);

    /* retrieve info about the child thread */
    child = pi_get(childpid);

    if (child == NULL) {
    	lock_release(pidlock);
        return ESRCH; // child not found
    }

    /* if the child already detached or this thread is not its parent*/
    if (child->pi_ppid == INVALID_PID || child->pi_ppid != curthread->t_pid ) {
    	lock_release(pidlock);
        return EINVAL;
    }

    /* disown the child */
    child->pi_ppid = INVALID_PID;
    if (child->pi_exited) {
    	pi_drop(child->pi_pid); /* child also exited; so clean it up */
    }

    lock_release(pidlock);
	return 0;
}

////////////////////////////////////////////////////////////
/*
 * ASST2 - helper function for pid_exit
 * pi_detach_children: detach all children (if any) of a parent. A thread will
 * call this when it is exiting.
 */
static
void
pi_detach_children(pid_t pid)
{
	struct pidinfo *child = NULL;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	for (int i = 1; i <= PROCS_MAX; ++i) {

		child = pidinfo[i];
		if ((child != NULL) && (child->pi_ppid == pid)) {

			DEBUG(DB_THREADS, "\npi_detach_children: parent=%d, child=%d\n",
					pid, child->pi_pid);

			child->pi_ppid = INVALID_PID; // disown the child

			/* clean ups a dead child whose state is joinable */
			if (child->pi_exited) {
				pi_drop(child->pi_pid); // destroy and remove
			}



		}
	}
}


/*
 * pid_exit - sets the exit status of this thread, disowns children,
 * and wakes any thread waiting for the curthread to exit. Frees 
 * the PID and exit status if the thread has been detached. Must be
 * called only if the thread has had a pid assigned.
 */
void
pid_exit(int status)
{
	DEBUG(DB_THREADS, "\npid_exit: pid=%d, exitcode=%d\n",
			curthread->t_pid, status);

	struct pidinfo *me;
	
	lock_acquire(pidlock);

	me = pi_get(curthread->t_pid);
	KASSERT(me != NULL);
	KASSERT(me->pi_exited == false);

	/* Set the exit status. */
	me->pi_exitstatus = status;
	me->pi_exited = true;

	/* disown children if any */
	pi_detach_children(me->pi_pid);

	/* if no parent, i.e. detached */
	if (me->pi_ppid == INVALID_PID) {
		pi_drop(me->pi_pid);

	} else {
		/* notify parent, if interested */
		cv_signal(me->pi_cv, pidlock);
	}

	lock_release(pidlock);
}

/*
 * pid_join - returns the exit status of the thread associated with
 * childpid as soon as it is available. If the thread has not yet
 * exited, curthread waits unless the flag WNOHANG is sent. 
 * Return: negative values indicates error; zero or positive values for
 * normal exit. status is set to 0 if child does not exit (in the case of
 * WNOHANG).
 *
 */
int
pid_join(pid_t childpid, int *status, int options)
{
	DEBUG(DB_THREADS, "\npid_join: parent=%d, child=%d, wnohang=%d\n",
				curthread->t_pid, childpid, options);

	/* checking unsupported/invalid options */
	if (options != 0 && options != WNOHANG) {
		return -EINVAL;
	}

	/* checking deadlock: joining myself */
	if (childpid == curthread->t_pid) {
		return -EDEADLK;
	}

	struct pidinfo *me;
	struct pidinfo *child;

	lock_acquire(pidlock);

	me = pi_get(curthread->t_pid);
	KASSERT(me != NULL);
	KASSERT(me->pi_exited == false); // better safe than sorrow

	child = pi_get(childpid);

	/* child not found */
	if (child == NULL) {
		lock_release(pidlock);
		return -ESRCH;
	}

	/* child already detached OR not the right parent.
	* Note: change error for this case to ECHILD to match with return
	* requirements of waitpid man page, not EINVAL as specified in A2 spec.
	*/
	if ( child->pi_ppid != me->pi_pid
			|| child->pi_ppid == INVALID_PID ) {
		lock_release(pidlock);
		return -ECHILD;
	}

	/* child still running and parent wants to wait */
	if (options != WNOHANG && child->pi_exited == false ) {
		cv_wait(child->pi_cv, pidlock); // release mutex, sleep, wake up, lock mutex
		KASSERT(child->pi_exited == true);
	}

	/* If we get here, one of the following happens:
	 * 1- If parent doesn't want to wait (WNOHANG) and the child is still
	 * running, return status=0 as required in waitpid's man page.
	 * 2- If the child already exited with an exit status. The parent either
	 * have skipped the wait or been just awaken.
	 */

	if (options == WNOHANG && child->pi_exited == false ) {
		*status = 0;
	} else {
		*status = child->pi_exitstatus;
	}

	lock_release(pidlock);
	return 0;
}

////////////////////////////////////////////////////////////

static void
pi_unset_signal(pid_t pid, int sig) {
	struct pidinfo *target;

	KASSERT(lock_do_i_hold(pidlock));

	target = pi_get(pid);
	KASSERT(target != NULL);
	target->pi_signal &= ~(1 << sig);
}


/**
 * ASST2: helper function for sys_kill.
 * pid_kill generates a signal that will be delivered to a thread.
 * Delivering is done in deliver_signal() in trap.c
 */
int
pid_kill(pid_t pid, int sig) {
	struct pidinfo *target;

	lock_acquire(pidlock);

	target = pi_get(pid);

	if (target == NULL) {
		lock_release(pidlock);
		return ESRCH;
	}

	if (sig == 0) { // just check if pid exit and return
		lock_release(pidlock);
		return 0;
	}

	if (sig > 32 || sig < 1) { // signal numbers start from 1
		lock_release(pidlock);
		return EINVAL;
	}

	if (sig == SIGCONT) {
		DEBUG(DB_THREADS, "\npid_kill: delivering SIGCONT to pid %d.\n", curthread->t_pid);
		// clear the SIGSTOP of the sleeper
		pi_unset_signal(pid, SIGSTOP);
		cv_signal(sleepers, sleeplock);

	} else if (sig == SIGKILL || sig == SIGSTOP
			|| sig == SIGINT || sig == SIGQUIT
			|| sig == SIGWINCH || sig == SIGINFO) {

		DEBUG(DB_THREADS, "\npid_kill: pid=%d, signal=%d\n", pid, sig);
		target->pi_signal |= 1 << sig;

	} else {

		lock_release(pidlock);
		return EUNIMP;
	}

	lock_release(pidlock);
	return 0;
}



/**
 * return the signal(s) that the thread pid has received.
 * return -1 if pid not found.
 */
int pid_get_signal(pid_t pid) {
	struct pidinfo *target;

	lock_acquire(pidlock);

	target = pi_get(pid);
	if (target == NULL) {
		return -1;
	}
	int sig = target->pi_signal;

//	DEBUG(DB_THREADS, "\npid_getsignal: pid=%d, signal=%d\n", pid, sig);

	lock_release(pidlock);
	return sig;
}


void pid_printstats() {
	struct pidinfo *me;

	lock_acquire(pidlock);

	for (int i = 1; i < PROCS_MAX; ++i) {
		me = pidinfo[i];
		if (me != NULL) {
			kprintf("%d.\tpid:%d,\tppid:%d,\texitted:%x,\texitstatus: %d\n",
			i, me->pi_pid, me->pi_ppid, me->pi_exited, me->pi_exitstatus);
		}
	}
	lock_release(pidlock);

}

