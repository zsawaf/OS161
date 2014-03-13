/*
 * Process-related syscalls.
 * New for A2.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <pid.h>
#include <machine/trapframe.h>
#include <syscall.h>
#include <kern/wait.h>
#include <copyinout.h>

/*
 * sys_fork
 * 
 * create a new process, which begins executing in md_forkentry().
 */


int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct trapframe *ntf; /* new trapframe, copy of tf */
	int result;

	/*
	 * Copy the trapframe to the heap, because we might return to
	 * userlevel and make another syscall (changing the trapframe)
	 * before the child runs. The child will free the copy.
	 */

	ntf = kmalloc(sizeof(struct trapframe));
	if (ntf==NULL) {
		return ENOMEM;
	}
	*ntf = *tf; /* copy the trapframe */

	result = thread_fork(curthread->t_name, enter_forked_process, 
			     ntf, 0, retval);
	if (result) {
		kfree(ntf);
		return result;
	}

	return 0;
}

/*
 * sys_getpid
 * return the id of the current thread
 */

int sys_getpid(int *retval) {
	*retval = curthread->t_pid;
    return 0;
}


/*
 * sys_waitpid
 * return the child's encoded exit/signal status in status.
 * On success, return the given pid. On failure, return one of the below:
 * 	ESRCH: 'pid' is a nonexistent process
 * 	EINVAL: 'options' is invalid
 * 	ECHILD: 'pid' is not a child or already detached
 *	EDEADLK: deadlock is avoided. waiting for self.
 *	EFAULT: 'status' is an invalid pointer
 *
 */
int sys_waitpid(pid_t pid, userptr_t status, int options, int *retval) {
	int localstatus;
	int result;

	result = thread_join(pid, &localstatus, options);
	if (result < 0) { // one of ESRCH, EINVAL, ECHILD or EDEADLK
		return -result; // must be positive values as per waitpid's contract
	}

	/* copy 'status' to user space.
	 * If status is an invalid pointer, return EFAULT. On success return 0. */
	result = copyout(&localstatus, status, sizeof(int));

	*retval = pid;

	return result;
}


/*
 * sys_kill
 */
int sys_kill(pid_t pid, int signum, int* retval) {
	(void)retval;
	return pid_kill(pid, signum);
}



