/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
   Written by Michael I. Bushnell.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "ports.h"
#include <assert.h>
#include <cthreads.h>
#include <hurd/ihash.h>

void *ports_allocate_port (struct port_bucket *bucket, 
			   size_t size, 
			   struct port_class *class)
{
  mach_port_t port;
  error_t err;
  struct port_info *pi;
  
  err = mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE,
			    &port);
  assert_perror (err);
  if (size < sizeof (struct port_info))
    size = sizeof (struct port_info);
  
  pi = malloc (size);
  assert (pi);
  pi->class = class;
  pi->refcnt = 1;
  pi->weakrefcnt = 0;
  pi->cancel_threshhold = 0;
  pi->mscount = 0;
  pi->flags = 0;
  pi->port_right = port;
  pi->current_rpcs = 0;
  pi->bucket = bucket;
  
  mutex_lock (&_ports_lock);
  
 loop:
  if (class->flags & PORT_CLASS_NO_ALLOC)
   { 
     class->flags |= PORT_CLASS_ALLOC_WAIT;
     condition_wait (&_ports_block, &_ports_lock);
     goto loop;
   }
  if (bucket->flags & PORT_BUCKET_NO_ALLOC)
    {
      bucket->flags |= PORT_BUCKET_ALLOC_WAIT;
      condition_wait (&_ports_block, &_ports_lock);
      goto loop;
    }

  err = ihash_add (bucket->htable, port, pi, &pi->hentry);
  assert_perror (err);
  pi->next = class->ports;
  pi->prevp = &class->ports;
  if (class->ports)
    class->ports->prevp = &pi->next;
  class->ports = pi;
  bucket->count++;
  class->count++;
  mutex_unlock (&_ports_lock);
  
  mach_port_move_member (mach_task_self (), pi->port_right, bucket->portset);
  return pi;
}

