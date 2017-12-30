#ifndef _LINUX_SYSCTL_H
#define _LINUX_SYSCTL_H

#include <linux/types.h>
#include <linux/rbtree.h>


// These two belong in linux/uidgid.h
typedef struct {
	uid_t val;
} kuid_t;
typedef struct {
	gid_t val;
} kgid_t;



typedef struct ctl_table ctl_table;

typedef int proc_handler (struct ctl_table *ctl, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos);


/* A sysctl table is an array of struct ctl_table: */
struct ctl_table
{
	const char *procname;		/* Text ID for /proc/sys, or zero */
	void *data;
	int maxlen;
	umode_t mode;
	struct ctl_table *child;	/* Deprecated */
	proc_handler *proc_handler;	/* Callback for text formatting */
	struct ctl_table_poll *poll;
	void *extra1;
	void *extra2;
};

struct ctl_node {
	struct rb_node node;
	struct ctl_table_header *header;
};

/* struct ctl_table_header is used to maintain dynamic lists of
   struct ctl_table trees. */
struct ctl_table_header
{
	union {
		struct {
			struct ctl_table *ctl_table;
			int used;
			int count;
			int nreg;
		};
		struct rcu_head rcu;
	};
	struct completion *unregistering;
	struct ctl_table *ctl_table_arg;
	struct ctl_table_root *root;
	struct ctl_table_set *set;
	struct ctl_dir *parent;
	struct ctl_node *node;
	struct list_head inodes; /* head for proc_inode->sysctl_inodes */
};

struct ctl_dir {
	/* Header must be at the start of ctl_dir */
	struct ctl_table_header header;
	struct rb_root root;
};

struct ctl_table_set {
	int (*is_seen)(struct ctl_table_set *);
	struct ctl_dir dir;
};

struct ctl_table_root {
	struct ctl_table_set default_set;
	struct ctl_table_set *(*lookup)(struct ctl_table_root *root);
	void (*set_ownership)(struct ctl_table_header *head,
			      struct ctl_table *table,
			      kuid_t *uid, kgid_t *gid);
	int (*permissions)(struct ctl_table_header *head, struct ctl_table *table);
};

/* struct ctl_path describes where in the hierarchy a table is added */
struct ctl_path {
	const char *procname;
};



/**
 * proc_dointvec_minmax - read a vector of integers with min/max values
 * @table: the sysctl table
 * @write: %TRUE if this is a write to the sysctl file
 * @buffer: the user buffer
 * @lenp: the size of the user buffer
 * @ppos: file position
 *
 * Reads/writes up to table->maxlen/sizeof(unsigned int) integer
 * values from/to the user buffer, treated as an ASCII string.
 *
 * This routine will ensure the values are within the range specified by
 * table->extra1 (min) and table->extra2 (max).
 *
 * Returns 0 on success.
 */
static inline int
proc_dointvec_minmax(struct ctl_table *table, int write,
					 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return 0;
}



#ifdef CONFIG_SYSCTL

// Pretend we don't support sysctl for now (need by i915_perf.c - important?)

#else /* CONFIG_SYSCTL */

static inline struct ctl_table_header *register_sysctl_table(struct ctl_table * table)
{
	return NULL;
}

static inline struct ctl_table_header *register_sysctl_paths(
			const struct ctl_path *path, struct ctl_table *table)
{
	return NULL;
}

static inline void unregister_sysctl_table(struct ctl_table_header * table)
{
}

static inline void setup_sysctl_set(struct ctl_table_set *p,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *))
{
}

#endif /* CONFIG_SYSCTL */

#endif
