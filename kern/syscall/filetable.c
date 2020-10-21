
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
    struct filetable *ft = (struct filetable *)kmalloc(sizeof(struct filetable)); // TODO check if it actually allocated space
    int err = 0;

    /* Create filetable lock */
    kprintf("before creating the lock\n");
    ft->ft_lock = lock_create("filetable-lock");
    kprintf("after creating the lock\n");
    /* Initialize file entries in the file table*/
    for (int fd = 0; fd < __OPEN_MAX; fd++) {
        ft->ft_file_entries[fd] = NULL;
    }

    err = filetable_init_cons(ft);
    if(err){
        kprintf("Error initing ft_cons");
        return err;
    }
    kprintf("is is before allocation\n");
    curproc->p_filetable = ft;
    kprintf("is if after allocation\n");
    return 0;
}

/* Initialize the first three filedescriptors for STDIN, STDOUT, STDERR */
int 
filetable_init_cons(struct filetable *ft){
    struct vnode *cons_in_vn = NULL;
    struct vnode *cons_out_vn = NULL;
    struct vnode *cons_err_vn = NULL;
    int err = 0;
    char path_in[5];
    char path_out[5];
    char path_err[5];

    strcpy(path_in, "con:");
    strcpy(path_out, "con:");
    strcpy(path_err, "con:");

    err = vfs_open(path_in, O_RDONLY, 0, &cons_in_vn);
    err = vfs_open(path_out, O_WRONLY, 0, &cons_out_vn);
    err = vfs_open(path_err, O_WRONLY, 0, &cons_err_vn);

    if (err) {
        kprintf("could not open console file with error: ");
        kprintf(strerror(err));
        return err;
    }

    struct file_entry *cons_in_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    struct file_entry *cons_out_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    struct file_entry *cons_err_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));

    cons_in_fe->fe_filename = path_in;
    cons_in_fe->fe_vn = cons_in_vn;
    cons_in_fe->fe_offset = 0;
    cons_in_fe->fe_status = O_RDONLY;
    cons_in_fe->fe_refcount = 1; 
    cons_in_fe->fe_lock = lock_create("cons-in-lock"); 

    cons_out_fe->fe_filename = path_out;
    cons_out_fe->fe_vn = cons_out_vn;
    cons_out_fe->fe_offset = 0;
    cons_out_fe->fe_status = O_WRONLY;
    cons_out_fe->fe_refcount = 1; 
    cons_out_fe->fe_lock = lock_create("cons-out-lock"); 

    cons_err_fe->fe_filename = path_err;
    cons_err_fe->fe_vn = cons_err_vn;
    cons_err_fe->fe_offset = 0;
    cons_err_fe->fe_status = O_WRONLY;
    cons_err_fe->fe_refcount = 1; 
    cons_err_fe->fe_lock = lock_create("cons-err-lock");     

    ft->ft_file_entries[0] = cons_in_fe;
    ft->ft_file_entries[1] = cons_out_fe;
    ft->ft_file_entries[2] = cons_err_fe;
    
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
    
    /* Find an empty row in the filetable */
    struct filetable *filetable = curproc->p_filetable;
    int fd = 3;
    kprintf("checkpoint 2.1\n");
    lock_acquire(filetable->ft_lock);
    kprintf("checkpoint 2.1.1\n");
    for (fd = 3; fd < __OPEN_MAX; fd++){
        if(filetable->ft_file_entries[fd] == NULL) {
            // kprintf("found an empty slot at %d for file %s\n", fd, filename);
            break;
        }
    }
    kprintf("checkpoint 2.2\n");
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
    kprintf("checkpoint 2.3\n");
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
    struct filetable *ft = curproc->p_filetable;
    struct file_entry *fe;

    /* Making sure noone changes the filetable while we access it */
    lock_acquire(ft->ft_lock);
    fe = ft->ft_file_entries[fd];
    /* check if the file is already closed */
    if (fe == NULL) {
        lock_release(ft->ft_lock);
        return EBADF;
    }
    /* If it's open let's close it */
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

// same as file_close but not atomic cuz dup2 is atomic already
int
dup_file_close(int fd)
{

    int err = 0;
    struct filetable *ft = curproc->p_filetable;
    struct file_entry *fe;

    fe = ft->ft_file_entries[fd];
    /* check if the file is already closed */
    if (fe == NULL) {
        return EBADF;
    }
    /* If it's open let's close it */
    fe->fe_refcount --;
    /* If there are no more references close it */
    if (fe->fe_refcount == 0) {
        vfs_close(ft->ft_file_entries[fd]->fe_vn);
        kfree(fe);
    }

    ft->ft_file_entries[fd] = NULL;

    return err;
}