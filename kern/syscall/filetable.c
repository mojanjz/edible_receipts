
#include <kern/errno.h>
#include <kern/limits.h>
#include <types.h>
#include <filetable.h>
#include <file_entry.h>
#include <current.h>
#include <vfs.h>

int 
filetable_init(void) {
    struct filetable *ft = (struct filetable *)kmalloc(sizeof(struct filetable));

    /*TODO: Need to initialize the first three file entries */

    /* Initialize file entries in the file table*/
    for (int fd = 0; fd < __OPEN_MAX; fd++) {
        if (fd < 3) {
            // STORE SOMETHING IN THE FIRST THREE
        } else {
            ft->ft_file_entries[fd] = NULL;
        }

    }

    curthread->t_filetable = ft;

    return 0;
}

// TODO: add synchronization here
int
file_open(char *filename, int flags, mode_t mode, int *retfd) {
    int err;
    char buf[32];
    struct vnode *ft_vnode;

    // already checked in vfs_open we hypothetically don't need this
    // /* Check if flags are right */
    // int how = flags & O_ACCMODE;
    // if (how != O_WRONLY || how != O_RDONLY || how != O_RDWR)
    //     return EINVAL;
    
    /* Find an emptry row in the filetable */
    struct filetable *filetable = curthread->t_filetable;
    int fd = 0;

    for (fd = 0; fd < __OPEN_MAX; fd++){
        if(filetable->ft_file_entries[fd] == NULL) // found an empty slot
            break;
    }
    
    /* File table is full */
    if(fd == __OPEN_MAX) {
        return EMFILE; 
    }

    /* vfs_open destroys the string passed into it, let's copy*/
    strcpy(buf, filename);
    err = vfs_open(buf, flags, mode, &ft_vnode);
    if (err) {
        return err;
    }

    /* Update the file table with the vnode */
    filetable->ft_file_entries[fd] = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    filetable->ft_file_entries[fd]->fe_status = flags;
    filetable->ft_file_entries[fd]->fe_offset = 0;
    filetable->ft_file_entries[fd]->fe_vn= ft_vnode;
    filetable->ft_file_entries[fd]->fe_filename = filename;

    /* create the lock */
    char fe_lock_name[__OPEN_MAX+10];
	snprintf(fe_lock_name, __OPEN_MAX+10, "fe-lock-%d", fd);
	filetable->ft_file_entries[fd]->fe_lock = lock_create(fe_lock_name);

    *retfd = fd;

    return 0;
}