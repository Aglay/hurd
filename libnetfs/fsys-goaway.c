/* implement the fsys_goaway RPC for libnetfs
   Copyright (C) 2001 Free Software Foundation, Inc.

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
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "priv.h"
#include "netfs.h"
#include "fsys_S.h"
#include "fsys_reply_U.h"

#include <stdlib.h>
#include <errno.h>
#include <hurd/ports.h>

error_t
netfs_S_fsys_goaway (fsys_t control,
		     mach_port_t reply,
		     mach_msg_type_name_t reply_type,
		     int flags)
{
  error_t err;
  struct port_info *pt;

  pt = ports_lookup_port (netfs_port_bucket, control,
			  netfs_control_class);
  if (! pt)
    return EOPNOTSUPP;

  err = netfs_shutdown (flags);
  if (! err)
    {
      fsys_goaway_reply (reply, reply_type, 0);
      exit (0);
    }

  ports_port_deref (pt);

  return err;
}