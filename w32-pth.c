/* w32-pth.c - GNU Pth emulation for W32 (MS Windows).
 * Copyright (c) 1999-2003 Ralf S. Engelschall <rse@engelschall.com>
 * Copyright (C) 2004, 2006, 2007 g10 Code GmbH
 *
 * This file is part of W32PTH.
 *
 * W32PTH is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * W32PTH is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * ------------------------------------------------------------------
 * This code is based on Ralf Engelschall's GNU Pth, a non-preemptive
 * thread scheduling library which can be found at
 * http://www.gnu.org/software/pth/.  MS Windows (W32) specific code
 * written by Timo Schulz, g10 Code.
 */

#include <config.h>

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <signal.h>
#include <errno.h>

/* We don't want to have any Windows specific code in the header, thus
   we use a macro which defaults to a compatible type in w32-pth.h. */
#define W32_PTH_HANDLE_INTERNAL  HANDLE
#include "pth.h"


#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#if FALSE != 0 || TRUE != 1 
#error TRUE or FALSE defined to wrong values
#endif

#if SIZEOF_LONG_LONG != 8
#error long long is not 64 bit
#endif


/* States whether this module has been initialized.  */
static int pth_initialized;

/* Keeps the current debug level. Define marcos to test them. */
static int debug_level;
#define DBG_ERROR  (debug_level >= 1)
#define DBG_INFO   (debug_level >= 2)
#define DBG_CALLS  (debug_level >= 3)

/* Variables to support event handling. */
static int pth_signo;
static HANDLE pth_signo_ev;

/* Mutex to make sure only one thread is running. */
static CRITICAL_SECTION pth_shd;

/* Object used by update_fdarray.  */
struct fdarray_item_s 
{
  int fd;
  long netevents;
};


/* Pth events are store in a double linked event ring.  */
struct pth_event_s
{
  struct pth_event_s *next; 
  struct pth_event_s *prev;
  HANDLE hd;                  /* The event object.  */
  int u_type;                 /* The type of the event.  */
  union
  {
    int              fd;      /* Used for PTH_EVENT_FD.  */
    struct 
    {
      int *rc;
      fd_set *rfds;
      fd_set *wfds;
      fd_set *efds;
    } sel;                    /* Used for PTH_EVENT_SELECT.  */
    struct 
    {
      struct sigset_s *set;
      int *signo;
    } sig;                    /* Used for PTH_EVENT_SIGS.  */
    struct timeval   tv;      /* Used for PTH_EVENT_TIME.  */
    pth_mutex_t     *mx;      /* Used for PTH_EVENT_MUTEX.  */
  } u;
  unsigned int flags;   /* Flags used to further describe an event.
                           This is the bit wise combination of
                           PTH_MODE_* or PTH_UNTIL_*.  */
  pth_status_t status;  /* Current status of the event.  */
};


/* Attribute object for threads.  */
struct pth_attr_s 
{
  unsigned int flags;
  unsigned int stack_size;
  char *name;
};


/* Object to keep information about a thread.  This may eventually be
   used to implement a scheduler queue.  */
struct thread_info_s
{
  void *(*thread)(void *); /* The actual thread fucntion.  */
  void * arg;              /* The argument passed to that fucntion.  */
  int joinable;            /* True if this Thread is joinable.  */
  HANDLE th;               /* Handle of this thread.  Used by non-joinable
                              threads to close the handle.  */
};


/* Convenience macro to startup the system.  */
#define implicit_init() do { if (!pth_initialized) pth_init(); } while (0)

/* Prototypes.  */
static pth_event_t do_pth_event (unsigned long spec, ...);
static unsigned int do_pth_waitpid (unsigned pid, int * status, int options);
static int do_pth_wait (pth_event_t ev);
static void *launch_thread (void * ctx);
static int do_pth_event_free (pth_event_t ev, int mode);




static const char *
log_get_prefix (const void *dummy)
{
  return "libw32pth";
}

static char *
w32_strerror (char *strerr, size_t strerrsize)
{
  if (strerrsize > 1)
    FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL, (int)GetLastError (),
                   MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                   strerr, strerrsize, NULL);
  return strerr;
}

static char *
wsa_strerror (char *strerr, size_t strerrsize)
{
  if (strerrsize > 1)
    FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL, (int)WSAGetLastError (),
                   MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                   strerr, strerrsize, NULL);
  return strerr;
}


/* Create a manual resetable event object useable in WFMO.  */
static HANDLE
create_event (void)
{
  SECURITY_ATTRIBUTES sa;
  HANDLE h, h2;
  char strerr[256];

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  h = CreateEvent (&sa, TRUE, FALSE, NULL);
  if (!h)
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: CreateEvent failed: %s\n",
                 log_get_prefix (NULL), 
                 w32_strerror (strerr, sizeof strerr));
      return NULL;
    }
  if (!DuplicateHandle (GetCurrentProcess(), h,
                        GetCurrentProcess(), &h2,
                        EVENT_MODIFY_STATE|SYNCHRONIZE, FALSE, 0 ) ) 
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: "
                 "setting synchronize for event object %p failed: %s\n",
                 log_get_prefix (NULL), h,
                 w32_strerror (strerr, sizeof strerr));
      CloseHandle (h);
      return NULL;
    }
  CloseHandle (h);
  if (DBG_INFO)
    {
      fprintf (stderr, "%s: CreateEvent(%p) succeeded\n",
               log_get_prefix (NULL), h2);
    }
  return h2;
}


static void
set_event (HANDLE h)
{
  char strerr[256];

  if (!SetEvent (h))
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: SetEvent(%p) failed: %s\n",
                 log_get_prefix (NULL), h,
                 w32_strerror (strerr, sizeof strerr));
    }
  else if (DBG_INFO)
    {
      fprintf (stderr, "%s: SetEvent(%p) succeeded\n",
               log_get_prefix (NULL), h);
    }
}

static void
reset_event (HANDLE h)
{
  char strerr[256];

  if (!ResetEvent (h))
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: ResetEvent(%p) failed: %s\n",
                 log_get_prefix (NULL), h,
                 w32_strerror (strerr, sizeof strerr));
    }
  else if (DBG_INFO)
    {
      fprintf (stderr, "%s: ResetEvent(%p) succeeded\n",
               log_get_prefix (NULL), h);
    }
}



/* Create a timer event. */
static HANDLE
create_timer (void)
{
  SECURITY_ATTRIBUTES sa;
  HANDLE h;
  char strerr[256];

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  h = CreateWaitableTimer (&sa, TRUE, NULL);
  if (!h)
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: CreateWaitableTimer failed: %s\n",
                 log_get_prefix (NULL), 
                 w32_strerror (strerr, sizeof strerr));
      return NULL;
    }
  if (DBG_INFO)
    {
      fprintf (stderr, "%s: CreateWaitableTimer(%p) succeeded\n",
               log_get_prefix (NULL), h);
    }
  return h;
}


int
pth_init (void)
{
  WSADATA wsadat;
  const char *s;
  
  if (pth_initialized)
    return TRUE;

  debug_level = (s=getenv ("DEBUG_PTH"))? atoi (s):0;
  if (debug_level)
    fprintf (stderr, "%s: pth_init: called.\n", log_get_prefix (NULL));

  if (WSAStartup (0x202, &wsadat))
    return FALSE;
  pth_signo = 0;
  InitializeCriticalSection (&pth_shd);
  if (pth_signo_ev)
    CloseHandle (pth_signo_ev);

  pth_signo_ev = create_event ();
  if (!pth_signo_ev)
    return FALSE;
  
  pth_initialized = 1;
  EnterCriticalSection (&pth_shd);
  return TRUE;
}


int
pth_kill (void)
{
  pth_signo = 0;
  if (pth_signo_ev)
    {
      CloseHandle (pth_signo_ev);
      pth_signo_ev = NULL;
    }
  if (pth_initialized)
    DeleteCriticalSection (&pth_shd);
  WSACleanup ();
  pth_initialized = 0;
  return TRUE;
}


static void
enter_pth (const char *function)
{
  /* Fixme: I am not sure whether the same thread my enter a critical
     section twice.  */
  if (DBG_CALLS)
    fprintf (stderr, "%s: enter_pth (%s)\n",
             log_get_prefix (NULL), function? function:"");
  LeaveCriticalSection (&pth_shd);
}


static void
leave_pth (const char *function)
{
  EnterCriticalSection (&pth_shd);
  if (DBG_CALLS)
    fprintf (stderr, "%s: leave_pth (%s)\n",
             log_get_prefix (NULL), function? function:"");
}


long 
pth_ctrl (unsigned long query, ...)
{
  implicit_init ();

  switch (query)
    {
    case PTH_CTRL_GETAVLOAD:
    case PTH_CTRL_GETPRIO:
    case PTH_CTRL_GETNAME:
    case PTH_CTRL_GETTHREADS_NEW:
    case PTH_CTRL_GETTHREADS_READY:
    case PTH_CTRL_GETTHREADS_RUNNING:
    case PTH_CTRL_GETTHREADS_WAITING:
    case PTH_CTRL_GETTHREADS_SUSPENDED:
    case PTH_CTRL_GETTHREADS_DEAD:
    case PTH_CTRL_GETTHREADS:
    default:
      return -1;
    }
  return 0;
}



pth_time_t
pth_timeout (long sec, long usec)
{
  pth_time_t tvd;

  tvd.tv_sec  = sec;
  tvd.tv_usec = usec;    
  return tvd;
}


int
pth_read_ev (int fd, void *buffer, size_t size, pth_event_t ev)
{
  implicit_init ();
  return 0;
}


int
pth_read (int fd,  void * buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);

  n = recv (fd, buffer, size, 0);
  if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
    {
      DWORD nread = 0;
      n = ReadFile ((HANDLE)fd, buffer, size, &nread, NULL);
      if (!n)
        {
          char strerr[256];

          if (DBG_ERROR)
            fprintf (stderr, "%s: pth_read(%d) failed read from file: %s\n",
                     log_get_prefix (NULL), fd,
                     w32_strerror (strerr, sizeof strerr));
          n = -1;
        }
      else
        n = (int)nread;
    }
  leave_pth (__FUNCTION__);
  return n;
}


int
pth_write_ev (int fd, const void *buffer, size_t size, pth_event_t ev)
{
  implicit_init ();
  return 0;
}


int
pth_write (int fd, const void * buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = send (fd, buffer, size, 0);
  if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
    {
      DWORD nwrite;
      char strerr[256];

      /* This is no real error because we first need to figure out if
         we have a handle or a socket.  */

      n = WriteFile ((HANDLE)fd, buffer, size, &nwrite, NULL);
      if (!n)
        {
          if (DBG_ERROR)
            fprintf (stderr, "%s: pth_write(%d) failed in write: %s\n",
                     log_get_prefix (NULL), fd,
                     w32_strerror (strerr, sizeof strerr));
          n = -1;
        }
      else
        n = (int)nwrite;
    }
  else if (n == -1)
    {
      char strerr[256];

      if (DBG_ERROR)
        fprintf (stderr, "%s: pth_write(%d) failed in send: %s\n",
                 log_get_prefix (NULL), fd,
                 w32_strerror (strerr, sizeof strerr));
      n = -1;
    }
  leave_pth (__FUNCTION__);
  return n;
}


int
pth_select (int nfd, fd_set * rfds, fd_set * wfds, fd_set * efds,
	    const struct timeval * timeout)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = select (nfd, rfds, wfds, efds, timeout);
  leave_pth (__FUNCTION__);
  return n;
}

static void
show_event_ring (const char *text, pth_event_t ev)
{
  pth_event_t r;

  if (!ev)
    {
      fprintf (stderr, "show_event_ring(%s):  No ring\n", text);
      return;
    }

  r = ev;
  do
    {
      fprintf (stderr, "show_event_ring(%s): type=%d r=%p prev=%p next=%p\n",
               text, r->u_type, r, r->prev, r->next);
    }
  while (r=r->next, r != ev);
}
  


int
pth_select_ev (int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds,
               const struct timeval *timeout, pth_event_t ev_extra)
{
  int rc, sel_rc;
  pth_event_t ev;
  pth_event_t ev_time = NULL;
  int selected;

  implicit_init ();
  enter_pth (__FUNCTION__);

  ev = do_pth_event (PTH_EVENT_SELECT, &sel_rc, nfd, rfds, wfds, efds);
  if (!ev)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }
  if (timeout)
    {
      ev_time = do_pth_event (PTH_EVENT_TIME, 
                              pth_timeout (timeout->tv_sec, timeout->tv_usec));
      if (!ev_time)
        {
          rc = -1;
          goto leave;
        }
      pth_event_concat (ev, ev_time, NULL);
    }
  if (ev_extra)
    pth_event_concat (ev, ev_extra, NULL);

  do 
    {
      rc = do_pth_wait (ev);
      if (rc < 0)
        goto leave;
    }
  while (!rc);
  
  pth_event_isolate (ev);
  if (timeout)
    pth_event_isolate (ev_time);

  if (DBG_INFO)
    {
      show_event_ring ("ev      ", ev);
      show_event_ring ("ev_time ", ev_time);
      show_event_ring ("ev_extra", ev_extra); 
    }

  /* Fixme: We should check whether select failed and return EBADF in
     this case.  */
  selected = (ev && ev->status == PTH_STATUS_OCCURRED);
  if (selected)
    rc = sel_rc;
  if (timeout && (ev_time && ev_time->status == PTH_STATUS_OCCURRED))
    {
      selected = 1;
      if (rfds)
        FD_ZERO(rfds);
      if (wfds)
        FD_ZERO(wfds);
      if (efds)
        FD_ZERO(efds);
      rc = 0;
    }
  if (ev_extra && !selected)
    {
      rc = -1;
      errno = EINTR;
    }

 leave:
  do_pth_event_free (ev, PTH_FREE_THIS);
  do_pth_event_free (ev_time, PTH_FREE_THIS);

  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_fdmode (int fd, int mode)
{
  unsigned long val;
  int ret = PTH_FDMODE_BLOCK;

  implicit_init ();
  /* Note: We don't do the enter/leave pth here because this is for one
     a fast function and secondly already called from inside such a
     block.  */
  /* XXX: figure out original fd mode */
  switch (mode)
    {
    case PTH_FDMODE_NONBLOCK:
      val = 1;
      if (ioctlsocket (fd, FIONBIO, &val) == SOCKET_ERROR)
        ret = PTH_FDMODE_ERROR;
      break;

    case PTH_FDMODE_BLOCK:
      val = 0;
      if (ioctlsocket (fd, FIONBIO, &val) == SOCKET_ERROR)
        ret = PTH_FDMODE_ERROR;
      break;
    }
  return ret;
}


int
pth_accept (int fd, struct sockaddr *addr, int *addrlen)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = accept (fd, addr, addrlen);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_accept_ev (int fd, struct sockaddr *addr, int *addrlen,
               pth_event_t ev_extra)
{
  pth_key_t ev_key;
  pth_event_t ev;
  int rv;
  int fdmode;

  implicit_init ();
  enter_pth (__FUNCTION__);

  fdmode = pth_fdmode (fd, PTH_FDMODE_NONBLOCK);
  if (fdmode == PTH_FDMODE_ERROR)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }

  ev = NULL;
  while ((rv = accept (fd, addr, addrlen)) == -1 && 
         (WSAGetLastError () == WSAEINPROGRESS || 
          WSAGetLastError () == WSAEWOULDBLOCK))
    {
      if (!ev)
        {
          ev = do_pth_event (PTH_EVENT_FD|PTH_UNTIL_FD_READABLE|
                             PTH_MODE_STATIC, &ev_key, fd);
          if (!ev)
            {
              leave_pth (__FUNCTION__);
              return -1;
            }
          if (ev_extra)
            pth_event_concat (ev, ev_extra, NULL);
        }
      /* Wait until accept has a chance. */
      do_pth_wait (ev);
      if (ev_extra)
        {
          pth_event_isolate (ev);
          if (ev && ev->status != PTH_STATUS_OCCURRED)
            {
              pth_fdmode (fd, fdmode);
              leave_pth (__FUNCTION__);
              return -1;
            }
        }
    }

  pth_fdmode (fd, fdmode);
  leave_pth (__FUNCTION__);
  return rv;   
}


int
pth_connect (int fd, struct sockaddr *name, int namelen)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = connect (fd, name, namelen);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_mutex_release (pth_mutex_t *mutex)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);

  if (!ReleaseMutex (*mutex))
    {
      char strerr[256];

      if (DBG_ERROR)
        fprintf (stderr, "%s: pth_release_mutex %p failed: %s\n",
                 log_get_prefix (NULL), *mutex,
                 w32_strerror (strerr, sizeof strerr));
      rc = FALSE;
    }
  else
    rc = TRUE;

  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_mutex_acquire (pth_mutex_t *mutex, int tryonly, pth_event_t ev_extra)
{
  int code;
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);

  /* FIXME: ev_extra is not yet supported.  */
  
  code = WaitForSingleObject (*mutex, INFINITE);
  switch (code) 
    {
      case WAIT_FAILED:
        {
          char strerr[256];
          
          if (DBG_ERROR)
            fprintf (stderr, "%s: pth_mutex_acquire for %p failed: %s\n",
                     log_get_prefix (NULL), *mutex,
                     w32_strerror (strerr, sizeof strerr));
        }
        rc = FALSE;
        break;
        
      case WAIT_OBJECT_0:
        rc = TRUE;
        break;

      default:
        if (DBG_ERROR)
          fprintf (stderr, "%s: WaitForSingleObject returned unexpected "
                   "code %d for mutex %p\n",
                   log_get_prefix (NULL), code, *mutex);
        rc = FALSE;
        break;
    }

  leave_pth (__FUNCTION__);
  return rc;
}



int
pth_mutex_init (pth_mutex_t *mutex)
{
  SECURITY_ATTRIBUTES sa;
  
  implicit_init ();
  enter_pth (__FUNCTION__);

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  *mutex = CreateMutex (&sa, FALSE, NULL);
  if (!*mutex)
   {
      leave_pth (__FUNCTION__);
      return FALSE;
    }
    
  leave_pth (__FUNCTION__);
  return TRUE;
}


int
pth_rwlock_init (pth_rwlock_t *rwlock)
{
  /* FIXME */
  return pth_mutex_init (rwlock);
}


int
pth_rwlock_acquire (pth_rwlock_t *rwlock, int op, int try, pth_event_t ev)
{
  /* FIXME */
  return pth_mutex_acquire (rwlock, try, ev);
}

int
pth_rwlock_release (pth_rwlock_t *rwlock)
{
  /* FIXME */
  return pth_mutex_release (rwlock);
}


pth_attr_t
pth_attr_new (void)
{
  pth_attr_t hd;

  implicit_init ();
  hd = calloc (1, sizeof *hd);
  return hd;
}


int
pth_attr_destroy (pth_attr_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  if (hd->name)
    free (hd->name);
  free (hd);
  return TRUE;
}


int
pth_attr_set (pth_attr_t hd, int field, ...)
{    
  va_list args;
  char * str;
  int val;
  int rc = TRUE;

  implicit_init ();

  va_start (args, field);
  switch (field)
    {
    case PTH_ATTR_JOINABLE:
      val = va_arg (args, int);
      if (val)
        {
          hd->flags |= PTH_ATTR_JOINABLE;
          if (DBG_INFO)
            fprintf (stderr, "%s: pth_attr_set: PTH_ATTR_JOINABLE\n",
                     log_get_prefix (NULL));
        }
      break;

    case PTH_ATTR_STACK_SIZE:
      val = va_arg (args, int);
      if (val)
        {
          hd->flags |= PTH_ATTR_STACK_SIZE;
          hd->stack_size = val;
          if (DBG_INFO)
            fprintf (stderr, "%s: pth_attr_set: PTH_ATTR_STACK_SIZE %d\n",
                     log_get_prefix (NULL), val);
        }
      break;

    case PTH_ATTR_NAME:
      str = va_arg (args, char*);
      if (hd->name)
        free (hd->name);
      if (str)
        {
          hd->name = strdup (str);
          if (!hd->name)
            return FALSE;
          hd->flags |= PTH_ATTR_NAME;
          if (DBG_INFO)
            fprintf (stderr, "%s: pth_attr_set: PTH_ATTR_NAME %s\n",
                     log_get_prefix (NULL), hd->name);
        }
      break;

    default:
      rc = FALSE;
      break;
    }
  va_end (args);
  return rc;
}


static pth_t
do_pth_spawn (pth_attr_t hd, void *(*func)(void *), void *arg)
{
  SECURITY_ATTRIBUTES sa;
  DWORD tid;
  HANDLE th;
  struct thread_info_s *ctx;

  if (!hd)
    return NULL;

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;

  ctx = calloc (1, sizeof *ctx);
  if (!ctx)
    return NULL;
  ctx->thread = func;
  ctx->arg = arg;
  ctx->joinable = (hd->flags & PTH_ATTR_JOINABLE);

  /* XXX: we don't use all thread attributes. */

  /* Note that we create the thread suspended so that we are able to
     store the thread's handle in the context structure.  We need to
     do this to be able to close the handle from the launch helper. 

     FIXME: We should not use W32's thread handle directly but keep
     our own thread control structure.  CTX may be used for that.  */
  if (DBG_INFO)
    fprintf (stderr, "%s: do_pth_spawn creating thread ...\n",
             log_get_prefix (NULL));
  th = CreateThread (&sa, hd->stack_size,
                     (LPTHREAD_START_ROUTINE)launch_thread,
                     ctx, CREATE_SUSPENDED, &tid);
  ctx->th = th;
  if (DBG_INFO)
    fprintf (stderr, "%s: do_pth_spawn created thread %p\n",
             log_get_prefix (NULL),th);
  if (!th)
    free (ctx);
  else
    ResumeThread (th);
  
  return th;
}

pth_t
pth_spawn (pth_attr_t hd, void *(*func)(void *), void *arg)
{
  HANDLE th;

  if (!hd)
    return NULL;

  implicit_init ();
  enter_pth (__FUNCTION__);
  th = do_pth_spawn (hd, func, arg);
  leave_pth (__FUNCTION__);
  return th;
}


pth_t 
pth_self (void)
{
  return GetCurrentThread ();
}

int
pth_join (pth_t hd, void **value)
{
  return TRUE;
}


/* friendly */
int
pth_cancel (pth_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  WaitForSingleObject (hd, 1000);
  TerminateThread (hd, 0);
  leave_pth (__FUNCTION__);
  return TRUE;
}


/* cruel */
int
pth_abort (pth_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  TerminateThread (hd, 0);
  leave_pth (__FUNCTION__);
  return TRUE;
}


void
pth_exit (void *value)
{
  implicit_init ();
  enter_pth (__FUNCTION__);
  pth_kill ();
  leave_pth (__FUNCTION__);
  exit ((int)(long)value);
}


static unsigned int
do_pth_waitpid (unsigned pid, int * status, int options)
{
#if 0
  pth_event_t ev;
  static pth_key_t ev_key = PTH_KEY_INIT;
  pid_t pid;

  pth_debug2("pth_waitpid: called from thread \"%s\"", pth_current->name);

  for (;;)
    {
      /* do a non-blocking poll for the pid */
      while (   (pid = pth_sc(waitpid)(wpid, status, options|WNOHANG)) < 0
                && errno == EINTR)
        ;

      /* if pid was found or caller requested a polling return immediately */
      if (pid == -1 || pid > 0 || (pid == 0 && (options & WNOHANG)))
        break;

      /* else wait a little bit */
      ev = pth_event(PTH_EVENT_TIME|PTH_MODE_STATIC, &ev_key,
                     pth_timeout (0,250000));
      pth_wait(ev);
    }

  pth_debug2("pth_waitpid: leave to thread \"%s\"", pth_current->name);
#endif
  return 0;
}


unsigned int
pth_waitpid (unsigned pid, int * status, int options)
{
  unsigned int n;

  implicit_init ();
  enter_pth (__FUNCTION__);
  n = do_pth_waitpid (pid, status, options);
  leave_pth (__FUNCTION__);
  return n;
}


static BOOL WINAPI
sig_handler (DWORD signo)
{
  switch (signo)
    {
    case CTRL_C_EVENT:     pth_signo = SIGINT; break;
    case CTRL_BREAK_EVENT: pth_signo = SIGTERM; break;
    default:
      return FALSE;
    }
  /* Fixme: We can keep only track of one signal at a time. */
  set_event (pth_signo_ev);
  if (DBG_INFO)
    fprintf (stderr, "%s: sig_handler=%d\n", log_get_prefix (NULL), pth_signo);
  return TRUE;
}


/* Helper to build an fdarray.  */
static int
build_fdarray (struct fdarray_item_s *fdarray, int nfdarray,
               fd_set *fds, long netevents)
{
  int i, j, fd;

  if (fds)
    for (i=0; i < fds->fd_count; i++)
      {
        fd = fds->fd_array[i];
        for (j=0; j < nfdarray; j++)
          if (fdarray[j].fd == fd)
            {
              fdarray[j].netevents |= netevents;
              break;
            }
        if (!(j < nfdarray) && nfdarray < FD_SETSIZE)
          {
            fdarray[nfdarray].fd = fd;
            fdarray[nfdarray].netevents = netevents;
            nfdarray++;
          }
      }
  return nfdarray;
}


static pth_event_t
do_pth_event_body (unsigned long spec, va_list arg)
{
  char strerr[256];
  pth_event_t ev;
  int i, rc;

  if ((spec & (PTH_MODE_CHAIN|PTH_MODE_REUSE)))
    {
      if (DBG_ERROR)
        fprintf (stderr, "%s: pth_event spec=%lu - not supported\n", 
                 log_get_prefix (NULL), spec);
      return NULL; /* Not supported.  */
    }

  if (DBG_INFO)
    fprintf (stderr, "%s: pth_event spec=%lu\n", log_get_prefix (NULL), spec);

  ev = calloc (1, sizeof *ev);
  if (!ev)
    return NULL;
  ev->next = ev;
  ev->prev = ev;
  if ((spec & PTH_EVENT_TIME))
    ev->hd = create_timer ();
  else
    ev->hd = create_event ();
  if (!ev->hd)
    {
      free (ev);
      return NULL;
    }

  /* We don't support static yet but we need to consume the
     argument.  */
  if ( (spec & PTH_MODE_STATIC) )
    {
      ev->flags |= PTH_MODE_STATIC;
      va_arg (arg, pth_key_t);
    }

  ev->status = PTH_STATUS_PENDING;

  if (spec == 0)
    ;
  else if (spec & PTH_EVENT_SIGS)
    {
      ev->u_type = PTH_EVENT_SIGS;
      ev->u.sig.set = va_arg (arg, struct sigset_s *);
      ev->u.sig.signo = va_arg (arg, int *);	
      /* The signal handler is disabled for now.  */
      rc = 0/*SetConsoleCtrlHandler (sig_handler, TRUE)*/;
      if (DBG_INFO)
        fprintf (stderr, "%s: pth_event: sigs rc=%d\n",
                 log_get_prefix (NULL), rc);
    }
  else if (spec & PTH_EVENT_FD)
    {
      if (spec & PTH_UNTIL_FD_READABLE)
        ev->flags |= PTH_UNTIL_FD_READABLE;
      ev->u_type = PTH_EVENT_FD;
      ev->u.fd = va_arg (arg, int);
      if (DBG_INFO)
        fprintf (stderr, "%s: pth_event: fd=%d\n",
                 log_get_prefix (NULL), ev->u.fd);
    }
  else if (spec & PTH_EVENT_TIME)
    {
      pth_time_t t;
      t = va_arg (arg, pth_time_t);
      ev->u_type = PTH_EVENT_TIME;
      ev->u.tv.tv_sec =  t.tv_sec;
      ev->u.tv.tv_usec = t.tv_usec;
    }
  else if (spec & PTH_EVENT_MUTEX)
    {
      ev->u_type = PTH_EVENT_MUTEX;
      ev->u.mx = va_arg (arg, pth_mutex_t*);
    }
  else if (spec & PTH_EVENT_SELECT)
    {
      struct fdarray_item_s fdarray[FD_SETSIZE];
      int nfdarray;

      ev->u_type = PTH_EVENT_SELECT;
      ev->u.sel.rc = va_arg (arg, int *);
      (void)va_arg (arg, int); /* Ignored. */
      ev->u.sel.rfds = va_arg (arg, fd_set *);
      ev->u.sel.wfds = va_arg (arg, fd_set *);
      ev->u.sel.efds = va_arg (arg, fd_set *);
      nfdarray = 0;
      nfdarray = build_fdarray (fdarray, nfdarray, 
                                ev->u.sel.rfds, (FD_READ|FD_ACCEPT) );
      nfdarray = build_fdarray (fdarray, nfdarray, 
                                ev->u.sel.wfds, (FD_WRITE) );
      nfdarray = build_fdarray (fdarray, nfdarray, 
                                ev->u.sel.efds, (FD_OOB|FD_CLOSE) );

      for (i=0; i < nfdarray; i++)
        {
          if (WSAEventSelect (fdarray[i].fd, ev->hd, fdarray[i].netevents))
            {
              if (DBG_ERROR)
                fprintf (stderr, 
                         "%s: pth_event: WSAEventSelect(%d[%d]) failed: %s\n",
                         log_get_prefix (NULL), i, fdarray[i].fd,
                         wsa_strerror (strerr, sizeof strerr));
            }
        }
    }

  return ev;
}

static pth_event_t
do_pth_event (unsigned long spec, ...)
{
  va_list arg;
  pth_event_t ev;

  va_start (arg, spec);
  ev = do_pth_event_body (spec, arg);
  va_end (arg);
    
  return ev;
}

pth_event_t
pth_event (unsigned long spec, ...)
{
  va_list arg;
  pth_event_t ev;

  implicit_init ();
  enter_pth (__FUNCTION__);
  
  va_start (arg, spec);
  ev = do_pth_event_body (spec, arg);
  va_end (arg);
    
  leave_pth (__FUNCTION__);
  return ev;
}


pth_event_t
pth_event_concat (pth_event_t head, ...)
{
  pth_event_t ev, next, last, tmp;
  va_list ap;

  if (!head)
    return NULL;

  implicit_init ();

  ev = head;
  last = ev->next;
  va_start (ap, head);
  while ( (next = va_arg (ap, pth_event_t)))
    {
      ev->next = next;
      tmp = next->prev;
      next->prev = ev;
      ev = tmp;
    }
  va_end (ap);

  ev->next = last;
  last->prev = ev;

  return head;
}


static int
wait_for_fd (int fd, int is_read, int nwait)
{
  struct timeval tv;
  fd_set r;
  fd_set w;
  int n;

  FD_ZERO (&r);
  FD_ZERO (&w);    
  FD_SET (fd, is_read ? &r : &w);

  tv.tv_sec = nwait;
  tv.tv_usec = 0;

  while (1)
    {
      n = select (fd+1, &r, &w, NULL, &tv);
      if (DBG_INFO)
        fprintf (stderr, "%s: wait_for_fd=%d fd %d (ec=%d)\n",
                 log_get_prefix (NULL), n, fd,(int)WSAGetLastError ());
      if (n == -1)
        break;
      if (!n)
        continue;
      if (n == 1)
        {
          if (is_read && FD_ISSET (fd, &r))
            break;
          else if (FD_ISSET (fd, &w))
            break;
        }
    }
  return 0;
}





static void *
launch_thread (void *arg)
{
  struct thread_info_s *c = arg;

  if (c)
    {
      leave_pth (__FUNCTION__);
      c->thread (c->arg);
      if (!c->joinable && c->th)
        {
          CloseHandle (c->th);
          c->th = NULL;
        }
      /* FIXME: We would badly fail if someone accesses the now
         deallocated handle. Don't use it directly but setup proper
         scheduling queues.  */
      enter_pth (__FUNCTION__);
      free (c);
    }
  ExitThread (0);
  return NULL;
}

/* void */
/* sigemptyset (struct sigset_s * ss) */
/* { */
/*     if (ss) { */
/* 	memset (ss->sigs, 0, sizeof ss->sigs); */
/* 	ss->idx = 0; */
/*     } */
/* } */


/* int */
/* sigaddset (struct sigset_s * ss, int signo) */
/* { */
/*     if (!ss) */
/* 	return -1; */
/*     if (ss->idx + 1 > 64) */
/* 	return -1; */
/*     ss->sigs[ss->idx] = signo; */
/*     ss->idx++; */
/*     return 0; */
/* }  */


/* static int */
/* sigpresent (struct sigset_s * ss, int signo) */
/* { */
/*     int i; */
/*     for (i=0; i < ss->idx; i++) { */
/* 	if (ss->sigs[i] == signo) */
/* 	    return 1; */
/*     } */
/* FIXME: See how to implement it.  */
/*     return 0; */
/* } */



int
pth_event_status (pth_event_t ev)
{
  int ret; 

  if (!ev)
    return 0;
  implicit_init ();
  enter_pth (__FUNCTION__);
  ret = ev? ev->status : 0;;
  leave_pth (__FUNCTION__);
  return ret;
}


int
pth_event_occurred (pth_event_t ev)
{
  return pth_event_status (ev) == PTH_STATUS_OCCURRED;
}



static int
do_pth_event_free (pth_event_t ev, int mode)
{
  if (!ev)
    return FALSE;

  if (mode == PTH_FREE_ALL)
    {
      pth_event_t cur = ev;
      do
        {
          pth_event_t next = cur->next;
          CloseHandle (cur->hd);
          cur->hd = NULL;
          free (cur);
          cur = next;
        }
      while (cur != ev);
    }
  else if (mode == PTH_FREE_THIS)
    {
      ev->prev->next = ev->next;
      ev->next->prev = ev->prev;
      CloseHandle (ev->hd);
      ev->hd = NULL;	    
      free (ev);
    }
  else
    return FALSE;

  return TRUE;
}

int
pth_event_free (pth_event_t ev, int mode)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = do_pth_event_free (ev, mode);
  leave_pth (__FUNCTION__);
  return rc;
}


pth_event_t
pth_event_isolate (pth_event_t ev)
{
  pth_event_t ring;

  if (!ev)
    return NULL;
  if (ev->next == ev && ev->prev == ev)
    return NULL; /* Only one event.  */

  ring = ev->next;
  ev->prev->next = ev->next;
  ev->next->prev = ev->prev;
  ev->prev = ev;
  ev->next = ev;
  return ring;    
}


static int
event_count (pth_event_t ev)
{
  pth_event_t r;
  int cnt = 0;

  if (ev)
    {
      r = ev;
      do
        {
          cnt++;
          r = r->next;
        }
      while (r != ev);
    }

  return cnt;
}



static pth_t
spawn_helper_thread (void *(*func)(void *), void *arg)
{
  SECURITY_ATTRIBUTES sa;
  DWORD tid;
  HANDLE th;

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;

  if (DBG_INFO)
    fprintf (stderr, "%s: spawn_helper_thread creating thread ...\n",
             log_get_prefix (NULL));
  th = CreateThread (&sa, 32*1024,
                     (LPTHREAD_START_ROUTINE)func,
                     arg, 0, &tid);
  if (DBG_INFO)
    fprintf (stderr, "%s: spawn_helper_thread created thread %p\n",
             log_get_prefix (NULL), th);

  return th;
}



static void *
wait_fd_thread (void * ctx)
{
  pth_event_t ev = ctx;

  wait_for_fd (ev->u.fd, ev->flags & PTH_UNTIL_FD_READABLE, 3600);
  if (DBG_INFO)
    fprintf (stderr, "%s: wait_fd_thread: exit.\n", log_get_prefix (NULL));
  set_event (ev->hd);
  ExitThread (0);
  return NULL;
}



static int
do_pth_wait (pth_event_t ev)
{
  char strerr[256];
  HANDLE waitbuf[MAXIMUM_WAIT_OBJECTS/2];
  pth_event_t evarray[MAXIMUM_WAIT_OBJECTS/2];
  HANDLE threadlist[MAXIMUM_WAIT_OBJECTS/2];
  DWORD n;
  int pos, idx, thlstidx, i;
  pth_event_t r;

  if (!ev)
    return 0;

  n = event_count (ev);
  if (n > MAXIMUM_WAIT_OBJECTS/2)
    return -1;

  if (DBG_INFO)
    fprintf (stderr, "%s: pth_wait: cnt %lu\n", log_get_prefix (NULL), n);

  /* Set all events to pending.  */
  r = ev;
  do 
    {
      r->status = PTH_STATUS_PENDING;
      r = r->next;
    }
  while ( r != ev);

  /* Prepare all events which requires to launch helper threads for
     some types.  This creates an array of handles which are lates
     passed to WFMO. */
  pos = thlstidx = 0;
  r = ev;
  do
    {
      switch (r->u_type)
        {
        case PTH_EVENT_SIGS:
          if (DBG_INFO)
            fprintf (stderr, "pth_wait: add signal event\n");
          /* Register the global signal event.  */
          evarray[pos] = r;  
          waitbuf[pos++] = pth_signo_ev;
          break;
          
        case PTH_EVENT_FD:
          if (DBG_INFO)
            fprintf (stderr, "pth_wait: spawn wait_fd_thread\n");
          evarray[pos] = r;  
          waitbuf[pos++] = r->hd;
          threadlist[thlstidx++] = spawn_helper_thread (wait_fd_thread, r);
          break;
          
        case PTH_EVENT_TIME:
          if (DBG_INFO)
            fprintf (stderr, "pth_wait: adding timer event\n");
          {
            LARGE_INTEGER ll;

            ll.QuadPart = - (r->u.tv.tv_sec * 10000000ll
                             + r->u.tv.tv_usec * 10); 
            if (!SetWaitableTimer (r->hd, &ll, 0, NULL, NULL, FALSE))
              {
                if (DBG_ERROR)
                  fprintf (stderr,"%s: %s: SetWaitableTimer failed: %s\n",
                           log_get_prefix (NULL), __func__,
                           w32_strerror (strerr, sizeof strerr));
                return -1;
              }
            evarray[pos] = r;  
            waitbuf[pos++] = r->hd;
          }
          break;

        case PTH_EVENT_SELECT:
          if (DBG_INFO)
            fprintf (stderr, "pth_wait: adding select event\n");
          evarray[pos] = r;  
          waitbuf[pos++] = r->hd;
          break;

        case PTH_EVENT_MUTEX:
          if (DBG_ERROR)
            fprintf (stderr, "pth_wait: ignoring mutex event.\n");
          break;
        }
      r = r->next;
    }
  while ( r != ev );

  if (DBG_INFO)
    {
      fprintf (stderr, "%s: pth_wait: WFMO n=%d\n", 
               log_get_prefix (NULL), pos);
      for (i=0; i < pos; i++)
        fprintf (stderr, "%s: pth_wait:      %d=%p\n", 
                 log_get_prefix (NULL), i, waitbuf[i]);
    }
  n = WaitForMultipleObjects (pos, waitbuf, FALSE, INFINITE);
  /* FIXME: We need to cancel all threads or keep them in a list so
     that they are reused if we need to wait on the same event again.
     Hmmm, that is all bullshit: We need to write a real
     scheduler.  */
  for (i=0; i < thlstidx; i++)
    CloseHandle (threadlist[i]);
  if (DBG_INFO)
    fprintf (stderr, "%s: pth_wait: WFMO returned %ld\n",
             log_get_prefix (NULL), n);

  if (n >= 0 && n < pos)
    {
      int count;
      /* At least one object has been signaled.  Walk over all events
         with an assigned handle and update the status.  We start at N
         which indicates the lowest signaled event.  */
      for (count = 0, idx = 0; idx < pos; idx++)
        if (WaitForSingleObject (evarray[idx]->hd, 0) == WAIT_OBJECT_0)
          {
            r = evarray[idx];

            if (DBG_INFO)
              fprintf (stderr, "%s: pth_wait: setting %d ev=%p\n",
                       __func__, idx, r);
            r->status = PTH_STATUS_OCCURRED;
            count++;
            switch (r->u_type)
              {
              case PTH_EVENT_SIGS:
                *(r->u.sig.signo) = pth_signo;
                break;
              case PTH_EVENT_SELECT:
                {
                  struct fdarray_item_s fdarray[FD_SETSIZE];
                  int nfdarray;
                  WSANETWORKEVENTS ne;
                  int ntotal = 0;
                  unsigned long val;
                  
                  nfdarray = 0;
                  nfdarray = build_fdarray (fdarray, nfdarray, 
                                            ev->u.sel.rfds, 0 );
                  nfdarray = build_fdarray (fdarray, nfdarray, 
                                            ev->u.sel.wfds, 0 );
                  nfdarray = build_fdarray (fdarray, nfdarray, 
                                            ev->u.sel.efds, 0 );

                  if (r->u.sel.rfds)
                    FD_ZERO (r->u.sel.rfds);
                  if (r->u.sel.wfds)
                    FD_ZERO (r->u.sel.wfds);
                  if (r->u.sel.efds)
                    FD_ZERO (r->u.sel.efds);
                  for (i=0; i < nfdarray; i++)
                    {
                      if (WSAEnumNetworkEvents (fdarray[i].fd, NULL, &ne))
                        {
                          if (DBG_ERROR)
                            fprintf (stderr, 
                                   "%s: pth_wait: "
                                   "WSAEnumNetworkEvents(%d[%d]) failed: %s\n",
                                   log_get_prefix (NULL), i, fdarray[i].fd,
                                   wsa_strerror (strerr, sizeof strerr));
                          continue;
                        }

                      if (r->u.sel.rfds 
                          && (ne.lNetworkEvents & (FD_READ|FD_ACCEPT)))
                        {
                          FD_SET (fdarray[i].fd, r->u.sel.rfds);
                          ntotal++;
                        }
                      if (r->u.sel.wfds 
                          && (ne.lNetworkEvents & (FD_WRITE)))
                        {
                          FD_SET (fdarray[i].fd, r->u.sel.wfds);
                          ntotal++;
                        }
                      if (r->u.sel.efds 
                          && (ne.lNetworkEvents & (FD_OOB|FD_CLOSE)))
                        {
                          FD_SET (fdarray[i].fd, r->u.sel.efds);
                          ntotal++;
                        }

                      /* Set the socket back to blocking mode.  */
                      /* Fixme: Do thsi only if the socket was in
                         blocking mode.  */
                      if (WSAEventSelect (fdarray[i].fd, NULL, 0))
                        {
                          if (DBG_ERROR)
                            fprintf (stderr, 
                                 "%s: pth_wait: WSAEventSelect(%d[%d]-clear)"
                                 " failed: %s\n",
                                 log_get_prefix (NULL), i, fdarray[i].fd,
                                 wsa_strerror (strerr, sizeof strerr));
                        }

                      val = 0;
                      if (ioctlsocket (fdarray[i].fd, FIONBIO, &val)
                          == SOCKET_ERROR)
                        {
                          if (DBG_ERROR)
                            fprintf (stderr, 
                                 "%s: pth_wait: ioctlsocket(%d[%d])"
                                 " failed: %s\n",
                                 log_get_prefix (NULL), i, fdarray[i].fd,
                                 wsa_strerror (strerr, sizeof strerr));
                        }


                    }
                  *r->u.sel.rc = ntotal;
                }
                break;
              }
            
            /* We don't reset Timer events and I don't know whether
               resetEvent will work at all.  SetWaitableTimer resets
               the timer. */
            if (r->u_type != PTH_EVENT_TIME)
              reset_event (evarray[idx]->hd);
          }
      if (DBG_INFO)
        fprintf (stderr, "%s: pth_wait: %d events have been signalled\n",
                 log_get_prefix (NULL), count);
      return count;
    }
  else if (n == WAIT_TIMEOUT)
    return 0;
  else
    return -1;
}


int
pth_wait (pth_event_t ev)
{
  int rc;

  implicit_init ();
  enter_pth (__FUNCTION__);
  rc = do_pth_wait (ev);
  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_sleep (int sec)
{
  static pth_key_t ev_key = PTH_KEY_INIT;
  pth_event_t ev;

  implicit_init ();
  enter_pth (__FUNCTION__);

  if (sec == 0)
    {
      leave_pth (__FUNCTION__);
      return 0;
    }

  ev = do_pth_event (PTH_EVENT_TIME|PTH_MODE_STATIC, &ev_key,
                     pth_timeout (sec, 0));
  if (ev == NULL)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }
  do_pth_wait (ev);
  do_pth_event_free (ev, PTH_FREE_ALL);

  leave_pth (__FUNCTION__);
  return 0;
}


int
pth_sigmask (int how, const sigset_t *set, sigset_t *old)
{

  return 0;
}




/* 
   Some simple tests.  
 */
#ifdef TEST
#include <stdio.h>

void * thread (void * c)
{

  Sleep (2000);
  set_event (((pth_event_t)c)->hd);
  fprintf (stderr, "\n\nhallo!.\n");
  pth_exit (NULL);
  return NULL;
}


int main_1 (int argc, char ** argv)
{
  pth_attr_t t;
  pth_t hd;
  pth_event_t ev;

  pth_init ();
  ev = pth_event (0, NULL);
  t = pth_attr_new ();
  pth_attr_set (t, PTH_ATTR_JOINABLE, 1);
  pth_attr_set (t, PTH_ATTR_STACK_SIZE, 4096);
  pth_attr_set (t, PTH_ATTR_NAME, "hello");
  hd = pth_spawn (t, thread, ev);

  pth_wait (ev);
  pth_attr_destroy (t);
  pth_event_free (ev, 0);
  pth_kill ();

  return 0;
}


static pth_event_t 
setup_signals (struct sigset_s *sigs, int *signo)
{
  pth_event_t ev;

  sigemptyset (sigs);
  sigaddset (sigs, SIGINT);
  sigaddset (sigs, SIGTERM);

  ev = pth_event (PTH_EVENT_SIGS, sigs, signo);
  return ev;
}

int
main_2 (int argc, char ** argv)
{
  pth_event_t ev;
  struct sigset_s sigs;
  int signo = 0;

  pth_init ();
  ev = setup_signals (&sigs, &signo);
  pth_wait (ev);
  if (pth_event_occured (ev) && signo)
    fprintf (stderr, "signal caught! signo %d\n", signo);

  pth_event_free (ev, PTH_FREE_ALL);
  pth_kill ();
  return 0;
}

int
main_3 (int argc, char ** argv)
{
  struct sockaddr_in addr, rem;
  int fd, n = 0, infd;
  int signo = 0;
  struct sigset_s sigs;
  pth_event_t ev;

  pth_init ();
  fd = socket (AF_INET, SOCK_STREAM, 0);

  memset (&addr, 0, sizeof addr);
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons (5050);
  addr.sin_family = AF_INET;
  bind (fd, (struct sockaddr*)&addr, sizeof addr);
  listen (fd, 5);

  ev = setup_signals (&sigs, &signo);
  n = sizeof addr;
  infd = pth_accept_ev (fd, (struct sockaddr *)&rem, &n, ev);
  fprintf (stderr, "infd %d: %s:%d\n", infd, inet_ntoa (rem.sin_addr),
          htons (rem.sin_port));

  closesocket (infd);
  pth_event_free (ev, PTH_FREE_ALL);
  pth_kill ();
  return 0;
}

int
main (int argc, char ** argv)
{
  pth_event_t ev;
  pth_key_t ev_key;

  pth_init ();
  /*ev = pth_event (PTH_EVENT_TIME, &ev_key, pth_timeout (5, 0));
    pth_wait (ev);
    pth_event_free (ev, PTH_FREE_ALL);*/
  pth_sleep (5);
  pth_kill ();
  return 0;
}
#endif

