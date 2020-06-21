/*
* Copyright (c) 2020 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <errno.h>
#include <stdio.h>

#include <onyx/vfs.h>
#include <onyx/pagecache.h>
#include <onyx/page.h>
#include <onyx/vm.h>
#include <onyx/rwlock.h>
#include <onyx/panic.h>
#include <onyx/dev.h>
#include <onyx/hashtable.hpp>
#include <onyx/fnv.h>
#include <onyx/scoped_lock.h>
#include <onyx/file.h>

fnv_hash_t inode_hash(inode &ino)
{
	auto h = fnv_hash(&ino.i_dev, sizeof(dev_t));
	h = fnv_hash_cont(&ino.i_inode, sizeof(ino_t), h);
	return h;
}

fnv_hash_t inode_hash(dev_t dev, ino_t ino)
{
	auto h = fnv_hash(&dev, sizeof(dev_t));
	return fnv_hash_cont(&ino, sizeof(ino_t), h);
}

constexpr size_t inode_hashtable_size = 512;

static cul::hashtable2<inode, inode_hashtable_size, fnv_hash_t, inode_hash> inode_hashtable;
static struct spinlock inode_hashtable_locks[inode_hashtable_size];

struct page_cache_block *inode_get_cache_block(struct inode *ino, size_t off, long flags)
{
	assert(ino->i_pages != nullptr);

	if(flags & FILE_CACHING_WRITE && off >= ino->i_pages->size)
	{
		ino->i_pages->size += (off - ino->i_pages->size) + PAGE_SIZE;
		struct page *p = alloc_page(0);
		if(!p)
			return nullptr;

		auto block = pagecache_create_cache_block(p, PAGE_SIZE, off, ino);
		if(!block)
		{
			free_page(p);
			return nullptr;
		}

		if(vmo_add_page(off, p, ino->i_pages) < 0)
		{
			page_cache_destroy(block);
			return nullptr;
		}

		page_pin(p);

		return block;

	}

	struct page *p = vmo_get(ino->i_pages, off, VMO_GET_MAY_POPULATE);
	if(!p)
		return nullptr;

	return p->cache;
}

struct page_cache_block *__inode_get_page_internal(struct inode *inode, size_t offset, long flags)
{
	size_t aligned_off = offset & ~(PAGE_SIZE - 1);

	struct page_cache_block *b = inode_get_cache_block(inode, aligned_off, flags);
	
	return b;
}

struct page_cache_block *inode_get_page(struct inode *inode, size_t offset, long flags = 0)
{
	struct page_cache_block *b = __inode_get_page_internal(inode, offset, flags);

	/* No need to pin the page since it's already pinned by vmo_get */

	return b;
}


extern "C"
ssize_t file_write_cache(void *buffer, size_t len, struct inode *ino, size_t offset)
{
	scoped_rwlock<rw_lock::write> g{ino->i_rwlock};

	size_t wrote = 0;
	size_t pos = offset;


	while(wrote != len)
	{
		struct page_cache_block *cache = inode_get_page(ino, offset, FILE_CACHING_WRITE);

		if(cache == nullptr)
			return wrote ?: -1;

		struct page *page = cache->page;

		auto cache_off = offset & (PAGE_SIZE - 1);
		auto rest = PAGE_SIZE - cache_off;

		auto amount = len - wrote < rest ? len - wrote : rest;

		if(copy_from_user((char *) cache->buffer + cache_off, (char*) buffer +
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
		pos += amount;

		if(pos > ino->i_size)
			inode_set_size(ino, pos);
	}

	return (ssize_t) wrote;
}

extern "C"
ssize_t file_read_cache(void *buffer, size_t len, struct inode *file, size_t offset)
{
	if((size_t) offset >= file->i_size)
		return 0;

	size_t read = 0;

	while(read != len)
	{
		struct page_cache_block *cache = inode_get_page(file, offset);

		if(!cache)
			return read ?: -1;

		struct page *page = cache->page;

		auto cache_off = offset % PAGE_SIZE;
		auto rest = PAGE_SIZE - cache_off;

		assert(rest > 0);
	
		size_t amount = len - read < (size_t) rest ?
			len - read : (size_t) rest;

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

extern "C"
int inode_special_init(struct inode *ino)
{
	if(ino->i_type == VFS_TYPE_BLOCK_DEVICE || ino->i_type == VFS_TYPE_CHAR_DEVICE)
	{
		struct dev *d = dev_find(ino->i_rdev);
		if(!d)
			return -ENODEV;
		ino->i_fops = &d->fops;
		ino->i_helper = d->priv;
	}

	return 0;
}

extern "C"
void inode_ref(struct inode *ino)
{
	__atomic_add_fetch(&ino->i_refc, 1, __ATOMIC_RELAXED);
#if 0
	if(ino->i_inode == 3549)
		printk("inode_ref(%lu) from %p\n", ino->i_refc, __builtin_return_address(0));
#endif
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
		struct page_cache_block *b = (page_cache_block *) datum;
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

void inode_release(struct inode *inode)
{
	bool should_die = inode_get_nlink(inode) == 0;
	//printk("Releasing inode %p\n", inode);

	if(inode->i_sb)
	{
		assert(inode->i_sb != NULL);

		/* Remove the inode from its superblock */
		superblock_remove_inode(inode->i_sb, inode);
	}

	if(inode->i_flags & INODE_FLAG_DIRTY)
		flush_remove_inode(inode);

	if(inode->i_type == VFS_TYPE_FILE)
		inode_sync(inode);

	inode_destroy_page_caches(inode);

	/* Note that we require kill_inode to be called before close, at least for now,
	 * because close may very well free resources that are needed to free the inode.
	 * This happens, for example, in ext2.
	 */
	struct superblock *sb = inode->i_sb;

	if(should_die && sb && sb->kill_inode)
	{
		/* TODO: Handle failures? */
		sb->kill_inode(inode);
	}

	if(inode->i_fops->close != NULL)
		inode->i_fops->close(inode);

	free(inode);
}

void inode_unref(struct inode *ino)
{
	unsigned long refs = __atomic_sub_fetch(&ino->i_refc, 1, __ATOMIC_RELAXED);
	//printk("unref %p(ino nr %lu) - refs %lu\n", ino, ino->i_inode, refs);
#if 0
	if(inode_should_die(ino))
	{
		printk("inode should die and refs = %lu\n", refs);
		while(true) {}
	}
#endif

	if(!refs && inode_should_die(ino))
	{
		inode_release(ino);
	}
#if 0
	if(ino->i_inode == 3549)
		printk("inode_unref(%lu) from %p\n", ino->i_refc, __builtin_return_address(0));
#endif
}

extern "C"
struct inode *superblock_find_inode(struct superblock *sb, ino_t ino_nr)
{
	auto hash = inode_hash(sb->s_devnr, ino_nr);

	auto index = inode_hashtable.get_hashtable_index(hash);

	scoped_lock<spinlock> g{&inode_hashtable_locks[index]};

	auto _l = inode_hashtable.get_hashtable(index);

	list_for_every(_l)
	{
		auto ino = container_of(l, inode, i_hash_list_node);
		
		if(ino->i_dev == sb->s_devnr && ino->i_inode == ino_nr)
		{
			inode_ref(ino);
			return ino;
		}
	}

	g.keep_locked();

	return nullptr;
}

extern "C"
void superblock_add_inode_unlocked(struct superblock *sb, struct inode *inode)
{
	auto hash = inode_hash(sb->s_devnr, inode->i_inode);
	auto index = inode_hashtable.get_hashtable_index(hash);

	MUST_HOLD_LOCK(&inode_hashtable_locks[index]);

	auto head = inode_hashtable.get_hashtable(index);

	list_add_tail(&inode->i_hash_list_node, head);

	scoped_lock g{&sb->s_ilock};
	list_add_tail(&inode->i_sb_list_node, &sb->s_inodes);
	__atomic_add_fetch(&sb->s_ref, 1, __ATOMIC_RELAXED);

	spin_unlock(&inode_hashtable_locks[index]);
}

/* Should only be used when creating new inodes(so we're sure that they don't exist). */
extern "C"
void superblock_add_inode(struct superblock *sb, struct inode *inode)
{
	auto hash = inode_hash(sb->s_devnr, inode->i_inode);
	auto index = inode_hashtable.get_hashtable_index(hash);
	scoped_lock g{&inode_hashtable_locks[index]};
	superblock_add_inode_unlocked(sb, inode);
	
	// Was already unlocked
	g.keep_locked();
}

extern "C"
void superblock_remove_inode(struct superblock *sb, struct inode *inode)
{
	scoped_lock g{&sb->s_ilock};
	list_remove(&inode->i_sb_list_node);

	__atomic_sub_fetch(&sb->s_ref, 1, __ATOMIC_RELAXED);
}

extern "C"
void superblock_kill(struct superblock *sb)
{
	list_for_every_safe(&sb->s_inodes)
	{
		struct inode *ino = container_of(l, inode, i_sb_list_node);

		close_vfs(ino);
	}
}

extern "C"
void inode_unlock_hashtable(struct superblock *sb, ino_t ino_nr)
{
	auto hash = inode_hash(sb->s_devnr, ino_nr);

	auto index = inode_hashtable.get_hashtable_index(hash);

	spin_unlock(&inode_hashtable_locks[index]);
}

extern "C"
int sys_fsync(int fd)
{
	auto_file f;
	if(f.from_fd(fd) < 0)
	{
		return -EBADF;
	}

	/* TODO: Same problem as inode_sync, return errors. */
	inode_sync(f.get_file()->f_ino);

	return 0;
}
