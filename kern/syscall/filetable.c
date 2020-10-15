
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

    /*TODO: Need to initialize the first three file entries */

    /* Initialize the first three filedescriptors for STDIN, STDOUT, STDERR */
    struct vnode *cons_vn = NULL;
    char path[5];
    strcpy(path, "con:");
    int err = vfs_open(path, O_RDWR, 0, &cons_vn);

    if (err) {
        kprintf("could not open console file with error: ");
        kprintf(strerror(err));
    }

    struct file_entry *cons_fe = (struct file_entry *)kmalloc(sizeof(struct file_entry));
    cons_fe->fe_filename = path;
    cons_fe->fe_vn = cons_vn;
    cons_fe->fe_offset = 0;
    cons_fe->fe_status = O_RDWR;

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
    int fd = 3;

    for (fd = 3; fd < __OPEN_MAX; fd++){
        if(filetable->ft_file_entries[fd] == NULL) // found an empty slot
        // kprintf("found an empty slot at %d for file %s\n", fd, filename);
        break;
    }
    
    /* File table is full */
    if(fd == __OPEN_MAX) {
        kprintf("file table is full\n");
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

    // kprintf("the file %s is now stored at fd %d with vnode address %p\n", filename, fd, (void *)filetable->ft_file_entries[fd]->fe_vn);

    /* create the lock */
    char fe_lock_name[__OPEN_MAX+10];
	snprintf(fe_lock_name, __OPEN_MAX+10, "fe-lock-%d", fd);
	filetable->ft_file_entries[fd]->fe_lock = lock_create(fe_lock_name);

    *retfd = fd;

    return 0;
}