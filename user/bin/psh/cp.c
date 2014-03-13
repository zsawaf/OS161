#include "psh.h"

/*
 * cp - copy a file.
 * Usage: cp oldfile newfile
 */


/* Copy one file to another. */
static
void
copy(const char *from, const char *to)
{
	int fromfd;
	int tofd;
	char buf[1024];
	int len, wr, wrtot;

	/*
	 * Open the files, and give up if they won't open
	 */
	fromfd = open(from, O_RDONLY);
	if (fromfd<0) {
		warn("cp - %s", from);
		return;
	}
	tofd = open(to, O_WRONLY|O_CREAT|O_TRUNC);
	if (tofd<0) {
		warn("cp - %s", to);
		close(fromfd);
		return;
	}

	/*
	 * As long as we get more than zero bytes, we haven't hit EOF.
	 * Zero means EOF. Less than zero means an error occurred.
	 * We may read less than we asked for, though, in various cases
	 * for various reasons.
	 */
	while ((len = read(fromfd, buf, sizeof(buf)))>0) {
		/*
		 * Likewise, we may actually write less than we attempted
		 * to. So loop until we're done.
		 */
		wrtot = 0;
		while (wrtot < len) {
			wr = write(tofd, buf+wrtot, len-wrtot);
			if (wr<0) {
				warn("cp - %s", to);
				close(fromfd);
				close(tofd);
				return;
			}
			wrtot += wr;
		}
	}
	/*
	 * If we got a read error, print it and exit.
	 */
	if (len<0) {
		warn("cp - %s", from);
		close(fromfd);
		close(tofd);
		return;
	}

	if (close(fromfd) < 0) {
		warn("cp - %s: close", from);
		close(tofd);
		return;
	}

	if (close(tofd) < 0) {
		warn("cp - %s: close", to);
		return;
	}
}

int
cmd_cp(int argc, char *argv[])
{
	/*
	 * Just do it.
	 *
	 * We don't allow the Unix model where you can do
	 *    cp file1 file2 file3 destination-directory
	 *
	 * although this would be pretty easy to add.
	 */
	if (argc!=3) {
		warnx("Usage: cp OLDFILE NEWFILE");
		return 0;
	}
	copy(argv[1], argv[2]);
	return 0;
}
