/*
 * Copyright (C) 2005-2009 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * file operations for special files.
 * while they exist in aufs virtually,
 * their file I/O is handled out of aufs.
 */

#include <linux/fs_stack.h>
#include "aufs.h"

static ssize_t aufs_aio_read_sp(struct kiocb *kio, const struct iovec *iov,
				unsigned long nv, loff_t pos)
{
	ssize_t err;
	aufs_bindex_t bstart;
	unsigned char wbr;
	struct file *file, *h_file;
	struct super_block *sb;

	file = kio->ki_filp;
	sb = file->f_dentry->d_sb;
	si_read_lock(sb, AuLock_FLUSH);
	fi_read_lock(file);
	bstart = au_fbstart(file);
	h_file = au_h_fptr(file, bstart);
	fi_read_unlock(file);
	wbr = !!au_br_writable(au_sbr(sb, bstart)->br_perm);
	si_read_unlock(sb);

	/* do not change the file in kio */
	AuDebugOn(!h_file->f_op || !h_file->f_op->aio_read);
	err = h_file->f_op->aio_read(kio, iov, nv, pos);
	if (err > 0 && wbr)
		file_accessed(h_file);

	return err;
}

static ssize_t aufs_aio_write_sp(struct kiocb *kio, const struct iovec *iov,
				 unsigned long nv, loff_t pos)
{
	ssize_t err;
	aufs_bindex_t bstart;
	unsigned char wbr;
	struct super_block *sb;
	struct file *file, *h_file;

	file = kio->ki_filp;
	sb = file->f_dentry->d_sb;
	si_read_lock(sb, AuLock_FLUSH);
	fi_read_lock(file);
	bstart = au_fbstart(file);
	h_file = au_h_fptr(file, bstart);
	fi_read_unlock(file);
	wbr = !!au_br_writable(au_sbr(sb, bstart)->br_perm);
	si_read_unlock(sb);

	/* do not change the file in kio */
	AuDebugOn(!h_file->f_op || !h_file->f_op->aio_write);
	err = h_file->f_op->aio_write(kio, iov, nv, pos);
	if (err > 0 && wbr)
		file_update_time(h_file);

	return err;
}

/* ---------------------------------------------------------------------- */

static int aufs_release_sp(struct inode *inode __maybe_unused, struct file *file)
{
	int err;
	struct file *h_file;

	fi_read_lock(file);
	h_file = au_h_fptr(file, au_fbstart(file));
	fi_read_unlock(file);
	/* close this fifo in aufs */
	err = h_file->f_op->release(inode, file); /* ignore */
	aufs_release_nondir(inode, file); /* ignore */
	return err;
}

/* ---------------------------------------------------------------------- */

/* currently, support only FIFO */
enum {AuSp_FIFO, AuSp_FIFO_R, AuSp_FIFO_W, AuSp_FIFO_RW,
      /* AuSp_SOCK, AuSp_CHR, AuSp_BLK, */
      AuSp_Last};
static int aufs_open_sp(struct inode *inode, struct file *file);
static struct au_sp_fop {
	volatile int		done;
	struct file_operations	fop;	/* not 'const' */
	spinlock_t		spin;
} au_sp_fop[AuSp_Last] = {
	[AuSp_FIFO] = {
		.fop	= {
			.open	= aufs_open_sp
		}
	}
};

static void au_init_fop_sp(struct file *file)
{
	struct au_sp_fop *p;
	int i;
	struct file *h_file;

	p = au_sp_fop;
	if (unlikely(!p->done)) {
		/* initialize first time only */
		static DEFINE_SPINLOCK(spin);

		spin_lock(&spin);
		if (!p->done) {
			BUILD_BUG_ON(sizeof(au_sp_fop)/sizeof(*au_sp_fop)
				     != AuSp_Last);
			for (i = 0; i < AuSp_Last; i++)
				spin_lock_init(&p[i].spin);
			p->done = 1;
		}
		spin_unlock(&spin);
	}

	switch (file->f_mode & (FMODE_READ | FMODE_WRITE)) {
		case FMODE_READ:
			i = AuSp_FIFO_R;
			break;
		case FMODE_WRITE:
			i = AuSp_FIFO_W;
			break;
		case FMODE_READ | FMODE_WRITE:
			i = AuSp_FIFO_RW;
			break;
		default:
			BUG();
	}

	p += i;
	if (unlikely(!p->done)) {
		/* initialize first time only */
		h_file = au_h_fptr(file, au_fbstart(file));
		spin_lock(&p->spin);
		if (!p->done) {
			p->fop = *h_file->f_op;
			if (p->fop.aio_read)
				p->fop.aio_read = aufs_aio_read_sp;
			if (p->fop.aio_write)
				p->fop.aio_write = aufs_aio_write_sp;
			p->fop.release = aufs_release_sp;
			p->done = 1;
		}
		spin_unlock(&p->spin);
	}
	file->f_op = &p->fop;
}

static int au_cpup_sp(struct dentry *dentry, aufs_bindex_t bcpup)
{
	int err;
	struct au_pin pin;
	struct dentry *parent;

	AuDbg("%.*s\n", AuDLNPair(dentry));

	err = 0;
	parent = dget_parent(dentry);
	di_write_lock_parent(parent);
	if (!au_h_dptr(parent, bcpup)) {
		err = au_cpup_dirs(dentry, bcpup);
		if (unlikely(err))
			goto out;
	}

	err = au_pin(&pin, dentry, bcpup, AuOpt_UDBA_NONE,
		     AuPin_DI_LOCKED | AuPin_MNT_WRITE);
	if (!err) {
		err = au_sio_cpup_simple(dentry, bcpup, -1, AuCpup_DTIME);
		au_unpin(&pin);
	}

 out:
	di_write_unlock(parent);
	dput(parent);
	return err;
}

static int au_do_open_sp(struct file *file, int flags)
{
	int err;
	aufs_bindex_t bcpup, bindex, bend;
	struct dentry *dentry;
	struct super_block *sb;
	struct file *h_file;
	struct inode *h_inode;

	dentry = file->f_dentry;
	AuDbg("%.*s\n", AuDLNPair(dentry));

	err = 0;
	sb = dentry->d_sb;
	bend = au_dbstart(dentry);
	if (au_br_rdonly(au_sbr(sb, bend))) {
		/* copyup first */
		bcpup = -1;
		for (bindex = 0; bindex < bend; bindex++)
			if (!au_br_rdonly(au_sbr(sb, bindex))) {
				bcpup = bindex;
				break;
			}
		if (bcpup >= 0) {
			/* need to copyup */
			di_read_unlock(dentry, AuLock_IR);
			di_write_lock_child(dentry);
			if (bcpup < au_dbstart(dentry))
				err = au_cpup_sp(dentry, bcpup);
			di_downgrade_lock(dentry, AuLock_IR);
		} else
			err = -EROFS;
	}
	if (unlikely(err))
		goto out;

	/* prepare h_file */
	err = au_do_open_nondir(file, file->f_flags);
	if (unlikely(err))
		goto out;

	h_file = au_h_fptr(file, au_fbstart(file));
	h_inode = h_file->f_dentry->d_inode;
	di_read_unlock(dentry, AuLock_IR);
	fi_write_unlock(file);
	si_read_unlock(sb);
	/* open this fifo in aufs */
	err = h_inode->i_fop->open(file->f_dentry->d_inode, file);
	si_noflush_read_lock(sb);
	fi_write_lock(file);
	di_read_lock_child(dentry, AuLock_IR);
	if (!err) {
		au_init_fop_sp(file);
		goto out; /* success */
	}

	au_finfo_fin(file);

 out:
	return err;
}

static int aufs_open_sp(struct inode *inode, struct file *file)
{
	return au_do_open(file, au_do_open_sp);
}

/* ---------------------------------------------------------------------- */

void au_init_special_fop(struct inode *inode, umode_t mode, dev_t rdev)
{
	init_special_inode(inode, mode, rdev);

	switch (mode & S_IFMT) {
	case S_IFIFO:
		inode->i_fop = &au_sp_fop[AuSp_FIFO].fop;
		/*FALLTHROUGH*/
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
		break;
	default:
		AuDebugOn(1);
	}
}

int au_special_file(umode_t mode)
{
	int ret;

	ret = 0;
	switch (mode & S_IFMT) {
	case S_IFIFO:
#if 0
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
#endif
		ret = 1;
	}

	return ret;
}