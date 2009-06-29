/*
 * mmap engine
 *
 * IO engine that reads/writes from files by doing memcpy to/from
 * a memory mapped region of the file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "../fio.h"
#include "../verify.h"

/*
 * Limits us to 1GB of mapped files in total
 */
#define MMAP_TOTAL_SZ	(1 * 1024 * 1024 * 1024UL)

static unsigned long mmap_map_size;
static unsigned long mmap_map_mask;

static int fio_mmap_file(struct thread_data *td, struct fio_file *f,
			 size_t length, off_t off)
{
	int flags = 0;
	int ret = 0;

	if (td_rw(td))
		flags = PROT_READ | PROT_WRITE;
	else if (td_write(td)) {
		flags = PROT_WRITE;

		if (td->o.verify != VERIFY_NONE)
			flags |= PROT_READ;
	} else
		flags = PROT_READ;

	f->mmap_ptr = mmap(NULL, length, flags, MAP_SHARED, f->fd, off);
	if (f->mmap_ptr == MAP_FAILED) {
		int err = errno;

		f->mmap_ptr = NULL;
		td_verror(td, err, "mmap");
		goto err;
	}

	if (!td_random(td)) {
		if (madvise(f->mmap_ptr, length, MADV_SEQUENTIAL) < 0) {
			td_verror(td, errno, "madvise");
			goto err;
		}
	} else {
		if (madvise(f->mmap_ptr, length, MADV_RANDOM) < 0) {
			td_verror(td, errno, "madvise");
			goto err;
		}
	}

err:
	return ret;
}

static int fio_mmapio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret = 0;

	if (io_u->buflen > mmap_map_size) {
		log_err("fio: bs too big for mmap engine\n");
		ret = EIO;
		goto err;
	}

	if (io_u->offset >= f->mmap_off &&
	    io_u->offset + io_u->buflen < f->mmap_off + f->mmap_sz)
		goto done;

	if (f->mmap_ptr) {
		if (munmap(f->mmap_ptr, f->mmap_sz) < 0) {
			ret = errno;
			goto err;
		}
		f->mmap_ptr = NULL;
	}

	f->mmap_sz = mmap_map_size;
	if (f->mmap_sz  > f->io_size)
		f->mmap_sz = f->io_size;

	f->mmap_off = io_u->offset;

	ret = fio_mmap_file(td, f, f->mmap_sz, f->mmap_off);
done:
	if (!ret)
		io_u->mmap_data = f->mmap_ptr + io_u->offset - f->mmap_off -
					f->file_offset;
err:
	return ret;
}

static int fio_mmapio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		memcpy(io_u->xfer_buf, io_u->mmap_data, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_WRITE)
		memcpy(io_u->mmap_data, io_u->xfer_buf, io_u->xfer_buflen);
	else if (ddir_sync(io_u->ddir)) {
		if (msync(f->mmap_ptr, f->mmap_sz, MS_SYNC)) {
			io_u->error = errno;
			td_verror(td, io_u->error, "msync");
		}
	}

	/*
	 * not really direct, but should drop the pages from the cache
	 */
	if (td->o.odirect && !ddir_sync(io_u->ddir)) {
		if (msync(io_u->mmap_data, io_u->xfer_buflen, MS_SYNC) < 0) {
			io_u->error = errno;
			td_verror(td, io_u->error, "msync");
		}
		if (madvise(io_u->mmap_data, io_u->xfer_buflen,  MADV_DONTNEED) < 0) {
			io_u->error = errno;
			td_verror(td, io_u->error, "madvise");
		}
	}

	return FIO_Q_COMPLETED;
}

static int fio_mmapio_init(struct thread_data *td)
{
	unsigned long shift, mask;

	mmap_map_size = MMAP_TOTAL_SZ / td->o.nr_files;
	mask = mmap_map_size;
	shift = 0;
	do {
		mask >>= 1;
		if (!mask)
			break;
		shift++;
	} while (1);
		
	mmap_map_mask = 1UL << shift;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "mmap",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_mmapio_init,
	.prep		= fio_mmapio_prep,
	.queue		= fio_mmapio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_NOEXTEND,
};

static void fio_init fio_mmapio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_mmapio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
