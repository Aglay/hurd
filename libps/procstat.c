/* The type proc_stat_t, which holds information about a hurd process.

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

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

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "ps.h"
#include "common.h"

/* ---------------------------------------------------------------- */

/* These directly correspond to the bits in a state, starting from 0.  See
   ps.h for an explanation of what each of these means.  */
char *proc_stat_state_tags = "RTHDSIWN<Z+sfmpox";

/* ---------------------------------------------------------------- */

/* Return the PSTAT_STATE_ bits describing the state of an individual thread,
   from that thread's thread_basic_info_t struct */
static int 
thread_state(thread_basic_info_t bi)
{
  int state = 0;

  switch (bi->run_state)
    {
    case TH_STATE_RUNNING:
      state |= PSTAT_STATE_RUNNING;
      break;
    case TH_STATE_STOPPED:
      state |= PSTAT_STATE_STOPPED;
      break;
    case TH_STATE_UNINTERRUPTIBLE:
      state |= PSTAT_STATE_WAIT;
      break;
    case TH_STATE_HALTED:
      state |= PSTAT_STATE_HALTED;
      break;
    case TH_STATE_WAITING:
      /* Act like unix: waits of less than 20 seconds means a process is
	 `sleeping' and >= 20 seconds means it's `idle' */
      state |= bi->sleep_time < 20 ? PSTAT_STATE_SLEEPING : PSTAT_STATE_IDLE;
      break;
    }

  if (bi->base_priority < 12)
    state |= PSTAT_STATE_PRIORITY;
  else if (bi->base_priority > 12)
    state |= PSTAT_STATE_NICED;

  if (bi->flags & TH_FLAGS_SWAPPED)
    state |= PSTAT_STATE_SWAPPED;

  return state;
}

/* ---------------------------------------------------------------- */

/* Returns FLAGS augmented with any other flags that are necessary
   preconditions to setting them.  */
static ps_flags_t 
add_preconditions (ps_flags_t flags)
{
  /* Implement any inter-flag dependencies: if the new flags in FLAGS depend
     on some other set of flags to be set, make sure those are also in
     FLAGS. */
  if (flags & PSTAT_TTY)
    flags |= PSTAT_CTTYID;
  if (flags & PSTAT_STATE)
    flags |= PSTAT_EXEC_FLAGS;	/* For the traced bit.  */
  if (flags & PSTAT_SUSPEND_COUNT)
    /* We just request the resources require for both the thread and task
       versions, as the extraneous info won't be possible to aquire anyway. */
    flags |= PSTAT_INFO | PSTAT_THREAD_INFO;
  if (flags & (PSTAT_CTTYID | PSTAT_CWDIR | PSTAT_AUTH | PSTAT_UMASK
	       | PSTAT_EXEC_FLAGS)
      && !(flags & PSTAT_NO_MSGPORT))
    {
      flags |= PSTAT_MSGPORT;
      flags |= PSTAT_TASK;	/* for authentication */
    }
  if (flags & (PSTAT_THREAD_INFO | PSTAT_STATE | PSTAT_OWNER))
    flags |= PSTAT_INFO;
  if (flags & PSTAT_TASK_EVENTS_INFO)
    flags |= PSTAT_TASK;

  return flags;
}

/* Those flags that should be set before calling should_suppress_msgport.  */
#define SHOULD_SUPPRESS_MSGPORT_FLAGS (PSTAT_INFO | PSTAT_THREAD_INFO)

/* Return true when there's some condition indicating that we shouldn't use
   PS's msg port.  For this routine to work correctly, PS's flags should
   contain as many flags in SHOULD_SUPPRESS_MSGPORT_FLAGS as possible.  */
static int
should_suppress_msgport (proc_stat_t ps)
{
  ps_flags_t have = ps->flags;

  if ((have & PSTAT_INFO) && ps->info->taskinfo.suspend_count != 0)
    /* Task is suspended.  */
    return TRUE;

  if ((have & PSTAT_THREAD_INFO) && ps->thread_basic_info.suspend_count != 0)
    /* All threads are suspended.  */
    return TRUE;

  if ((have & PSTAT_INFO) && ps->info->nthreads == 0)
    /* No threads (some bug could cause the procserver still to think there's
       a msgport).  */
    return TRUE;

  return FALSE;
}

/* Returns FLAGS with PSTAT_MSGPORT turned off and PSTAT_NO_MSGPORT on.  */
#define SUPPRESS_MSGPORT_FLAGS(flags) \
   (((flags) & ~PSTAT_MSGPORT) | PSTAT_NO_MSGPORT)

/* ---------------------------------------------------------------- */

/* Add FLAGS to PS's flags, fetching information as necessary to validate
   the corresponding fields in PS.  Afterwards you must still check the flags
   field before using new fields, as something might have failed.  Returns
   a system error code if a fatal error occurred, or 0 if none.  */
error_t
proc_stat_set_flags (proc_stat_t ps, ps_flags_t flags)
{
  ps_flags_t have = ps->flags;	/* flags set in ps */
  ps_flags_t need;		/* flags not set in ps, but desired to be */
  ps_flags_t no_msgport_flags;	/* a version of FLAGS for use when we can't
				   use the msg port.  */
  process_t server = ps_context_server(ps->context);

  /* Turn off use of the msg port if we decide somewhere along the way that
     it's hosed.  */
  void suppress_msgport ()
    {
      /* Turn off those things that were only good given the msg port.  */
      need &= ~(flags & ~no_msgport_flags);
      have = SUPPRESS_MSGPORT_FLAGS (have);
    }

  /* Propagate PSTAT_NO_MSGPORT.  */
  if (flags & PSTAT_NO_MSGPORT)
    have = SUPPRESS_MSGPORT_FLAGS (have);
  if (have & PSTAT_NO_MSGPORT)
    flags = SUPPRESS_MSGPORT_FLAGS (flags);

  no_msgport_flags = add_preconditions (SUPPRESS_MSGPORT_FLAGS (flags));
  flags = add_preconditions (flags);

  if (flags & PSTAT_MSGPORT)
    /* Add in some values that we can use to determine whether the msgport
       shouldn't be used.  */
    flags |= add_preconditions (SHOULD_SUPPRESS_MSGPORT_FLAGS);

  need = flags & ~have;

  /* MGET: If we're trying to set FLAG, and the preconditions PRECOND are set
     in the flags already, then eval CALL and set ERR to the result.
     If the resulting ERR is 0 add FLAG to the set of valid flags.  ERR is
     returned.  */
#define MGET(_flag, _precond, call) \
  ({ ps_flags_t flag = (_flag), precond = (_precond); \
     error_t err; \
     if (!(need & (flag)) || (have & (precond)) != (precond)) \
       err = 0; \
     else \
       { \
	 err = (call); \
	 if (!err) \
	   have |= flag; \
       } \
     err; \
   })

  /* A version of MGET specifically for the msg port, that turns off the msg
     port if a call to it times out.  It also implies a precondition of
     PSTAT_MSGPORT.  */
#define MP_MGET(flag, precond, call) \
  ({ error_t err = MGET(flag, (precond) | PSTAT_MSGPORT, call); \
     if (err) suppress_msgport (); \
     err; \
   })

  /* the process's struct procinfo as returned by proc_getprocinfo.  */
  if ((need & PSTAT_INFO) && (have & PSTAT_PID))
    {
      ps->info_size = 0;
      if (proc_getprocinfo(server, ps->pid, (int **)&ps->info, &ps->info_size)
	  == 0)
	have |= PSTAT_INFO;
    }

  /* A summary of the proc's thread_{basic,sched}_info_t structures: sizes
     and cumulative times are summed, prioritys and delta time are
     averaged.  The run_states are added by having running thread take
     precedence over waiting ones, and if there are any other incompatible
     states, simply using a bogus value of -1 */
  if ((need & PSTAT_THREAD_INFO) && (have & PSTAT_INFO))
    {
      int i;
      struct procinfo *pi = ps->info;
      thread_sched_info_t tsi = &ps->thread_sched_info;
      thread_basic_info_t tbi = &ps->thread_basic_info;

      bzero(tbi, sizeof *tbi);
      bzero(tsi, sizeof *tsi);

      for (i = 0; i < pi->nthreads; i++)
	{
	  thread_basic_info_t bi = &pi->threadinfos[i].pis_bi;
	  thread_sched_info_t si = &pi->threadinfos[i].pis_si;
	  int thread_run_state = bi->run_state;

	  /* Construct some aggregate run-state for the process:  besides the
	     defined thread run_states, we use 0 to mean no threads, and -1
	     to mean two threads have conflicting run_stats.  */

	  if (tbi->run_state == 0)
	    /* No prior state, just copy this thread's.  */
	    tbi->run_state = thread_run_state;
	  else if (tbi->run_state == TH_STATE_RUNNING
		   || thread_run_state == TH_STATE_RUNNING)
	    /* If any thread is running, mark the process as running.  */
	    tbi->run_state = TH_STATE_RUNNING;
	  else if (tbi->run_state != bi->run_state)
	    /* Otherwise there are two conflicting non-running states, so
	       just give up and say we don't know.  */
	    tbi->run_state = -1;

	  tsi->base_priority += si->base_priority;
	  tsi->cur_priority += si->cur_priority;

	  tbi->cpu_usage += bi->cpu_usage;
	  tbi->sleep_time += bi->sleep_time;

	  /* The aggregate suspend count is the minimum of all threads.  */
	  if (i == 0 || tbi->suspend_count > bi->suspend_count)
	    tbi->suspend_count = bi->suspend_count;

	  tbi->user_time.seconds += bi->user_time.seconds;
	  tbi->user_time.microseconds += bi->user_time.microseconds;
	  tbi->system_time.seconds += bi->system_time.seconds;
	  tbi->system_time.microseconds += bi->system_time.microseconds;
	}

      if (pi->nthreads > 0)
	{
	  tsi->base_priority /= pi->nthreads;
	  tsi->cur_priority /= pi->nthreads;
	  tbi->sleep_time /= pi->nthreads;
	}

      tbi->user_time.seconds += tbi->user_time.microseconds / 1000000;
      tbi->user_time.microseconds %= 1000000;
      tbi->system_time.seconds += tbi->system_time.microseconds / 1000000;
      tbi->system_time.microseconds %= 1000000;

      have |= PSTAT_THREAD_INFO;
    }

  if ((need & PSTAT_SUSPEND_COUNT)
      &&
      ((have & PSTAT_PID) ? (have & PSTAT_INFO) : (have & PSTAT_THREAD_INFO)))
    {
      if (have & PSTAT_PID)
	ps->suspend_count = ps->info->taskinfo.suspend_count;
      else
	ps->suspend_count = ps->thread_basic_info.suspend_count;
      have |= PSTAT_SUSPEND_COUNT;
    }

  ps->flags = have;		/* should_suppress_msgport looks at them.  */
  if (should_suppress_msgport (ps))
    suppress_msgport ();

  /* The process's libc msg port (see <hurd/msg.defs>).  */
  MGET(PSTAT_MSGPORT, PSTAT_PID, proc_getmsgport(server, ps->pid, &ps->msgport));
  /* The process's process port.  */
  MGET(PSTAT_PROCESS, PSTAT_PID, proc_pid2proc(server, ps->pid, &ps->process));
  /* The process's task port.  */
  MGET(PSTAT_TASK, PSTAT_PID, proc_pid2task(server, ps->pid, &ps->task));

  /* VM statistics for the task.  See <mach/task_info.h>.  */
  if ((need & PSTAT_TASK_EVENTS_INFO) && (have & PSTAT_TASK))
    {
      ps->task_events_info = &ps->task_events_info_buf;
      ps->task_events_info_size = TASK_EVENTS_INFO_COUNT;
      if (task_info(ps->task, TASK_EVENTS_INFO,
		    (task_info_t)&ps->task_events_info,
		    &ps->task_events_info_size)
	  == 0)
	have |= PSTAT_TASK_EVENTS_INFO;
    }

  /* Get the process's exec flags (see <hurd/hurd_types.h>).  */
  MP_MGET(PSTAT_EXEC_FLAGS, PSTAT_TASK,
	  msg_get_exec_flags(ps->msgport, ps->task, &ps->exec_flags));

  /* PSTAT_STATE_ bits for the process and all its threads.  */
  if ((need & PSTAT_STATE) && (have & PSTAT_INFO))
    {
      int i;
      int state = 0;
      struct procinfo *pi = ps->info;
      int pi_flags = pi->state;

      /* The union of all thread state bits...  */
      for (i = 0; i < pi->nthreads; i++)
	state |= thread_state(&pi->threadinfos[i].pis_bi);

      /* ... and process-only state bits.  */
      if (pi_flags & PI_ZOMBIE)
	state |= PSTAT_STATE_ZOMBIE;
      if (pi_flags & PI_SESSLD)
	state |= PSTAT_STATE_SESSLDR;
      if (!(pi_flags & PI_EXECED))
	state |= PSTAT_STATE_FORKED;
      if (pi_flags & PI_NOMSG)
	state |= PSTAT_STATE_NOMSG;
      if (pi_flags & PI_NOPARENT)
	state |= PSTAT_STATE_NOPARENT;
      if (pi_flags & PI_ORPHAN)
	state |= PSTAT_STATE_ORPHANED;

      if ((have & PSTAT_EXEC_FLAGS) & (ps->exec_flags & EXEC_TRACED))
	state |= PSTAT_STATE_TRACED;

      ps->state = state;
      have |= PSTAT_STATE;
    }

  /* The process's exec arguments */
  if ((need & PSTAT_ARGS) && (have & PSTAT_PID))
    {
      ps->args_len = 0;
      if (proc_getprocargs(server, ps->pid, &ps->args, &ps->args_len))
	ps->args_len = 0;
      else
	have |= PSTAT_ARGS;
    }

  /* The ctty id port; note that this is just a magic cookie;
     we use it to fetch a port to the actual terminal -- it's not useful for
     much else.  */
  MP_MGET(PSTAT_CTTYID, PSTAT_TASK,
	  msg_get_init_port(ps->msgport, ps->task,
			    INIT_PORT_CTTYID, &ps->cttyid));

  /* A port to the process's current working directory.  */
  MP_MGET(PSTAT_CWDIR, PSTAT_TASK,
	  msg_get_init_port(ps->msgport, ps->task,
			    INIT_PORT_CWDIR, &ps->cwdir));

  /* The process's auth port, which we can use to determine who the process
     is authenticated as.  */
  MP_MGET(PSTAT_AUTH, PSTAT_TASK,
	  msg_get_init_port(ps->msgport, ps->task, INIT_PORT_AUTH, &ps->auth));

  /* The process's umask, which controls which protection bits won't be set
     when creating a file.  */
  MP_MGET(PSTAT_UMASK, PSTAT_TASK,
	  msg_get_init_int(ps->msgport, ps->task, INIT_UMASK, &ps->umask));

  /* A ps_user_t object for the process's owner.  */
  if ((need & PSTAT_OWNER) && (have & PSTAT_INFO))
    if (ps_context_find_user(ps->context, ps->info->owner, &ps->owner) == 0)
      have |= PSTAT_OWNER;

  /* A ps_tty_t for the process's controlling terminal, or NULL if it
     doesn't have one.  */
  if ((need & PSTAT_TTY) && (have & PSTAT_CTTYID))
    if (ps_context_find_tty_by_cttyid(ps->context, ps->cttyid, &ps->tty) == 0)
      have |= PSTAT_TTY;

  ps->flags = have;

  return 0;
}

/* ---------------------------------------------------------------- */
/* Discard PS and any resources it holds.  */
void 
_proc_stat_free(ps)
     proc_stat_t ps;
{
  /* Free the mach port PORT if FLAG is set in PS's flags.  */
#define MFREEPORT(flag, port) \
    ((ps->flags & (flag)) \
     ? mach_port_deallocate(mach_task_self(), (ps->port)) : 0)

  /* If FLAG is set in PS's flags, vm_deallocate MEM, consisting of SIZE 
     elements of type ELTYPE, *unless* MEM == SBUF, which usually means
     that MEM points to a static buffer somewhere instead of vm_alloc'd
     memory.  */
#define MFREEVM(flag, mem, size, sbuf, eltype) \
    (((ps->flags & (flag)) && ps->mem != sbuf) \
     ? (VMFREE(ps->mem, ps->size * sizeof(eltype))) : 0)

  /* free PS's ports */
  MFREEPORT(PSTAT_PROCESS, process);
  MFREEPORT(PSTAT_TASK, task);
  MFREEPORT(PSTAT_MSGPORT, msgport);
  MFREEPORT(PSTAT_CTTYID, cttyid);
  MFREEPORT(PSTAT_CWDIR, cwdir);
  MFREEPORT(PSTAT_AUTH, auth);

  /* free any allocated memory pointed to by PS */
  MFREEVM(PSTAT_INFO, info, info_size, 0, char);
  MFREEVM(PSTAT_ARGS, args, args_len, 0, char);
  MFREEVM(PSTAT_TASK_EVENTS_INFO,
	  task_events_info, task_events_info_size,
	  &ps->task_events_info_buf, char);

  FREE(ps);
}

/* Returns in PS a new proc_stat_t for the process PID at the process context
   CONTEXT.  If a memory allocation error occurs, ENOMEM is returned,
   otherwise 0.  */
error_t
_proc_stat_create(pid_t pid, ps_context_t context, proc_stat_t *ps)
{
  *ps = NEW(struct proc_stat);
  if (*ps == NULL)
    return ENOMEM;

  (*ps)->pid = pid;
  (*ps)->flags = PSTAT_PID;
  (*ps)->context = context;

  return 0;
}

/* ---------------------------------------------------------------- */

/* Returns in THREAD_PS a proc_stat_t for the Nth thread in the proc_stat_t
   PS (N should be between 0 and the number of threads in the process).  The
   resulting proc_stat_t isn't fully functional -- most flags can't be set in
   it.  It also contains a pointer to PS, so PS shouldn't be freed without
   also freeing THREAD_PS.  If N was out of range, EINVAL is returned.  If a
   memory allocation error occured, ENOMEM is returned.  Otherwise, 0 is
   returned.  */
error_t
proc_stat_thread_create(proc_stat_t ps, unsigned index, proc_stat_t *thread_ps)
{
  error_t err = proc_stat_set_flags(ps, PSTAT_THREAD_INFO);
  if (err)
    return err;
  else if (index >= ps->info->nthreads)
    return EINVAL;
  else
    {
      proc_stat_t tps = NEW(struct proc_stat);

      if (tps == NULL)
	return ENOMEM;

      /* A value of -1 for the PID indicates that this is a thread.  */
      tps->pid = -1;

      /* TPS is initialized with these values; any attempts to set other
	 flags will fail because PSTAT_PID isn't one of them.  */
      tps->flags = PSTAT_THREAD | PSTAT_THREAD_INFO | PSTAT_STATE;

      bcopy(&ps->info->threadinfos[index].pis_bi,
	    &tps->thread_basic_info,
	    sizeof(thread_basic_info_data_t));
      bcopy(&ps->info->threadinfos[index].pis_si,
	    &tps->thread_sched_info,
	    sizeof(thread_sched_info_data_t));

      /* Construct the state with only the per-thread bits.  */
      tps->state = thread_state(&tps->thread_basic_info);

      tps->thread_origin = ps;
      tps->thread_index = index;

      tps->context = ps->context;

      *thread_ps = tps;

      return 0;
    }
}
