/* BEGIN A4 SETUP */
/*
 * Declarations for file handle and file table management.
 * New for A4.
 */

#ifndef _FILE_H_
#define _FILE_H_

#include <kern/limits.h>
#include <synch.h>

struct vnode;

/*
 * filetable struct
 * just an array, nice and simple.  
 * It is up to you to design what goes into the array.  The current
 * array of ints is just intended to make the compiler happy.
 */
struct filetable {
        int flags;
        off_t offsets;
        int *dups;
        volatile bool ownerships;
        struct vnode *vnodes;
        struct lock *ftlock;
};

/* these all have an implicit arg of the curthread's filetable */
int filetable_init(void);
void filetable_destroy(struct filetable *ft[]);

/* opens a file (must be kernel pointers in the args) */
int file_open(char *filename, int flags, int mode, int *retfd);

/* closes a file */
int file_close(int fd);

/* A4: You should add additional functions that operate on
 * the filetable to help implement some of the filetable-related
 * system calls.
 */
/* scan for availible fd */
int filetable_scan(void);
void fd_init(int);
int fd_dup(int, int, int*retfd);
int filetable_copy(struct filetable *old[], struct filetable *new[]);
int filetable_openfiles(void);

#endif /* _FILE_H_ */

/* END A4 SETUP */
