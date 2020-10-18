
#include <kern/errno.h>
#include <kern/limits.h>
#include <types.h>
#include <filetable.h>
#include <file_entry.h>
#include <current.h>
#include <vfs.h>

// TODO: ADD FILETABLE DESTROY
int 
filetable_init(void) {
    kprintf("initializing filetable\n");
    struct filetable *ft = (struct filetable *)kmalloc(sizeof(struct filetable));

    /* Create filetable lock */
    ft->ft_lock = lock_create("filetable-lock");

    /* Initialize the first three filedescriptors for STDIN, STDOUT, STDERR */
    struct vnode *cons_vn = NULL;
    char path[5];
    strcpy(path, "con:");
    int err = vfs_open(path, O_RDWR, 0, &cons_vn);

    if (err) {
        kprintf("could not open console file with error: ");
        kprintf(strerror(err));
        return err;
    }

    struct file_entry *cons_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    cons_fe->fe_filename = path;
    cons_fe->fe_vn = cons_vn;
    cons_fe->fe_offset = 0;
    cons_fe->fe_status = O_RDWR;
    cons_fe->fe_refcount = 3; // all three point to the same file entry;
    cons_fe->fe_lock = lock_create("cons-lock"); // TODO: should they all have the same lock?

    /* Initialize file entries in the file table*/
    for (int fd = 0; fd < __OPEN_MAX; fd++) {
        if (fd < 3) {
            ft->ft_file_entries[fd] = cons_fe;
        } else {
            ft->ft_file_entries[fd] = NULL;
        }
    }

    curthread->t_filetable = ft;

    return 0;
}

/*
 * Calls vfs_open on the filename stored in a buffer kernel
 * Stores the open file as a file entry in the process's filetable
 */
int
file_open(char *filename, int flags, mode_t mode, int *retfd) {
    int err;
    char buf[32];
    struct vnode *ft_vnode;

    /* Check if flags are right */
    int how = flags & O_ACCMODE;
    if (how != O_WRONLY && how != O_RDONLY && how != O_RDWR)
        return EINVAL;
    
    /* Find an emptry row in the filetable */
    struct filetable *filetable = curthread->t_filetable;
    int fd = 3;

    lock_acquire(filetable->ft_lock);
    for (fd = 3; fd < __OPEN_MAX; fd++){
        if(filetable->ft_file_entries[fd] == NULL) {
            // kprintf("found an empty slot at %d for file %s\n", fd, filename);
            break;
        }
    }
    
    /* File table is full */
    if(fd == __OPEN_MAX) {
        kprintf("file table is full for file %s\n", filename);
        lock_release(filetable->ft_lock);
        return EMFILE; 
    }

    /* vfs_open destroys the string passed into it, let's copy*/
    strcpy(buf, filename);
    err = vfs_open(buf, flags, mode, &ft_vnode);
    if (err) {
        lock_release(filetable->ft_lock);
        return err;
    }

    /* Update the file table with the vnode */
    filetable->ft_file_entries[fd] = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    filetable->ft_file_entries[fd]->fe_status = flags;
    filetable->ft_file_entries[fd]->fe_offset = 0;
    filetable->ft_file_entries[fd]->fe_vn= ft_vnode;
    filetable->ft_file_entries[fd]->fe_filename = filename;
    filetable->ft_file_entries[fd]->fe_refcount = 1;
    /* Create the file entry lock */
    char fe_lock_name[__OPEN_MAX+10];
	snprintf(fe_lock_name, __OPEN_MAX+10, "fe-lock-%d", fd);
	filetable->ft_file_entries[fd]->fe_lock = lock_create(fe_lock_name);
    lock_release(filetable->ft_lock);
    *retfd = fd;

    // kprintf("successfully opened file %s with fd: %d\n", filename, fd);
    return 0;
}

int
file_close(int fd)
{
    int err = 0;
    struct filetable *ft = curthread->t_filetable;
    struct file_entry *fe;

    /* Making sure noone changes the filetable while we access it */
    lock_acquire(ft->ft_lock);
    fe = ft->ft_file_entries[fd];
    fe->fe_refcount --;
    /* If there are no more references close it */
    if (fe->fe_refcount == 0) {
        vfs_close(ft->ft_file_entries[fd]->fe_vn);
        lock_destroy(fe->fe_lock);
        kfree(fe);
    }

    ft->ft_file_entries[fd] = NULL;
    lock_release(ft->ft_lock);

    return err;
}