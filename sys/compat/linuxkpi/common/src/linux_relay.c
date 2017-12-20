#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/kernel.h>

#include <linux/relay.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <asm/uaccess.h>


#pragma GCC warning "The whole relay channel system need review, fixing and testing!"



// Default channel callbacks

static int subbuf_start_default_callback (struct rchan_buf *buf,
					  void *subbuf,
					  void *prev_subbuf,
					  size_t prev_padding) {
	if (relay_buf_full(buf))
		return 0;
	return 1;
}

static void buf_mapped_default_callback(struct rchan_buf *buf,
					struct file *filp) {
}

static void buf_unmapped_default_callback(struct rchan_buf *buf,
					  struct file *filp) {
}

static struct dentry *create_buf_file_default_callback(const char *filename,
						       struct dentry *parent,
						       umode_t mode,
						       struct rchan_buf *buf,
						       int *is_global) {
	return NULL;
}

static int remove_buf_file_default_callback(struct dentry *dentry)
{
	return -EINVAL;
}

static struct rchan_callbacks default_channel_callbacks = {
	.subbuf_start = subbuf_start_default_callback,
	.buf_mapped = buf_mapped_default_callback,
	.buf_unmapped = buf_unmapped_default_callback,
	.create_buf_file = create_buf_file_default_callback,
	.remove_buf_file = remove_buf_file_default_callback,
};

static void setup_callbacks(struct rchan *chan,
				   struct rchan_callbacks *cb) {
	if(!cb) {
		chan->cb = &default_channel_callbacks;
		return;
	}

	if(!cb->subbuf_start)
		cb->subbuf_start = subbuf_start_default_callback;
	if(!cb->buf_mapped)
		cb->buf_mapped = buf_mapped_default_callback;
	if(!cb->buf_unmapped)
		cb->buf_unmapped = buf_unmapped_default_callback;
	if(!cb->create_buf_file)
		cb->create_buf_file = create_buf_file_default_callback;
	if(!cb->remove_buf_file)
		cb->remove_buf_file = remove_buf_file_default_callback;
	chan->cb = cb;
}





static void error_only_global() {
	printf("ERROR! Linux relay channels support only global buffer at this time.\n");
}


static struct dentry *relay_create_buf_file(struct rchan *chan,
											struct rchan_buf *buf,
											unsigned int cpu) {
	struct dentry *dentry;
	char *tmpname;

	tmpname = kzalloc(NAME_MAX + 1, GFP_KERNEL);
	if (!tmpname)
		return NULL;
	snprintf(tmpname, NAME_MAX, "%s%d", chan->base_filename, cpu);

	/* Create file in fs */
	dentry = chan->cb->create_buf_file(tmpname, chan->parent,
									   S_IRUSR, buf,
									   &chan->is_global);
	kfree(tmpname);
	return dentry;
}






struct rchan *relay_open(const char *base_filename,
						 struct dentry *parent,
						 size_t subbuf_size,
						 size_t n_subbufs,
						 struct rchan_callbacks *cb,
						 void *private_data) {
	struct rchan *chan;
	struct rchan_buf *buf;
	struct rchan_buf **bufs;

	chan = malloc(sizeof(struct rchan), M_KMALLOC, M_WAITOK);

	buf = malloc(sizeof(struct rchan_buf), M_KMALLOC, M_WAITOK);
	bufs = malloc(2*sizeof(struct rchan_buf *), M_KMALLOC, M_WAITOK);
	bufs[0] = buf;
	bufs[1] = NULL;
	chan->buf = bufs;

	chan->private_data = private_data;
	chan->version = RELAYFS_CHANNEL_VERSION;
	chan->n_subbufs = n_subbufs;
	chan->subbuf_size = subbuf_size;
	chan->alloc_size = PAGE_ALIGN(subbuf_size * n_subbufs);
	chan->parent = parent;
	chan->private_data = private_data;
	if (base_filename) {
		chan->has_base_filename = 1;
		strlcpy(chan->base_filename, base_filename, NAME_MAX);
	}

	setup_callbacks(chan, cb);
	kref_init(&chan->kref);

	return chan;
}

extern int relay_late_setup_files(struct rchan *chan,
								  const char *base_filename,
								  struct dentry *parent) {
	int err = 0;
	struct dentry *dentry;
	struct rchan_buf *buf;

	if (!chan || !base_filename)
		return -EINVAL;

	strlcpy(chan->base_filename, base_filename, NAME_MAX);

	/* Is chan already set up? */
	if (unlikely(chan->has_base_filename)) {
		return -EEXIST;
	}
	chan->has_base_filename = 1;
	chan->parent = parent;

	if (chan->is_global) {
		err = -EINVAL;
		buf = chan->buf[0];
		if (!WARN_ON_ONCE(!buf)) {
			dentry = relay_create_buf_file(chan, buf, 0);
			if (dentry && !WARN_ON_ONCE(!chan->is_global)) {
				buf->dentry = dentry;
				err = 0;
			}
		}
		return err;
	}

	error_only_global();
	return -EINVAL;
}

extern void relay_close(struct rchan *chan) {
	if(!chan)
		return;
	struct rchan_buf *buf = chan->buf[0];
	free(buf, M_KMALLOC);
	struct rchan_buf **bufs = chan->buf;
	free(bufs, M_KMALLOC);
	free(chan, M_KMALLOC);
}

extern void relay_flush(struct rchan *chan) {

	struct rchan_buf *buf;

	if(!chan)
		return;

	if(chan->is_global && (buf = chan->buf[0])) {
		relay_switch_subbuf(buf, 0);
		return;
	}

	error_only_global();
}

extern void relay_subbufs_consumed(struct rchan *chan,
								   unsigned int cpu,
								   size_t  subbufs_consumed) {
	struct rchan_buf *buf;

	if (!chan)
		return;

	buf = chan->buf[0];
	if (!buf || subbufs_consumed > chan->n_subbufs)
		return;

	if (subbufs_consumed > buf->subbufs_produced - buf->subbufs_consumed)
		buf->subbufs_consumed = buf->subbufs_produced;
	else
		buf->subbufs_consumed += subbufs_consumed;
}

static void __relay_reset(struct rchan_buf *buf, unsigned int init) {
#pragma GCC warning "Incomplete implementation (check IRQ stuff)??"
	size_t i;

	if (init) {
		init_waitqueue_head(&buf->read_wait);
		kref_init(&buf->kref);
		/* init_irq_work(&buf->wakeup_work, wakeup_readers); */
	} else {
		/* irq_work_sync(&buf->wakeup_work); */
	}

	buf->subbufs_produced = 0;
	buf->subbufs_consumed = 0;
	buf->bytes_consumed = 0;
	buf->finalized = 0;
	buf->data = buf->start;
	buf->offset = 0;

	for (i = 0; i < buf->chan->n_subbufs; i++)
		buf->padding[i] = 0;

	buf->chan->cb->subbuf_start(buf, buf->data, NULL, 0);
}

extern void relay_reset(struct rchan *chan) {
	struct rchan_buf *buf;

	if (!chan)
		return;

	if (chan->is_global && (buf = chan->buf[0])) {
		__relay_reset(buf, 0);
		return;
	}

	error_only_global();
}

extern int relay_buf_full(struct rchan_buf *buf) {
	size_t ready = buf->subbufs_produced - buf->subbufs_consumed;
	return (ready >= buf->chan->n_subbufs) ? 1 : 0;
}

static int relay_buf_empty(struct rchan_buf *buf) {
	return (buf->subbufs_produced - buf->subbufs_consumed) ? 0 : 1;
}

extern size_t relay_switch_subbuf(struct rchan_buf *buf,
								  size_t length) {
	void *old, *new;
	size_t old_subbuf, new_subbuf;

	if (unlikely(length > buf->chan->subbuf_size))
		goto toobig;

	if (buf->offset != buf->chan->subbuf_size + 1) {
		buf->prev_padding = buf->chan->subbuf_size - buf->offset;
		old_subbuf = buf->subbufs_produced % buf->chan->n_subbufs;
		buf->padding[old_subbuf] = buf->prev_padding;
		buf->subbufs_produced++;
		if (buf->dentry)
			i_size_write(d_inode(buf->dentry), buf->chan->subbuf_size - buf->padding[old_subbuf]);
		else
			buf->early_bytes += buf->chan->subbuf_size -
					    buf->padding[old_subbuf];
		smp_mb();
		if (waitqueue_active(&buf->read_wait)) {
			/*
			 * Calling wake_up_interruptible() from here
			 * will deadlock if we happen to be logging
			 * from the scheduler (trying to re-grab
			 * rq->lock), so defer it.
			 */
			/* irq_work_queue(&buf->wakeup_work); */
			// Try wakeup from here. Ok on FreeBSD?
			wake_up_interruptible(&buf->read_wait);
		}
	}

	old = buf->data;
	new_subbuf = buf->subbufs_produced % buf->chan->n_subbufs;
	new = (char*)buf->start + new_subbuf * buf->chan->subbuf_size;
	buf->offset = 0;
	if (!buf->chan->cb->subbuf_start(buf, new, old, buf->prev_padding)) {
		buf->offset = buf->chan->subbuf_size + 1;
		return 0;
	}
	buf->data = new;
	buf->padding[new_subbuf] = 0;

	if (unlikely(length + buf->offset > buf->chan->subbuf_size))
		goto toobig;

	return length;

toobig:
	buf->chan->last_toobig = length;
	return 0;

}




static int relay_file_open(struct inode *inode, struct file *filp) {
	struct rchan_buf *buf = inode->i_private;
	kref_get(&buf->kref);
	filp->private_data = buf;

	return nonseekable_open(inode, filp);
}

static unsigned int relay_file_poll(struct file *filp, poll_table *wait) {
	unsigned int mask = 0;
	struct rchan_buf *buf = filp->private_data;

	if (buf->finalized)
		return POLLERR;

	if (filp->f_mode & FMODE_READ) {
		poll_wait(filp, &buf->read_wait, wait);
		if (!relay_buf_empty(buf))
			mask |= POLLIN | POLLRDNORM;
	}

	return mask;
}

static int relay_file_release(struct inode *inode, struct file *filp) {
	/* struct rchan_buf *buf = filp->private_data; */
	/* kref_put(&buf->kref, relay_remove_buf); */

	// relay_close() will clean up everything

	return 0;
}

static void relay_file_read_consume(struct rchan_buf *buf,
									size_t read_pos,
									size_t bytes_consumed) {
	size_t subbuf_size = buf->chan->subbuf_size;
	size_t n_subbufs = buf->chan->n_subbufs;
	size_t read_subbuf;

	if (buf->subbufs_produced == buf->subbufs_consumed &&
	    buf->offset == buf->bytes_consumed)
		return;

	if (buf->bytes_consumed + bytes_consumed > subbuf_size) {
		relay_subbufs_consumed(buf->chan, 0 /*buf->cpu*/, 1);
		buf->bytes_consumed = 0;
	}

	buf->bytes_consumed += bytes_consumed;
	if (!read_pos)
		read_subbuf = buf->subbufs_consumed % n_subbufs;
	else
		read_subbuf = read_pos / buf->chan->subbuf_size;
	if (buf->bytes_consumed + buf->padding[read_subbuf] == subbuf_size) {
		if ((read_subbuf == buf->subbufs_produced % n_subbufs) &&
		    (buf->offset == subbuf_size))
			return;
		relay_subbufs_consumed(buf->chan, 0, 1);
		buf->bytes_consumed = 0;
	}
}

static int relay_file_read_avail(struct rchan_buf *buf, size_t read_pos) {
	size_t subbuf_size = buf->chan->subbuf_size;
	size_t n_subbufs = buf->chan->n_subbufs;
	size_t produced = buf->subbufs_produced;
	size_t consumed = buf->subbufs_consumed;

	relay_file_read_consume(buf, read_pos, 0);

	consumed = buf->subbufs_consumed;

	if (unlikely(buf->offset > subbuf_size)) {
		if (produced == consumed)
			return 0;
		return 1;
	}

	if (unlikely(produced - consumed >= n_subbufs)) {
		consumed = produced - n_subbufs + 1;
		buf->subbufs_consumed = consumed;
		buf->bytes_consumed = 0;
	}

	produced = (produced % n_subbufs) * subbuf_size + buf->offset;
	consumed = (consumed % n_subbufs) * subbuf_size + buf->bytes_consumed;

	if (consumed > produced)
		produced += n_subbufs * subbuf_size;

	if (consumed == produced) {
		if (buf->offset == subbuf_size &&
		    buf->subbufs_produced > buf->subbufs_consumed)
			return 1;
		return 0;
	}

	return 1;
}

static size_t relay_file_read_subbuf_avail(size_t read_pos,
										   struct rchan_buf *buf) {
	size_t padding, avail = 0;
	size_t read_subbuf, read_offset, write_subbuf, write_offset;
	size_t subbuf_size = buf->chan->subbuf_size;

	write_subbuf = ((char*)buf->data - (char*)buf->start) / subbuf_size;
	write_offset = buf->offset > subbuf_size ? subbuf_size : buf->offset;
	read_subbuf = read_pos / subbuf_size;
	read_offset = read_pos % subbuf_size;
	padding = buf->padding[read_subbuf];

	if (read_subbuf == write_subbuf) {
		if (read_offset + padding < write_offset)
			avail = write_offset - (read_offset + padding);
	} else
		avail = (subbuf_size - padding) - read_offset;

	return avail;
}

static size_t relay_file_read_start_pos(size_t read_pos,
										struct rchan_buf *buf) {
	size_t read_subbuf, padding, padding_start, padding_end;
	size_t subbuf_size = buf->chan->subbuf_size;
	size_t n_subbufs = buf->chan->n_subbufs;
	size_t consumed = buf->subbufs_consumed % n_subbufs;

	if (!read_pos)
		read_pos = consumed * subbuf_size + buf->bytes_consumed;
	read_subbuf = read_pos / subbuf_size;
	padding = buf->padding[read_subbuf];
	padding_start = (read_subbuf + 1) * subbuf_size - padding;
	padding_end = (read_subbuf + 1) * subbuf_size;
	if (read_pos >= padding_start && read_pos < padding_end) {
		read_subbuf = (read_subbuf + 1) % n_subbufs;
		read_pos = read_subbuf * subbuf_size;
	}

	return read_pos;
}

static size_t relay_file_read_end_pos(struct rchan_buf *buf,
									  size_t read_pos,
									  size_t count) {
	size_t read_subbuf, padding, end_pos;
	size_t subbuf_size = buf->chan->subbuf_size;
	size_t n_subbufs = buf->chan->n_subbufs;

	read_subbuf = read_pos / subbuf_size;
	padding = buf->padding[read_subbuf];
	if (read_pos % subbuf_size + count + padding == subbuf_size)
		end_pos = (read_subbuf + 1) * subbuf_size;
	else
		end_pos = read_pos + count;
	if (end_pos >= subbuf_size * n_subbufs)
		end_pos = 0;

	return end_pos;
}

static ssize_t relay_file_read(struct file *filp,
							   char __user *buffer,
							   size_t count,
							   loff_t *ppos) {
	struct rchan_buf *buf = filp->private_data;
	size_t read_start, avail;
	size_t written = 0;
	int ret;

	if (!count)
		return 0;

#pragma GCC warning "VI_LOCK() instead of inode_lock(), OK?"

	VI_LOCK(file_inode(filp));
	do {
		void *from;

		if (!relay_file_read_avail(buf, *ppos))
			break;

		read_start = relay_file_read_start_pos(*ppos, buf);
		avail = relay_file_read_subbuf_avail(read_start, buf);
		if (!avail)
			break;

		avail = min(count, avail);
		from = (char*)buf->start + read_start;
		ret = avail;
		if (copy_to_user(buffer, from, avail))
			break;

		buffer += ret;
		written += ret;
		count -= ret;

		relay_file_read_consume(buf, read_start, ret);
		*ppos = relay_file_read_end_pos(buf, read_start, ret);
	} while (count);
	VI_UNLOCK(file_inode(filp));

	return written;
}



const struct file_operations relay_file_operations = {
	.open		= relay_file_open,
	.poll		= relay_file_poll,
	/* .mmap		= relay_file_mmap, */
	.read		= relay_file_read,
	.llseek		= no_llseek,
	.release	= relay_file_release,
	/* .splice_read	= relay_file_splice_read, */
};
