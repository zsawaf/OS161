/*
 * killtest - test kill().
 *
 * This should work correctly when SIGKILL is implemented for kill().
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>

/*
 * Helper function for fork that prints a warning on error.
 */
static
int
dofork(void)
{
	int pid;
	pid = fork();
	if (pid < 0) {
		warn("fork failed.");
	}
	if (pid == 0) {
		long long z = 20000LL;
//		int pid2ch = 48 + getpid();
		while (1) {
			--z;
			if (z==0) {
				putchar('.');
//				putchar(pid2ch);
				z = 20000LL;
			}
		}
	}
	return pid;
}

/*
 * Actually run the test.
 */
static
void
test()
{
	int pid0, pid1, ret;

	pid0 = dofork();
	warnx("Child 0 created.");
	pid1 = dofork();
	warnx("Child 1 created.");

	ret = kill(pid1, 9);
	if (ret == -1) {
		warn("kill failed.");
	}
	else {
		warnx("Child 1 killed.");
	}
	ret = kill(pid0, 9);
	if (ret == -1) {
		warn("kill failed.");
	}
	else {
		warnx("Child 0 killed.");
	}
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
	warnx("Starting.");

	test();

	warnx("Complete.");
	return 0;
}
