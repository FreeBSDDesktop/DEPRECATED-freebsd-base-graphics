
#include <linux/file.h>
#include <linux/fs.h>

struct file *
anon_inode_getfile(const char *name,
				   const struct file_operations *fops,
				   void *priv, int flags);
int
anon_inode_getfd(const char *name, const struct file_operations *fops,
				 void *priv, int flags);
