// FreeBSD
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/namei.h>
#include <sys/vnode.h>

// Linux
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>


struct file *
anon_inode_getfile(const char *name,
				   const struct file_operations *fops,
				   void *priv, int flags) {

	struct thread *td = curthread;
	struct nameidata nd;
	struct linux_file *fp;
	int error;
	int mode = 0;
	int cmode = 0;
	u_int vn_open_flags = 0;

	if (td->td_proc->p_fd->fd_rdir == NULL)
		td->td_proc->p_fd->fd_rdir = rootvnode;
	if (td->td_proc->p_fd->fd_cdir == NULL)
		td->td_proc->p_fd->fd_cdir = rootvnode;

	flags = FFLAGS(flags);

	// Create file descriptor
	fp = alloc_file(mode, fops);
	if(!fp)
		return (NULL);
	fp->f_flags = flags & FMASK;

	NDINIT(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, name, td);
	error = vn_open_cred(&nd, &flags, cmode, vn_open_flags, td->td_ucred, fp->_file);
	NDFREE(&nd, NDF_ONLY_PNBUF);
	if (error != 0)
		return (NULL);

	fp->f_vnode = nd.ni_vp;
	fp->f_flags = flags & (O_ACCMODE | O_NONBLOCK);
	fp->private_data = priv;
	fp->f_op = fops;
	fp->f_mode = mode;

	VOP_UNLOCK(nd.ni_vp, 0);

	return fp;
}
