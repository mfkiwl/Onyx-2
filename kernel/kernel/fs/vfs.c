/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <libgen.h>
#include <limits.h>

#include <onyx/panic.h>
#include <onyx/vfs.h>
#include <onyx/dev.h>
#include <onyx/pagecache.h>
#include <onyx/log.h>
#include <onyx/mtable.h>
#include <onyx/sysfs.h>
#include <onyx/fnv.h>
#include <onyx/object.h>
#include <onyx/process.h>
#include <onyx/dentry.h>
#include <onyx/mm/flush.h>
#include <onyx/vm.h>
#include <onyx/clock.h>

struct file *fs_root = NULL;
struct file *mount_list = NULL;

ssize_t write_file_cache(void *buffer, size_t sizeofwrite, struct inode *file, off_t offset);
bool inode_is_cacheable(struct inode *file);

#define FILE_CACHING_READ	(0 << 0)
#define FILE_CACHING_WRITE	(1 << 0)

struct filesystem_root
{
	struct object object;
	struct file *file;
	struct dentry *root_dentry;
};

struct filesystem_root boot_root = {0};

int vfs_init(void)
{
	object_init(&boot_root.object, NULL);
	//dentry_init();

	return 0;
}

struct filesystem_root *get_filesystem_root(void)
{
	struct process *p = get_current_process();
	if(!p)
		return &boot_root;

	return &boot_root;
}

struct file *get_fs_root(void)
{
	struct filesystem_root *root = get_filesystem_root();

	return root->file;
}

#ifdef CONFIG_CHECK_PAGE_CACHE_INTEGRITY
uint32_t crc32_calculate(uint8_t *ptr, size_t len);

#endif

/* This function trims the part of the page that wasn't read in(because the segment of
 * the file is smaller than PAGE_SIZE).
 */
static void zero_rest_of_page(struct page *page, size_t to_read)
{
	unsigned char *buf = PAGE_TO_VIRT(page) + to_read;

	size_t to_zero = PAGE_SIZE - to_read;

	memset(buf, 0, to_zero);
} 

struct page *vmo_inode_commit(size_t off, struct vm_object *vmo)
{
	struct inode *i = vmo->ino;

	struct page *page = alloc_page(PAGE_ALLOC_NO_ZERO);
	if(!page)
		return NULL;
	page->flags |= PAGE_FLAG_BUFFER;

	size_t to_read = i->i_size - off < PAGE_SIZE ? i->i_size - off : PAGE_SIZE;

	assert(to_read <= PAGE_SIZE);

	unsigned long old = thread_change_addr_limit(VM_KERNEL_ADDR_LIMIT);

	assert(i->i_fops->readpage != NULL);
	ssize_t read = i->i_fops->readpage(page, off, i);

	thread_change_addr_limit(old);

	if(read != (ssize_t) to_read)
	{
		printk("Error file read %lx bytes out of %lx, off %lx\n", read, to_read, off);
		perror("file");
		/* TODO: clean up */
		free_page(page);
		return NULL;
	}

	zero_rest_of_page(page, to_read);

	if(!pagecache_create_cache_block(page, read, off, i))
	{
		free_page(page);
		return NULL;
	}

	return page;
}

int inode_create_vmo(struct inode *ino)
{
	ino->i_pages = vmo_create(ino->i_size, NULL);
	if(!ino->i_pages)
		return -1;
	ino->i_pages->commit = vmo_inode_commit;
	ino->i_pages->ino = ino;
	return 0;
}

struct page_cache_block *inode_get_cache_block(struct inode *ino, size_t off)
{
	MUST_HOLD_LOCK(&ino->i_pages_lock);

	if(!ino->i_pages)
	{
		if(inode_create_vmo(ino) < 0)
			return NULL;
	}

	if(off >= ino->i_pages->size)
	{
		ino->i_pages->size += (off - ino->i_pages->size) + PAGE_SIZE;
	}

	struct page *p = vmo_get(ino->i_pages, off, VMO_GET_MAY_POPULATE);
	if(!p)
		return NULL;

	return p->cache;
}

struct page_cache_block *__inode_get_page_internal(struct inode *inode, size_t offset, long flags)
{
	off_t aligned_off = (offset / PAGE_CACHE_SIZE) * PAGE_CACHE_SIZE;

	MUST_HOLD_LOCK(&inode->i_pages_lock);
	struct page_cache_block *b = inode_get_cache_block(inode, aligned_off);
	
	return b;
}

struct page_cache_block *inode_get_page(struct inode *inode, off_t offset, long flags)
{
	spin_lock_preempt(&inode->i_pages_lock);

	struct page_cache_block *b = __inode_get_page_internal(inode, offset, flags);

	/* No need to pin the page since it's already pinned by vmo_get */

	spin_unlock_preempt(&inode->i_pages_lock);

	return b;
}

void inode_update_atime(struct inode *ino)
{
	ino->i_atime = clock_get_posix_time();
	inode_mark_dirty(ino);
}

void inode_update_ctime(struct inode *ino)
{
	ino->i_ctime = clock_get_posix_time();
	inode_mark_dirty(ino);
}

void inode_update_mtime(struct inode *ino)
{
	ino->i_mtime = clock_get_posix_time();
	inode_mark_dirty(ino);
}

ssize_t do_actual_read(size_t offset, size_t len, void *buf, struct file *file)
{
	if(!inode_is_cacheable(file->f_ino))
		return file->f_ino->i_fops->read(offset, len, buf, file);
	
	return lookup_file_cache(buf, len, file->f_ino, offset);
}

bool is_invalid_length(size_t len)
{
	return ((ssize_t) len) < 0;
}

size_t clamp_length(size_t len)
{
	if(is_invalid_length(len))
		len = SSIZE_MAX;
	return len;
}

ssize_t read_vfs(size_t offset, size_t len, void *buffer, struct file *file)
{
	struct inode *ino = file->f_ino;
	if(ino->i_type & VFS_TYPE_DIR)
		return errno = EISDIR, -1;
	
	if(!ino->i_fops->read)
		return errno = EIO, -1;

	len = clamp_length(len);

	ssize_t res = do_actual_read(offset, len, buffer, file);

	if(res >= 0)
	{
		inode_update_atime(ino);
	}

	return res;
}

ssize_t do_actual_write(size_t offset, size_t len, void *buffer, struct file *f)
{
	ssize_t st = 0;
	struct inode *ino = f->f_ino;

	if(!inode_is_cacheable(ino))
	{
		st = ino->i_fops->write(offset, len, buffer, f);
	}
	else
	{
		st = write_file_cache(buffer, len, ino, offset);
	}

	if(st >= 0)
	{
		inode_update_mtime(ino);
	}
	
	return st;
}

ssize_t write_vfs(size_t offset, size_t len, void *buffer, struct file *f)
{
	struct inode *ino = f->f_ino;
	if(ino->i_type & VFS_TYPE_DIR)
		return errno = EISDIR, -1;
	
	if(!ino->i_fops->write)
		return errno = EIO, -1;

	len = clamp_length(len);
	
	ssize_t res = do_actual_write(offset, len, buffer, f);

	return res;
}

int ioctl_vfs(int request, char *argp, struct file *this)
{
	if(this->f_ino->i_fops->ioctl != NULL)
		return this->f_ino->i_fops->ioctl(request, (void*) argp, this);
	return -ENOSYS;
}

void close_vfs(struct inode *this)
{
	object_unref(&this->i_object);
}

struct file *do_actual_open(struct file *this, const char *name)
{
	assert(this != NULL);

	if(this->f_ino->i_fops->open == NULL)
		return errno = EIO, NULL;

	struct inode *i = this->f_ino->i_fops->open(this, name);

	if(!i)
		return NULL;
	
	struct file *f = inode_to_file(i);
	if(!f)
	{
		close_vfs(i);
		return NULL;
	}

	if(f->f_ino->i_fops->on_open)
	{
		if(f->f_ino->i_fops->on_open(f) < 0)
		{
			fd_put(f);
			return NULL;
		}		
	}

	return f;
}

char *readlink_vfs(struct file *file)
{
	if(file->f_ino->i_fops->readlink)
	{
		char *p = file->f_ino->i_fops->readlink(file);
		if(p != NULL)
			inode_update_atime(file->f_ino);
		
		return p;
	}

	return errno = EINVAL, NULL;
}

struct file *follow_symlink(struct file *file, struct file *parent)
{
	char *symlink = readlink_vfs(file);
	if(!symlink)
		return NULL;

	struct file *ret = open_vfs(parent, symlink);

	free(symlink);

	return ret;
}

bool file_can_access(struct file *f, unsigned int perms)
{
	bool access_good = true;
	struct creds *c = creds_get();
	struct inode *file = f->f_ino;

	if(unlikely(c->euid == 0))
	{
		/* We're root: the access is good */
		goto out;
	}

	/* We're not root, let's do permission checking */

	/* Case 1 -  we're the owners of the file (file->uid == c->euid) */

	/* We're going to transform FILE_ACCESS_* constants (our perms var) into UNIX permissions */
	mode_t ino_perms;

	if(likely(file->i_uid == c->euid))
	{
		ino_perms = ((perms & FILE_ACCESS_READ) ? S_IRUSR : 0) |
                    ((perms & FILE_ACCESS_WRITE) ? S_IWUSR : 0) |
					((perms & FILE_ACCESS_EXECUTE) ? S_IXUSR : 0);
	}
	else if(file->i_gid == c->egid)
	{
		/* Case 2 - we're in the same group as the file */
		ino_perms = ((perms & FILE_ACCESS_READ) ? S_IRGRP : 0) |
                    ((perms & FILE_ACCESS_WRITE) ? S_IWGRP : 0) |
					((perms & FILE_ACCESS_EXECUTE) ? S_IXGRP : 0);
	}
	else
	{
		/* Case 3 - others permissions apply */
		ino_perms = ((perms & FILE_ACCESS_READ) ? S_IROTH : 0) |
                    ((perms & FILE_ACCESS_WRITE) ? S_IWOTH : 0) |
					((perms & FILE_ACCESS_EXECUTE) ? S_IXOTH : 0);
	}

	/* Now, test the calculated permission bits against the file's mode */

	access_good = (file->i_mode & ino_perms) == ino_perms;

#if 0
	if(!access_good)
	{
		printk("Halting for debug: ino perms %u, perms %u\n", ino_perms, file->i_mode);
		while(true) {}
	}
#endif
out:
	creds_put(c);
	return access_good;
}

struct file *open_path_segment(char *segm, struct file *node)
{
	/* Let's check if we have read access to the directory before doing anything */
	if(!file_can_access(node, FILE_ACCESS_READ))
	{
		return errno = EACCES, NULL;
	}

	struct file *file = do_actual_open(node, segm);
	if(!file)
		return NULL;

	if(file->f_ino->i_type == VFS_TYPE_SYMLINK)
	{
		struct file *target = follow_symlink(file, node);
		if(!target)
			return NULL;
		
		fd_put(file);
		file = target;
	}

	struct file *mountpoint = NULL;
	if((mountpoint = mtable_lookup(file)))
	{
		fd_put(file);
		file = mountpoint;
	}

	return file;
}

struct file *open_vfs(struct file* this, const char *name)
{
	/* Okay, so we need to traverse the path */
	/* First off, dupe the string */
	char *path = strdup(name);
	if(!path)
		return errno = ENOMEM, NULL;
	char *saveptr;
	char *orig = path;

	/* Now, tokenize it using strtok */
	path = strtok_r(path, "/", &saveptr);
	struct file *node = this;

	while(path)
	{
		struct file *new_node = open_path_segment(path, node);

		if(node != this)
			fd_put(node);

		node = new_node;
		if(!node)
		{
#if 0
			perror("open_path_segment");
			printk("Failed opening %s, segment %s\n", name, path);
#endif
			free(orig);
			return NULL;
		}

		path = strtok_r(NULL, "/", &saveptr);
	}

	free(orig);

	if(node == this)
		fd_get(node);

	return node;
}

struct file *creat_vfs(struct file *this, const char *path, int mode)
{
	char *dup = strdup(path);
	if(!dup)
		return errno = ENOMEM, NULL;

	char *dir = dirname((char*) dup);

	struct file *base;
	if(*dir != '.' && strlen(dir) != 1)
		base = open_vfs(this, dir);
	else
		base = this;

	/* Reset the string again */
	strcpy(dup, path);
	if(!base)
	{
		errno = ENOENT;
		goto error;
	}

	if(!file_can_access(base, FILE_ACCESS_WRITE))
	{
		fd_put(base);
		errno = EACCES;
		goto error;
	}

	if(base->f_ino->i_fops->creat == NULL)
		goto error_nosys;
	struct inode *ret = base->f_ino->i_fops->creat(basename((char*) dup), mode, base);
	
	free(dup);
	
	struct file *f = inode_to_file(ret);
	if(!f)
		close_vfs(ret);

	return f;

error_nosys:
	errno = ENOSYS;

error:
	free(dup);
	return NULL;
}

struct file *mkdir_vfs(const char *path, mode_t mode, struct file *this)
{
	char *dup = strdup(path);
	if(!dup)
		return errno = ENOMEM, NULL;

	char *dir = dirname((char*) dup);
	struct file *base;
	if(*dir != '.' && strlen(dir) != 1)
		base = open_vfs(this, dir);
	else
		base = this;

	/* Reset the string again */
	strcpy(dup, path);
	if(!base)
	{
		errno = ENOENT;
		goto error;
	}

	if(!file_can_access(base, FILE_ACCESS_WRITE))
	{
		fd_put(base);
		errno = EACCES;
		goto error;
	}

	if(base->f_ino->i_fops->mkdir == NULL)
		goto error_nosys;

	struct inode *ret = base->f_ino->i_fops->mkdir(basename((char*) dup), mode, base);
	free(dup);

	struct file *f = inode_to_file(ret);
	if(!f)
		close_vfs(ret);

	return f;

error_nosys:
	errno = ENOSYS;

error:
	free(dup);
	return NULL;
}

struct file *mknod_vfs(const char *path, mode_t mode, dev_t dev, struct file *this)
{
	char *dup = strdup(path);
	if(!dup)
		return errno = ENOMEM, NULL;

	char *dir = dirname((char*) dup);
	struct file *base;
	if(*dir != '.' && strlen(dir) != 1)
		base = open_vfs(this, dir);
	else
		base = this;

	/* Reset the string again */
	strcpy(dup, path);
	if(!base)
	{
		errno = ENOENT;
		goto error;
	}

	if(!file_can_access(base, FILE_ACCESS_WRITE))
	{
		fd_put(base);
		errno = EACCES;
		goto error;
	}

	if(base->f_ino->i_fops->mknod == NULL)
		goto error_nosys;

	struct inode *ret = base->f_ino->i_fops->mknod(basename((char*) dup), mode, dev, base);
	free(dup);
	
	struct file *f = inode_to_file(ret);
	if(!f)
		close_vfs(ret);

	return f;

error_nosys:
	errno = ENOSYS;

error:
	free(dup);
	return NULL;
}

int mount_fs(struct inode *fsroot, const char *path)
{
	assert(fsroot != NULL);

	printf("mount_fs: Mounting on %s\n", path);
	
	if(strcmp((char*) path, "/") == 0)
	{
		struct file *f = zalloc(sizeof *f);
		if(!f)
			return -ENOMEM;
		f->f_ino = fsroot;
		f->f_refcount = 1;

		if(boot_root.file)
		{
			fd_put(boot_root.file);
		}

		boot_root.file = f;
	}
	else
	{
		/* TODO: This seems iffy logic, at best */
		struct file *file = open_vfs(get_fs_root(), dirname((char*) path));
		if(!file)
			return -errno;
		file = do_actual_open(file, basename((char*) path));
		if(!file)
			return -errno;
		struct file *fsroot_f = inode_to_file(fsroot);
		if(!fsroot_f)
		{
			fd_put(file);
			return -ENOMEM;
		}
	
		return mtable_mount(file, fsroot_f);
	}
	return 0;
}

off_t do_getdirent(struct dirent *buf, off_t off, struct file *file)
{
	if(file->f_ino->i_fops->getdirent != NULL)
		return file->f_ino->i_fops->getdirent(buf, off, file);
	return -ENOSYS;
}

unsigned int putdir(struct dirent *buf, struct dirent *ubuf,
	unsigned int count)
{
	unsigned int reclen = buf->d_reclen;
	
	if(reclen > count)
		return errno = EINVAL, -1;

	if(copy_to_user(ubuf, buf, reclen) < 0)
	{
		errno = EFAULT;
		return -1;
	}

	return reclen > count ? count : reclen;
}

int getdents_vfs(unsigned int count, putdir_t putdir,
	struct dirent* dirp, off_t off, struct getdents_ret *ret,
	struct file *f)
{
	if(!(f->f_ino->i_type & VFS_TYPE_DIR))
		return errno = ENOTDIR, -1;
	
	/*printk("Seek: %lu\n", off);
	printk("Count: %u\n", count);*/
	struct dirent buf;
	unsigned int pos = 0;
	
	while(pos < count)
	{
		off_t of = do_getdirent(&buf, off, f);
		
		//printk("Dirent: %s\n", buf.d_name);
		if(of == 0)
		{
			if(pos)
				return pos;
			return 0;
		}

		/* Error, return -1 with errno set */
		if(of < 0)
			return errno = -of, -1;

		/* Put the dirent in the user-space buffer */
		unsigned int written = putdir(&buf, dirp, count);
		/* Error, most likely out of buffer space */
		if(written == (unsigned int) -1)
		{
			if(!pos) return -1;
			else
				return pos;
		}

		pos += written;
		dirp = (void*) (char *) dirp + written;
		off = of;
		ret->read = pos;
		ret->new_off = off;
	}

	return pos; 
}

int stat_vfs(struct stat *buf, struct file *node)
{
	if(node->f_ino->i_fops->stat != NULL)
		return node->f_ino->i_fops->stat(buf, node);
	
	return errno = ENOSYS, (unsigned int) -1;
}

short default_poll(void *poll_table, short events, struct file *node);

short poll_vfs(void *poll_file, short events, struct file *node)
{
	if(node->f_ino->i_fops->poll != NULL)
		return node->f_ino->i_fops->poll(poll_file, events, node);
	
	return default_poll(poll_file, events, node);
}

bool inode_is_cacheable(struct inode *ino)
{
	if(ino->i_flags & INODE_FLAG_DONT_CACHE)
		return false;
	if(ino->i_type != VFS_TYPE_FILE)
		return false;

	return true;
}

ssize_t lookup_file_cache(void *buffer, size_t sizeofread, struct inode *file,
	off_t offset)
{
	if(!inode_is_cacheable(file))
		return -1;

	if((size_t) offset >= file->i_size)
		return 0;

	size_t read = 0;

	while(read != sizeofread)
	{
		struct page_cache_block *cache = inode_get_page(file, offset, FILE_CACHING_READ);

		if(!cache)
		{
			if(read)
			{
				return read;
			}
			else
			{
				errno = ENOMEM;
				return -1;
			}
		}

		struct page *page = cache->page;

		off_t cache_off = offset % PAGE_CACHE_SIZE;
		off_t rest = PAGE_CACHE_SIZE - cache_off;

		assert(rest > 0);
	
		size_t amount = sizeofread - read < (size_t) rest ?
			sizeofread - read : (size_t) rest;

		if(offset + amount > file->i_size)
		{
			amount = file->i_size - offset;
			if(copy_to_user((char*) buffer + read, (char*) cache->buffer +
				cache_off, amount) < 0)
			{
				page_unpin(page);
				errno = EFAULT;
				return -1;
			}

			page_unpin(page);
			return read + amount;
		}
		else
		{
			if(copy_to_user((char*) buffer + read,  (char*) cache->buffer +
				cache_off, amount) < 0)
			{
				page_unpin(page);
				errno = EFAULT;
				return -1;
			}
		}

		offset += amount;
		read += amount;

		page_unpin(page);
	}

	return (ssize_t) read;
}

ssize_t write_file_cache(void *buffer, size_t len, struct inode *ino,
	off_t offset)
{
	if(!inode_is_cacheable(ino))
		return -1;

	size_t wrote = 0;
	do
	{
		/* Adjust the file size if needed */
		if(offset + len > ino->i_size)
		{
			/* TODO: Adjust the block count */
			ino->i_size = offset + len;
			inode_update_ctime(ino);
			inode_mark_dirty(ino);
		}
	
		struct page_cache_block *cache = inode_get_page(ino, offset,
						  FILE_CACHING_WRITE);

		if(cache == NULL)
		{
			if(wrote)
			{
				return wrote;
			}
			else
			{
				errno = ENOMEM;
				return -1;
			}
		}

		struct page *page = cache->page;

		off_t cache_off = offset % PAGE_CACHE_SIZE;
		off_t rest = PAGE_CACHE_SIZE - cache_off;

		size_t amount = len - wrote < (size_t) rest ?
			len - wrote : (size_t) rest;

		if(copy_from_user((char*) cache->buffer + cache_off, (char*) buffer +
			wrote, amount) < 0)
		{
			page_unpin(page);
			errno = EFAULT;
			return -1;
		}
	
		if(cache->size < cache_off + amount)
		{
			cache->size = cache_off + amount;
		}

		pagecache_dirty_block(cache);

		page_unpin(page);
	
		offset += amount;
		wrote += amount;

	} while(wrote != len);

	return (ssize_t) wrote;
}

int default_ftruncate(off_t length, struct file *f)
{
	if(length < 0)
		return -EINVAL;
	struct inode *vnode = f->f_ino;
	
	if((size_t) length <= vnode->i_size)
	{
		/* Possible memory/disk leak, but filesystems should handle it */
		vnode->i_size = (size_t) length;
		return 0;
	}

	char *page = zalloc(PAGE_SIZE);
	if(!page)
	{
		return -ENOMEM;
	}

	printk("Default ftruncate\n");

	size_t length_diff = (size_t) length - vnode->i_size;
	size_t off = vnode->i_size;

	while(length_diff != 0)
	{
		size_t to_write = length_diff >= PAGE_SIZE ? PAGE_SIZE : length_diff;

		unsigned long old = thread_change_addr_limit(VM_KERNEL_ADDR_LIMIT);
		size_t written = write_vfs(off, to_write, page, f);
		
		thread_change_addr_limit(old);
		if(written != to_write)
		{
			free(page);
			return -errno;
		}

		off += to_write;
		length_diff -= to_write;
	}

	free(page);

	return 0;
}

int ftruncate_vfs(off_t length, struct file *vnode)
{
	if(length < 0)
		return -EINVAL;

	if(vnode->f_ino->i_fops->ftruncate != NULL)
		return vnode->f_ino->i_fops->ftruncate(length, vnode);
	else
	{
		return default_ftruncate(length, vnode);
	}

	return -ENOSYS;
}

int default_fallocate(int mode, off_t offset, off_t len, struct file *file)
{
	/* VERY VERY VERY VERY VERY quick and dirty implementation to satisfy /bin/ld(.gold) */
	if(mode != 0)
		return -EINVAL;

	char *page = zalloc(PAGE_SIZE);
	if(!page)
	{
		return -ENOMEM;
	}

	size_t length_diff = (size_t) len;
	size_t off = off;
	while(length_diff != 0)
	{
		size_t to_write = length_diff >= PAGE_SIZE ? PAGE_SIZE : length_diff;

		size_t written = write_vfs(off, to_write, page, file);

		if(written != to_write)
		{
			free(page);
			return (int) written;
		}

		off += to_write;
		length_diff -= to_write;
	}

	free(page);

	return 0;
}

int fallocate_vfs(int mode, off_t offset, off_t len, struct file *file)
{
	if(file->f_ino->i_fops->fallocate)
	{
		return file->f_ino->i_fops->fallocate(mode, offset, len, file);
	}
	else
		return default_fallocate(mode, offset, len, file);

	return -EINVAL;
}

int symlink_vfs(const char *dest, struct file *inode)
{
	if(!file_can_access(inode, FILE_ACCESS_WRITE))
		return -EACCES;

	if(inode->f_ino->i_fops->symlink != NULL)
		return inode->f_ino->i_fops->symlink(dest, inode);
	return -ENOSYS;
}

void inode_destroy_page_caches(struct inode *inode)
{
	if(inode->i_pages)
		vmo_unref(inode->i_pages);
}

ssize_t inode_sync(struct inode *inode)
{
	struct rb_itor it;
	it.node = NULL;
	mutex_lock(&inode->i_pages->page_lock);

	it.tree = inode->i_pages->pages;

	while(rb_itor_valid(&it))
	{
		void *datum = *rb_itor_datum(&it);
		struct page_cache_block *b = datum;
		struct page *page = b->page;

		if(page->flags & PAGE_FLAG_DIRTY)
		{
			flush_sync_one(&b->fobj);
		}

		rb_itor_next(&it);
	}

	/* TODO: Return errors */
	mutex_unlock(&inode->i_pages->page_lock);
	return 0;
}

void inode_release(struct object *object)
{
	struct inode *inode = (struct inode *) object;

	if(inode->i_sb)
	{
		assert(inode->i_sb != NULL);

		/* Remove the inode from its superblock */
		superblock_remove_inode(inode->i_sb, inode);
	}

	if(inode->i_flags & INODE_FLAG_DIRTY)
		flush_remove_inode(inode);

	/* TODO: Detect the case where we're getting deleted and avoid sync'ing caches */
	if(inode->i_type == VFS_TYPE_FILE)
		inode_sync(inode);

	inode_destroy_page_caches(inode);

	if(inode->i_fops->close != NULL)
		inode->i_fops->close(inode);

	free(inode);
}

struct inode *inode_create(bool is_reg)
{
	struct inode *inode = zalloc(sizeof(*inode));

	if(!inode)
		return NULL;

	/* Don't release inodes immediately */
	object_init(&inode->i_object, inode_release);

	if(is_reg)
	{
		if(inode_create_vmo(inode) < 0)
		{
			free(inode);
			return NULL;
		}
	}

	return inode;
}

int link_vfs(struct file *target, const char *name, struct file *dir)
{
	if(!file_can_access(dir, FILE_ACCESS_WRITE))
		return -EACCES;

	if(dir->f_ino->i_fops->link)
		return dir->f_ino->i_fops->link(target, name, dir);
	return -EINVAL;
}

int unlink_vfs(const char *name, int flags, struct file *node)
{
	if(!file_can_access(node, FILE_ACCESS_WRITE))
		return -EACCES;
	if(node->f_ino->i_fops->link)
		return node->f_ino->i_fops->unlink(name, flags, node);
	return -EINVAL;
}

void inode_mark_dirty(struct inode *ino)
{
	unsigned long old_flags = __sync_fetch_and_or(&ino->i_flags, INODE_FLAG_DIRTY);

	__sync_synchronize();

	if(old_flags & INODE_FLAG_DIRTY)
		return;

	flush_add_inode(ino);	
}

int inode_flush(struct inode *ino)
{
	struct superblock *sb = ino->i_sb;

	if(!sb || !sb->flush_inode)
		return 0;
	
	return sb->flush_inode(ino);
}

struct file *inode_to_file(struct inode *ino)
{
	struct file *f = zalloc(sizeof(struct file));
	if(!f)
		return NULL;
	f->f_ino = ino;
	f->f_flags = 0;
	f->f_refcount = 1;
	f->f_seek = 0;

	return f;
}
