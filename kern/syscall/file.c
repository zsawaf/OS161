/* BEGIN A4 SETUP */
/*
 * File handles and file tables.
 * New for ASST4
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/unistd.h>
#include <file.h>
#include <syscall.h>

#include <current.h>
#include <lib.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <copyinout.h>

#include "thread.h"
#include <vnode.h>

/*** openfile functions ***/

/*
 * file_open
 * opens a file, places it in the filetable, sets RETFD to the file
 * descriptor. the pointer arguments must be kernel pointers.
 * NOTE -- the passed in filename must be a mutable string.
 * 
 * A4: As per the OS/161 man page for open(), you do not need 
 * to do anything with the "mode" argument.
 */
int
file_open(char *filename, int flags, int mode, int *retfd)
{
    (void)mode;
        int fd;
        int result;
        char path[5];
        struct stat stats;
        int do_append = 0;

        fd = filetable_scan();
        if (fd < 3){
            return EMFILE;
        }
        fd_init(fd);
        KASSERT(curthread->t_filetable[fd] != NULL);
       
        strcpy(path, filename);
        
        //check for append, set offset to end of file
        if (flags & O_APPEND){
            do_append = 1;
            flags = flags & O_ACCMODE;
        }
        result = vfs_open(path, flags, 0, &curthread->t_filetable[fd]->vnodes);
        if (result){
            return result;
        }
        curthread->t_filetable[fd]->ftlock = lock_create("ftlock");
        if (curthread->t_filetable[fd]->ftlock == NULL){
            vfs_close(curthread->t_filetable[fd]->vnodes);
            kfree(curthread->t_filetable[fd]);
            curthread->t_filetable[fd] = NULL;
            return ENOMEM;
        }
        if (do_append){   
            result = VOP_STAT(curthread->t_filetable[fd]->vnodes, &stats);
            if (result){
                return result;
            }
            KASSERT(&stats != NULL);
            curthread->t_filetable[fd]->offsets = stats.st_size;
        }
        //kprintf("OPEN offset %lu\n", (unsigned long)curthread->t_filetable[fd]->offsets);
        curthread->t_filetable[fd]->flags = flags & O_ACCMODE;
        KASSERT(curthread->t_filetable[fd]->flags == O_RDONLY || 
                curthread->t_filetable[fd]->flags == O_WRONLY ||
                curthread->t_filetable[fd]->flags == O_RDWR);
        *retfd = fd;
        return 0;
}

/* 
 * file_close
 * Called when a process closes a file descriptor.  Think about how you plan
 * to handle fork, and what (if anything) is shared between parent/child after
 * fork.  Your design decisions will affect what you should do for close.
 */
int
file_close(int fd)
{
        if (fd < 0 || fd >= __OPEN_MAX){
                return EBADF;
        }
        if (curthread->t_filetable[fd] == NULL){
                return EBADF;
        }
        lock_acquire(curthread->t_filetable[fd]->ftlock);
        if (*curthread->t_filetable[fd]->dups > 0){
            *curthread->t_filetable[fd]->dups = *curthread->t_filetable[fd]->dups - 1;
            lock_release(curthread->t_filetable[fd]->ftlock);
            curthread->t_filetable[fd] = NULL;    
        }else if(*curthread->t_filetable[fd]->dups == 0){
            vfs_close(curthread->t_filetable[fd]->vnodes);
            lock_release(curthread->t_filetable[fd]->ftlock);
            lock_destroy(curthread->t_filetable[fd]->ftlock);
            kfree(curthread->t_filetable[fd]);
            curthread->t_filetable[fd] = NULL;
        }
	return 0;
}

/*** filetable functions ***/

/* 
 * filetable_init
 * pretty straightforward -- allocate the space, set up 
 * first 3 file descriptors for stdin, stdout and stderr,
 * and initialize all other entries to NULL.
 * 
 * Should set curthread->t_filetable to point to the
 * newly-initialized filetable.
 * 
 * Should return non-zero error code on failure.  Currently
 * does nothing but returns success so that loading a user
 * program will succeed even if you haven't written the
 * filetable initialization yet.
 */

int
filetable_init(void)
{
    int result;
    char path[5];
    KASSERT(curthread->t_filetable[0] == 0 && curthread->t_filetable[1] == 0 
            && curthread->t_filetable[2] == 0);
    
    /*Ser every filetable entry to NULL*/
    for (int i = 3; i < __OPEN_MAX; i++){
        curthread->t_filetable[i] = NULL;
    }
    fd_init(0);
    fd_init(1);
    fd_init(2);
    if (curthread->t_filetable[0] == NULL || curthread->t_filetable[1] == NULL 
            || curthread->t_filetable[2] == NULL ){
        return EBADF;
    }
    strcpy(path, "con:");
    result = vfs_open(path, O_RDONLY, 0, &curthread->t_filetable[0]->vnodes);
    if (result){
        return result;
    }
    strcpy(path, "con:");
    result = vfs_open(path, O_WRONLY, 0, &curthread->t_filetable[1]->vnodes);
    if (result){
        return result;
    }
    strcpy(path, "con:");
    result = vfs_open(path, O_WRONLY, 0, &curthread->t_filetable[2]->vnodes);
    if (result){
        return result;
    }
    curthread->t_filetable[0]->flags = O_RDONLY;
    curthread->t_filetable[1]->flags = O_WRONLY;
    curthread->t_filetable[2]->flags = O_WRONLY;

    return 0;
}	

/*
 * filetable_destroy
 * closes the files in the file table, frees the table.
 * This should be called as part of cleaning up a process (after kill
 * or exit).
 */
void
filetable_destroy(struct filetable *ft[])
{
    int fd = 0;
    while (fd <= __OPEN_MAX){
        if (ft[fd] != NULL){
            lock_acquire(ft[fd]->ftlock);
        if (*ft[fd]->dups > 0){
            *ft[fd]->dups = *ft[fd]->dups - 1;
            lock_release(ft[fd]->ftlock);
        }else if(*ft[fd]->dups == 0){
            vfs_close(ft[fd]->vnodes);
            lock_release(ft[fd]->ftlock);
            lock_destroy(ft[fd]->ftlock);
            kfree(ft[fd]);
        }
        }
        fd++;
    }
}	


/* 
 * You should add additional filetable utility functions here as needed
 * to support the system calls.  For example, given a file descriptor
 * you will want some sort of lookup function that will check if the fd is 
 * valid and return the associated vnode (and possibly other information like
 * the current file position) associated with that open file.
 */


void 
fd_init(int fd)
{
    KASSERT(curthread->t_filetable[fd] == NULL);
    curthread->t_filetable[fd] = kmalloc(sizeof(struct filetable) );
    int *dup = kmalloc(sizeof(int));
    *dup = 0;
    curthread->t_filetable[fd]->dups = dup;
    curthread->t_filetable[fd]->offsets = 0;
    curthread->t_filetable[fd]->ftlock = lock_create("ftlock");
    KASSERT(curthread->t_filetable[fd] != NULL);
}

int
fd_dup(int oldfd, int newfd, int *retval)
{
    if (newfd < 0 || newfd >= __OPEN_MAX || oldfd < 0 || oldfd >= __OPEN_MAX){
        return EBADF;
    }
    if (curthread->t_filetable[oldfd] == NULL){
        return EBADF;
    }
    if (newfd == oldfd){
        return 0;
    }
    if (curthread->t_filetable[newfd] != NULL){
        file_close(newfd);
    }
    lock_acquire(curthread->t_filetable[oldfd]->ftlock);
    *curthread->t_filetable[oldfd]->dups = *curthread->t_filetable[oldfd]->dups + 1;
    lock_release(curthread->t_filetable[oldfd]->ftlock);
    fd_init(newfd);
    if (curthread->t_filetable[newfd] == NULL){
        return ENOMEM;
    }
    curthread->t_filetable[newfd]->dups = curthread->t_filetable[oldfd]->dups;
    curthread->t_filetable[newfd]->flags = curthread->t_filetable[oldfd]->flags;
    curthread->t_filetable[newfd]->vnodes = curthread->t_filetable[oldfd]->vnodes;
    curthread->t_filetable[newfd]->offsets = curthread->t_filetable[oldfd]->offsets;
    *retval = newfd;
    return 0;
}


int
filetable_copy(struct filetable *old[], struct filetable *new[])
{
    int i = 0;
    while (i < __OPEN_MAX){

        if (old[i] != NULL){
                lock_acquire(curthread->t_filetable[i]->ftlock);
                *old[i]->dups = *old[i]->dups + 1;
                lock_release(curthread->t_filetable[i]->ftlock);
                new[i] = kmalloc(sizeof(struct filetable));
                new[i]->vnodes = old[i]->vnodes;
                new[i]->offsets = old[i]->offsets;
                new[i]->flags = old[i]->flags;
                new[i]->dups = old[i]->dups;
                new[i]->ftlock = old[i]->ftlock;
                DEBUG(DB_SFS, "DUPS %d\n", *old[i]->dups);
        }
        i++;
    }
    return i;
}

/* 
 * filetable_scan
 * Walk a filetable searching for the lowest empty position.  Else, return -1
 * __OPEN_MAX reached for process.
 */
int
filetable_scan()
{
    int pos = 3;
    while (pos <= __OPEN_MAX){
        if (curthread->t_filetable[pos] == NULL){
            return pos;
        }
        pos++;
    }
    return -1;
}
/* 
 * filetable_openfiles
 * Return true if there is at least one open file in the curthreads filetable.
 */
int
filetable_openfiles(){
    for (int i = 0; i < __OPEN_MAX; i++){
        if (curthread->t_filetable[i] != NULL){
            return 1;
        }
    }
    return 0;
}
/* END A4 SETUP */
