/* File truncation

   Copyright (C) 1995 Free Software Foundation, Inc.

   Converted to work under the hurd by Miles Bader <miles@gnu.ai.mit.edu>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/*
 *  linux/fs/ext2/truncate.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/truncate.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include "ext2fs.h"

#ifdef DONT_CACHE_MEMORY_OBJECTS
#define MAY_CACHE 0
#else
#define MAY_CACHE 1
#endif

/* ---------------------------------------------------------------- */

/* Write something to each page from START to END inclusive of memory
   object OBJ, but make sure the data doesns't actually change. */
static void
poke_pages (memory_object_t obj,
	    vm_offset_t start,
	    vm_offset_t end)
{
  vm_address_t addr, poke;
  vm_size_t len;
  error_t err;
  
  while (start < end)
    {
      len = 8 * vm_page_size;
      if (len > end - start)
	len = end - start;
      addr = 0;
      err = vm_map (mach_task_self (), &addr, len, 0, 1, obj, start, 0,
		    VM_PROT_WRITE|VM_PROT_READ, VM_PROT_READ|VM_PROT_WRITE, 0);
      if (!err)
	{
	  for (poke = addr; poke < addr + len; poke += vm_page_size)
	    *(volatile int *)poke = *(volatile int *)poke;
	  vm_deallocate (mach_task_self (), addr, len);
	}
      start += len;
    }
}

/* ---------------------------------------------------------------- */

#define DIRECT_BLOCK(length) \
  ((length + block_size - 1) >> log2_block_size)
#define INDIRECT_BLOCK(length, offset) ((int)DIRECT_BLOCK(length) - offset)
#define DINDIRECT_BLOCK(length, offset) \
  (((int)DIRECT_BLOCK(length) - offset) / addr_per_block)
#define TINDIRECT_BLOCK(length) \
  (((int)DIRECT_BLOCK(length) \
    - (addr_per_block * addr_per_block + addr_per_block + EXT2_NDIR_BLOCKS)) \
   / (addr_per_block * addr_per_block))

static void
trunc_direct (struct node * node, unsigned long length)
{
  u32 block;
  int i;
  unsigned long block_to_free = 0;
  unsigned long free_count = 0;
  int direct_block = DIRECT_BLOCK(length);

  ext2_debug ("truncating direct blocks from %lu, block %d",
	      length, direct_block);

  for (i = direct_block ; i < EXT2_NDIR_BLOCKS ; i++)
    {
      block = node->dn->info.i_data[i];
      if (!block)
	continue;

      node->dn->info.i_data[i] = 0;

      node->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
      node->dn_stat_dirty = 1;

      if (free_count == 0)
	{
	  block_to_free = block;
	  free_count++;
	}
      else if (free_count > 0 && block_to_free == block - free_count)
	free_count++;
      else
	{
	  ext2_free_blocks (block_to_free, free_count);
	  block_to_free = block;
	  free_count = 1;
	}
    }

  if (free_count > 0)
    ext2_free_blocks (block_to_free, free_count);
}

/* ---------------------------------------------------------------- */

static void
trunc_indirect (struct node * node, unsigned long length, int offset, u32 * p)
{
  int i, block;
  char * ind_bh;
  u32 * ind;
  int modified = 0;
  unsigned long block_to_free = 0;
  unsigned long free_count = 0;
  int indirect_block = INDIRECT_BLOCK (length, offset);

  if (indirect_block < 0)
    indirect_block = 0;

  ext2_debug ("truncating indirect (offs = %d) blocks from %lu, block %d",
	      offset, length, indirect_block);

  block = *p;
  if (!block)
    return;

  ind_bh = bptr (block);

  for (i = indirect_block ; i < addr_per_block ; i++)
    {
      ind = (u32 *)ind_bh + i;
      block = *ind;

      if (block)
	{
	  *ind = 0;

	  if (free_count == 0)
	    {
	      block_to_free = block;
	      free_count++;
	    }
	  else if (free_count > 0 && block_to_free == block - free_count)
	    free_count++;
	  else
	    {
	      ext2_free_blocks (block_to_free, free_count);
	      block_to_free = block;
	      free_count = 1;
	    }

	  node->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
	  node->dn_stat_dirty = 1;

	  modified = 1;
	}
    }

  if (free_count > 0)
    ext2_free_blocks (block_to_free, free_count);

  ind = (u32 *) ind_bh;
  for (i = 0; i < addr_per_block; i++)
    if (*(ind++))
      break;

  if (i >= addr_per_block)
    {
      block = *p;
      *p = 0;
      node->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
      node->dn_stat_dirty = 1;
      ext2_free_blocks (block, 1);
    }
  else if (modified)
    record_indir_poke (node, ind_bh);
}

/* ---------------------------------------------------------------- */

static void
trunc_dindirect (struct node * node, unsigned long length,
		 int offset, u32 * p)
{
  int i, block;
  char * dind_bh;
  u32 * dind;
  int modified = 0;
  int dindirect_block = DINDIRECT_BLOCK (length, offset);

  if (dindirect_block < 0)
    dindirect_block = 0;

  ext2_debug ("truncating dindirect (offs = %d) blocks from %lu, block %d",
	      offset, length, dindirect_block);

  block = *p;
  if (!block)
    return;

  dind_bh = bptr (block);

  for (i = dindirect_block ; i < addr_per_block ; i++)
    {
      dind = i + (u32 *) dind_bh;
      block = *dind;

      if (!block)
	{
	  trunc_indirect (node, length, offset + (i * addr_per_block), dind);
	  modified = 1;
	}
    }

  dind = (u32 *) dind_bh;
  for (i = 0; i < addr_per_block; i++)
    if (*(dind++))
      break;

  if (i >= addr_per_block)
    {
      block = *p;
      *p = 0;
      node->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
      node->dn_stat_dirty = 1;
      ext2_free_blocks (block, 1);
    }
  else if (modified)
    record_indir_poke (node, dind_bh);
}

/* ---------------------------------------------------------------- */

static void
trunc_tindirect (struct node * node, unsigned long length)
{
  int i, block;
  char * tind_bh;
  u32 * tind, * p;
  int modified = 0;
  int tindirect_block = TINDIRECT_BLOCK (length);

  if (tindirect_block < 0)
    tindirect_block = 0;

  ext2_debug ("truncating tindirect blocks from %lu, block %d",
	      length, tindirect_block);

  p = node->dn->info.i_data + EXT2_TIND_BLOCK;
  if (!(block = *p))
    return;

  tind_bh = bptr (block);
  if (!tind_bh)
    {
      *p = 0;
      return;
    }

  for (i = tindirect_block ; i < addr_per_block ; i++)
    {
      tind = i + (u32 *) tind_bh;
      trunc_dindirect(node, length,
		      (EXT2_NDIR_BLOCKS
		       + addr_per_block
		       + (i + 1) * addr_per_block * addr_per_block),
		      tind);
      modified = 1;
    }

  tind = (u32 *) tind_bh;
  for (i = 0; i < addr_per_block; i++)
    if (*(tind++))
      break;

  if (i >= addr_per_block)
    {
      block = *p;
      *p = 0;
      node->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
      node->dn_stat_dirty = 1;
      ext2_free_blocks (block, 1);
    }
  else if (modified)
    record_indir_poke (node, tind_bh);
}

/* ---------------------------------------------------------------- */

/* Flush all the data past the new size from the kernel.  Also force any
   delayed copies of this data to take place immediately.  (We are implicitly
   changing the data to zeros and doing it without the kernel's immediate
   knowledge; accordingl we must help out the kernel thusly.) */
static void
force_delayed_copies (struct node *node, off_t length)
{
  struct user_pager_info *upi;

  spin_lock (&node_to_page_lock);
  upi = node->dn->fileinfo;
  if (upi)
    ports_port_ref (upi->p);
  spin_unlock (&node_to_page_lock);
 
  if (upi)
    {
      mach_port_t obj;
      
      pager_change_attributes (upi->p, MAY_CACHE, MEMORY_OBJECT_COPY_NONE, 1);
      obj = diskfs_get_filemap (node);
      poke_pages (obj, round_page (length), round_page (node->allocsize));
      mach_port_deallocate (mach_task_self (), obj);
      pager_flush_some (upi->p, round_page(length), node->allocsize - length, 1);
      ports_port_deref (upi->p);
    }
}

static void
enable_delayed_copies (struct node *node)
{
  struct user_pager_info *upi;

  spin_lock (&node_to_page_lock);
  upi = node->dn->fileinfo;
  if (upi)
    ports_port_ref (upi->p);

  spin_unlock (&node_to_page_lock);
  if (upi)
    {
      pager_change_attributes (upi->p, MAY_CACHE, MEMORY_OBJECT_COPY_DELAY, 0);
      ports_port_deref (upi->p);
    }
}

/* ---------------------------------------------------------------- */

/* The user must define this function.  Truncate locked node NODE to be SIZE
   bytes long.  (If NODE is already less than or equal to SIZE bytes
   long, do nothing.)  If this is a symlink (and diskfs_shortcut_symlink
   is set) then this should clear the symlink, even if 
   diskfs_create_symlink_hook stores the link target elsewhere.  */
error_t
diskfs_truncate (struct node *node, off_t length)
{
  error_t err;
  int offset;

  assert (!diskfs_readonly);

  if (length >= node->dn_stat.st_size)
    return 0;

  /*
   * If the file is not being truncated to a block boundary, the
   * contents of the partial block following the end of the file must be
   * zeroed in case it ever becomes accessible again because of
   * subsequent file growth.
   */
  offset = length % block_size;
  if (offset > 0)
    {
      diskfs_node_rdwr (node, (void *)zeroblock, length, block_size - offset,
			1, 0, 0);
      diskfs_file_update (node, 1);
    }

  ext2_discard_prealloc(node);

  force_delayed_copies (node, length);

  rwlock_writer_lock (&node->dn->alloc_lock);

  /* Update the size on disk; fsck will finish freeing blocks if necessary
     should we crash. */
  node->dn_stat.st_size = length;
  node->dn_set_mtime = 1;
  node->dn_set_ctime = 1;
  diskfs_node_update (node, 1);

  err = diskfs_catch_exception();
  if (!err)
    {
      trunc_direct(node, length);
      trunc_indirect (node, length, EXT2_IND_BLOCK,
		      (u32 *) &node->dn->info.i_data[EXT2_IND_BLOCK]);
      trunc_dindirect (node, length, EXT2_IND_BLOCK +
		       EXT2_ADDR_PER_BLOCK(sblock),
		       (u32 *) &node->dn->info.i_data[EXT2_DIND_BLOCK]);
      trunc_tindirect (node, length);

      node->allocsize = round_block (length);

      /* Set our end-of-file variables to a pessimistic state -- it won't
	 hurt if they are wrong.  */
      node->dn->last_block_allocated = 0;
      node->dn->last_page_partially_writable =
	trunc_page (node->allocsize) != node->allocsize;
    }

  node->dn_set_mtime = 1;
  node->dn_set_ctime = 1;
  node->dn_stat_dirty = 1;

  /* Now we can permit delayed copies again. */
  enable_delayed_copies (node);

  rwlock_writer_unlock (&node->dn->alloc_lock);

  return err;
}
