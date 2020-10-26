#include <kern/errno.h>
#include <kern/limits.h>
#include <types.h>
#include <filetable.h>
#include <file_entry.h>
#include <current.h>
#include <vfs.h>

/* 
 * Function to create and initialize a new file table.
 * 
 * Parameters: void
 * Returns: the newly created & initialized file table
 */
struct filetable *
filetable_init()
{
    struct filetable *ft = (struct filetable *)kmalloc(sizeof(struct filetable));
    if (ft == NULL) {
        return NULL;
    }

    /* Create filetable lock */
    ft->ft_lock = lock_create("filetable-lock");
    if(ft->ft_lock == NULL) {
        kfree(ft);
        return NULL;
    }

    /* Initialize file entries in the file table to be NULL*/
    for (int fd = 0; fd < __OPEN_MAX; fd++) {
        ft->ft_file_entries[fd] = NULL;
    }

    return ft;
}

/* 
 * Initialize the first three filedescriptors for STDIN, STDOUT, STDERR.
 * Parameters: pointer to ft (the file table for which to init the special file entries)
 */
int 
filetable_init_std(struct filetable *ft){
    struct vnode *std_in_vn = NULL;
    struct vnode *std_out_vn = NULL;
    struct vnode *std_err_vn = NULL;
    int err = 0;
    char path_in[5];
    char path_out[5];
    char path_err[5];

    /* Path string cannot be reused since vfs_open destroys the string passed to it */
    strcpy(path_in, "con:");
    strcpy(path_out, "con:");
    strcpy(path_err, "con:");

    err = vfs_open(path_in, O_RDONLY, 0, &std_in_vn);
    err = vfs_open(path_out, O_WRONLY, 0, &std_out_vn);
    err = vfs_open(path_err, O_WRONLY, 0, &std_err_vn);

    if (err) {
        return err;
    }

    struct file_entry *std_in_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    struct file_entry *std_out_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    struct file_entry *std_err_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    if ((std_in_fe == NULL)|| (std_out_fe == NULL) || (std_err_fe == NULL))
        return ENOMEM;

    /* Init STDIN */
    std_in_fe->fe_filename = path_in;
    std_in_fe->fe_vn = std_in_vn;
    std_in_fe->fe_offset = 0;
    std_in_fe->fe_status = O_RDONLY;
    std_in_fe->fe_refcount = 1; 
    std_in_fe->fe_lock = lock_create("std-in-lock"); 
    if (std_in_fe->fe_lock == NULL){
        kfree(std_in_fe);
        return ENOMEM;
    }

    /* Init STDOUT */
    std_out_fe->fe_filename = path_out;
    std_out_fe->fe_vn = std_out_vn;
    std_out_fe->fe_offset = 0;
    std_out_fe->fe_status = O_WRONLY;
    std_out_fe->fe_refcount = 1; 
    std_out_fe->fe_lock = lock_create("std-out-lock"); 
    if (std_out_fe->fe_lock == NULL){
        kfree(std_out_fe);
        return ENOMEM; 
    }
    /* Init STDERR */
    std_err_fe->fe_filename = path_err;
    std_err_fe->fe_vn = std_err_vn;
    std_err_fe->fe_offset = 0;
    std_err_fe->fe_status = O_WRONLY;
    std_err_fe->fe_refcount = 1; 
    std_err_fe->fe_lock = lock_create("std-err-lock"); 
    if (std_err_fe->fe_lock == NULL){
        kfree(std_err_fe);
        return ENOMEM;
    }    

    ft->ft_file_entries[0] = std_in_fe;
    ft->ft_file_entries[1] = std_out_fe;
    ft->ft_file_entries[2] = std_err_fe;
    
    return 0;
}

/*
 * Opens a file from a file table.  Calls vfs_open on the filename stored in a buffer kernel
 * Stores the open file as a file entry in the process's filetable.
 * 
 * Parameters: pointer to filename to open, flags (specify how to open file), mode (optional param specifying permissions),
 * pointer to retval.
 * Returns: On success, 0.  On failure, error value.
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
    lock_acquire(filetable->ft_lock);
    for (fd = 3; fd < __OPEN_MAX; fd++){
        if(filetable->ft_file_entries[fd] == NULL) {
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

    return 0;
}

/* 
 * Closes the file with file handle fd
 *
 * Parameters: fd (file handle to close)
 * Returns: On success, 0.  On failure, error code.
 */
int
file_close(int fd)
{
    int err = 0;
    struct filetable *ft = curproc->p_filetable;
    struct file_entry *fe;

    /* Making sure no one changes the filetable while we access it */
    lock_acquire(ft->ft_lock);
    fe = ft->ft_file_entries[fd];
    /* Check if the file is already closed */
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

/* 
 * UNSYNCHRONIZED method used to close a file in dup2.  Same as file_close but not atomic since
 *  dup2 is atomic already.
 * 
 * Parameters: fd (file handle to close)
 * Returns: On success, 0.  On failure, error code.
 */
int
dup_file_close(int fd)
{

    int err = 0;
    struct filetable *ft = curproc->p_filetable;
    struct file_entry *fe;

    fe = ft->ft_file_entries[fd];
    /* Check if the file is already closed */
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

/* 
 *Destroys a filetable.

 * Parameters: ft (filetable to destroy)
 * Returns: void
 */
void filetable_destroy(struct filetable *ft)
{
    KASSERT(ft != NULL);

    /* Iterate over file table and destroy file entries */
    for (int i = __OPEN_MAX - 1; i <= 0; i--){
        /* We can just call file_close, since it destroys the file entry's lock and kfrees it */
        if(ft->ft_file_entries[i] != NULL)
            file_close(i);
    }

    lock_destroy(ft->ft_lock);
    kfree(ft);
}

void
filetable_copy(struct filetable *new_ft, struct filetable *ft)
{
    lock_acquire(ft->ft_lock);
    for (int i=0; i< __OPEN_MAX; i++) {
        struct file_entry *fe = ft->ft_file_entries[i];

        if(fe == NULL) {
            continue;
        }

        lock_acquire(fe->fe_lock);
        fe->fe_refcount = fe->fe_refcount + 1;
        lock_release(fe->fe_lock);
    
        new_ft->ft_file_entries[i] = fe;
    }

    lock_release(ft->ft_lock);
}