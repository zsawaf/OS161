/* BEGIN A4 SETUP */
/* This file existed for A2 and A3, but has been completely replaced for A4.
 * We have kept the dumb versions of sys_read and sys_write to support early
 * testing, but they should be replaced with proper implementations that
 * use your open file table to find the correct vnode given a file descriptor
 * number.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <vfs.h>
#include <vnode.h>
#include <uio.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <copyinout.h>
#include <synch.h>
#include <file.h>

/* This special-case for the console vnode should go away with a proper
 * open file table implementation.
 */

struct vnode *cons_vnode=NULL;

void dumb_consoleIO_bootstrap()
{
  int result;
  char path[5];

  /* The path passed to vfs_open must be mutable.
   * vfs_open may modify it.
   */

  strcpy(path, "con:");
  result = vfs_open(path, O_RDWR, 0, &cons_vnode);

  if (result) {
    /* Tough one... if there's no console, there's not
     * much point printing a warning...
     * but maybe the bootstrap was just called in the wrong place
     */
    kprintf("Warning: could not initialize console vnode\n");
    kprintf("User programs will not be able to read/write\n");
    cons_vnode = NULL;
  }
}

/*
 * mk_useruio
 * sets up the uio for a USERSPACE transfer.
 */
static
void
mk_useruio(struct iovec *iov, struct uio *u, userptr_t buf,
       size_t len, off_t offset, enum uio_rw rw)
{

    iov->iov_ubase = buf;
    iov->iov_len = len;
    u->uio_iov = iov;
    u->uio_iovcnt = 1;
    u->uio_offset = offset;
    u->uio_resid = len;
    u->uio_segflg = UIO_USERSPACE;
    u->uio_rw = rw;
    u->uio_space = curthread->t_addrspace;
}

/*
 * sys_open
 * just copies in the filename, then passes work to file_open.
 * You have to write file_open.
 *
 */
int
sys_open(userptr_t filename, int flags, int mode, int *retval)
{
    char *fname;
    int result;

    if ( (fname = (char *)kmalloc(__PATH_MAX)) == NULL) {
        return ENOMEM;
    }

    result = copyinstr(filename, fname, __PATH_MAX, NULL);
    if (result) {
        kfree(fname);
        return result;
    }

    result =  file_open(fname, flags, mode, retval);
    kfree(fname);
    return result;
}

/*
 * sys_close
 * You have to write file_close.
 */
int
sys_close(int fd)
{
    return file_close(fd);
}

/*
 * sys_dup2
 *
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{
    int result;
    result = fd_dup(oldfd, newfd, retval);
    if (result){
        return result;
    }
    *retval = newfd;
    return 0;
}

/*
 * sys_read
 * calls VOP_READ.
 *
 * A4: This is the "dumb" implementation of sys_write:
 * it only deals with file descriptors 1 and 2, and
 * assumes they are permanently associated with the
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
    struct uio user_uio;
    struct iovec user_iov;
    int result;

    /* better be a valid file descriptor */
    if (fd < 0 || fd >= __OPEN_MAX) {
      return EBADF;
    }
    if (curthread->t_filetable[fd] == NULL){
        return EBADF;
    }
    if ( buf == NULL || curthread->t_filetable[fd]->vnodes == NULL){
        return EFAULT;
    }
    lock_acquire(curthread->t_filetable[fd]->ftlock);
    /* set up a uio with the buffer, its size, and the current offset */
    mk_useruio(&user_iov, &user_uio, buf, size, curthread->t_filetable[fd]->offsets, UIO_READ);

    /* does the read */
   result = VOP_READ(curthread->t_filetable[fd]->vnodes, &user_uio);
    if (result) {
        lock_release(curthread->t_filetable[fd]->ftlock);
        return result;
    }
    /*
     * The amount read is the size of the buffer originally, minus
     * how much is left in it.
     */
    *retval = size - user_uio.uio_resid;
    lock_release(curthread->t_filetable[fd]->ftlock);
    curthread->t_filetable[fd]->offsets = user_uio.uio_offset;

    return 0;
}

/*
 * sys_write
 * calls VOP_WRITE.
 *
 * A4: This is the "dumb" implementation of sys_write:
 * it only deals with file descriptors 1 and 2, and
 * assumes they are permanently associated with the
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */

int
sys_write(int fd, userptr_t buf, size_t len, int *retval)
{
        struct uio user_uio;
        struct iovec user_iov;
        int result;
        
        if (fd < 0 || fd >= __OPEN_MAX) {
                return EBADF;
        }
        if (curthread->t_filetable[fd] == NULL){
                return EBADF;
        }
        if ( buf == NULL || curthread->t_filetable[fd]->vnodes == NULL){
                return EFAULT;
        }
        lock_acquire(curthread->t_filetable[fd]->ftlock);
        /* set up a uio with the buffer, its size, and the current offset */
        mk_useruio(&user_iov, &user_uio, buf, len, curthread->t_filetable[fd]->offsets, UIO_WRITE);

        /* does the write */
        result = VOP_WRITE(curthread->t_filetable[fd]->vnodes, &user_uio);
        if (result) {
            lock_release(curthread->t_filetable[fd]->ftlock);
                return result;
        }

        /*
         * the amount written is the size of the buffer originally,
         * minus how much is left in it.
         */
        *retval = len - user_uio.uio_resid;
        lock_release(curthread->t_filetable[fd]->ftlock);
	   curthread->t_filetable[fd]->offsets = user_uio.uio_offset;
       
        return 0;
}

/*
 * sys_lseek
 *
 */
int
sys_lseek(int fd, off_t offset, int whence, off_t *retval)
{
      
    /**
     * 
     * @param fd : The file descriptor of the pointer that is going to be moved.
     * @param offset : The offset of the pointer (measured in bytes).
     * @param whence : The method in which the offset is to be interpreted 
     * (relative, absolute, etc.). Legal values for this variable are provided at the end.
     * @param retval : Returns the offset of the pointer (in bytes) from the 
     * beginning of the file. If the return value is -1, then there was an 
     * error moving the pointer.
     * @return -1 on error otherwise offset of the poitner.
     */
    
    //error checking
    //check file desc boundaries and check if file descriptor is valid
    if (fd < 0 || fd >= __OPEN_MAX) {
        *retval = -1;
        return EBADF;
    }
    
    if (curthread->t_filetable[fd] == NULL) {
        *retval = -1;
        return EBADF;
    }
    
    off_t pos;
    struct filetable *ft = curthread->t_filetable[fd];
    struct stat stats;
    int result;
            
    switch (whence) {
        	case SEEK_SET:
                pos = offset;
                break;

                case SEEK_CUR:
                pos = ft->offsets + offset;
                break;

                case SEEK_END:
                VOP_STAT(ft->vnodes, &stats);
                KASSERT(&stats != NULL);
                pos = stats.st_size + offset;
                break;

                default:
                return EINVAL;
    }
    
    if(pos < 0) {
            *retval = -1;
            return EINVAL;
    }
    
    if (ft->vnodes == NULL){
        *retval = -1;
        return EINVAL;
    }
    result = VOP_TRYSEEK(ft->vnodes, pos);
    
    if(result) {
        return result;
    }
    
    ft->offsets = pos;
    *retval = ft->offsets;
    
    return 0;
}

/* really not "file" calls, per se, but might as well put it here */

/*
 * Given a path, change the curthread current working directory to the working directory
 * of the given path.
 * Return 0 upon success and -1 upon failure.
 */
int
sys_chdir(userptr_t path)
{
    int result;
    struct vnode *dir;
    char *path_name;
   
   
    if ( (path_name = (char *)kmalloc(__PATH_MAX)) == NULL) {
        return ENOMEM;
    }
 
    result = copyinstr(path, path_name, __PATH_MAX, NULL);
    if (result) {
        kfree(path_name);
        return result;
    }
   
    // path is too long.. but when can this be?
    if (sizeof(path_name) > __PATH_MAX) {
      return ENAMETOOLONG;
    }

    // find the node belonging to the desired directory
    result  = vfs_lookup(path_name, &dir); 
    if (result) { // not zero
        return result;
    }
    // make it the current directory
    result = vfs_setcurdir(dir);

    return result;
}

/*
 * get the pathname of the current working directory
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval) // why triple underline?
{
    struct uio user_uio;
    struct iovec user_iov;
    int result;

    // the size argument is 0
    if (buflen == 0) {
        return EINVAL;
    }

    mk_useruio(&user_iov, &user_uio, buf, buflen, 0, UIO_READ);
    result = vfs_getcwd(&user_uio);
    if (result) {
        *retval = -1; // why? Because we're returning -1 on error - Zafer
        return result;
    }
   
    // save the # of bytes read in retval
    *retval = buflen - user_uio.uio_resid;

    return 0;
}

/*
 * obtain information about an open file associated with
 * the file descriptor fildes, and shall write it to the
 * area pointed to by buf.
 */
int
sys_fstat(int fd, userptr_t statptr)
{
    if (fd < 0 || fd >= __OPEN_MAX) {
      return EBADF;
    }
    if (curthread->t_filetable[fd] == NULL){
        return EBADF;
    }
    if ( statptr == NULL || curthread->t_filetable[fd]->vnodes == NULL){
        return EFAULT;
    }

    struct vnode *file_vnode;
    struct stat stats;
    struct uio user_uio;
    struct iovec user_iov;
    int result;

    file_vnode = curthread->t_filetable[fd]->vnodes;

    // file doesn't exist, must take out this error check in a function
    if (!file_vnode) {
        return EBADF;
    }

    result = VOP_STAT(file_vnode, &stats);
    if (result) {
        return result;
    }

   
    // now must make statptr point to stats...
     mk_useruio(&user_iov, &user_uio, statptr, sizeof(stats), 0, UIO_READ);
     result = uiomove(&stats, sizeof(stats), &user_uio);
     return result;
}

/*
 * read filename from directory
 */
int
sys_getdirentry(int fd, userptr_t buf, size_t buflen, int *retval)
{
    if (fd < 0 || fd >= __OPEN_MAX){
        return EBADF;
    }
    if (curthread->t_filetable[fd] == NULL){
        return EBADF;
    }
    if ( buf == NULL || curthread->t_filetable[fd]->vnodes == NULL){
        return EFAULT;
    }
    struct vnode *file_vnode;
    struct uio user_uio;
    struct iovec user_iov;
    int result;

    file_vnode = curthread->t_filetable[fd]->vnodes;

     // file doesn't exist
    if (!file_vnode) {
        *retval = -1;
        return EBADF;
    }

    mk_useruio(&user_iov, &user_uio, buf, buflen, curthread->t_filetable[fd]->offsets, UIO_READ);
    result = VOP_GETDIRENTRY(file_vnode, &user_uio); 

    // vop_getdirentry failed
    if (result) {
        *retval = -1;
        return result;
    }

    // return the size of file name
    *retval = buflen - user_uio.uio_resid;

    // better take this out into a function
    curthread->t_filetable[fd]->offsets = user_uio.uio_offset;
    return 0;
}


/* END A4 SETUP */
