
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
    int err = 0;

    /* Create filetable lock */
    ft->ft_lock = lock_create("filetable-lock");

    /* Initialize the first three filedescriptors for STDIN, STDOUT, STDERR */
    // struct vnode *cons_vn = NULL;
    // char path[5];
    // strcpy(path, "con:");
    // int err = vfs_open(path, O_RDWR, 0, &cons_vn);

    // if (err) {
    //     kprintf("could not open console file with error: ");
    //     kprintf(strerror(err));
    //     return err;
    // }

    // struct file_entry *cons_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    // cons_fe->fe_filename = path;
    // cons_fe->fe_vn = cons_vn;
    // cons_fe->fe_offset = 0;
    // cons_fe->fe_status = O_RDWR;
    // cons_fe->fe_refcount = 3; // all three point to the same file entry;
    // cons_fe->fe_lock = lock_create("cons-lock"); // TODO: should they all have the same lock?

    /* Initialize file entries in the file table*/
    for (int fd = 0; fd < __OPEN_MAX; fd++) {
        // if (fd < 3) {
        //     ft->ft_file_entries[fd] = cons_fe;
        // } else {
            ft->ft_file_entries[fd] = NULL;
        // }
    }

    err = filetable_init_cons(ft);
    if(err){
        kprintf("Error initing ft_cons");
        return err;
    }

    curthread->t_filetable = ft;

    return 0;
}

/* Initialize the first three filedescriptors for STDIN, STDOUT, STDERR */
int 
filetable_init_cons(struct filetable *ft){
    kprintf("In console init");
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

    kprintf("Checkpoint 1\n");
    err = vfs_open(path_in, O_RDONLY, 0, &cons_in_vn);
    kprintf("Is there an error? %d", err);
    err = vfs_open(path_out, O_WRONLY, 0, &cons_out_vn);
    kprintf("Is there an error? %d", err);
    err = vfs_open(path_err, O_WRONLY, 0, &cons_err_vn);
    kprintf("Is there an error? %d", err);

    kprintf("Checkpoint 2\n");
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
        kprintf("file table is full\n");
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