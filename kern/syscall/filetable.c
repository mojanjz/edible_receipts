#include <filetable.h>
#include <current.h>

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

    curthread->t_proc->p_filetable = ft;

    return 0;
}