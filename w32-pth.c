/* w32-pth.c - GNU Pth emulation for W32 (MS Windows).
 * Copyright (c) 1999-2003 Ralf S. Engelschall <rse@engelschall.com>
 * Copyright (C) 2004, 2006, 2007, 2008, 2010 g10 Code GmbH
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
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * ------------------------------------------------------------------
 * This code is based on Ralf Engelschall's GNU Pth, a non-preemptive
 * thread scheduling library which can be found at
 * http://www.gnu.org/software/pth/.  MS Windows (W32) specific code
 * written by Timo Schulz, g10 Code.
 */

#include <config.h>

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#include <errno.h>

#include "utils.h"
#include "debug.h"
#include "w32-io.h"

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

/* FIXME: We can only undefine this when we have static thread-local
   event allocation.  */
#define NO_PTH_MODE_STATIC 1

/* States whether this module has been initialized.  */
static int pth_initialized;

/* Debug helpers.  */
int debug_level;
#ifdef HAVE_W32CE_SYSTEM
HANDLE dbghd = INVALID_HANDLE_VALUE;
#else
FILE *dbgfp;
#endif

/* Variables to support event handling. */
static int pth_signo;
static HANDLE pth_signo_ev;

/* Mutex to make sure only one thread is running. */
static CRITICAL_SECTION pth_shd;

/* A sentinel to catch bogus use of pth_enter/pth_leave.  */
static int enter_leave_api_sentinel;

/* Counter to track the number of PTH threads.  */
static int thread_counter;

/* Object used by update_fdarray.  */
struct fdarray_item_s 
{
  int fd;
  long netevents;
};


#ifdef HAVE_W32CE_SYSTEM
/* W32CE timer implementation.  */
static HANDLE w32ce_timer_ev;           /* Event to kick the timer thread.  */
static CRITICAL_SECTION w32ce_timer_cs; /* Protect the objects below.  */
static int w32ce_timer_thread_lauchend; /* True if the timer thread has
                                           been launched.  */
static struct               /* The timer array.  */
{
  HANDLE event;             /* Event to be signaled for this timer or
                               NULL for an unused slot.  */
  int active;               /* Timer is active.  */
  unsigned long remaining;  /* Remaining time in milliseconds. */
} w32ce_timer[32];
#endif /*HAVE_W32CE_SYSTEM*/


/* Pth events are store in a double linked event ring.  */
struct pth_event_s
{
  struct pth_event_s *next; 
  struct pth_event_s *prev;
  HANDLE hd;                  /* The event object.  Note that this is
                                 also used directly for the
                                 PTH_EVENT_HANDLE event.  */
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



/* Our own malloc function.  Eventually we will use HeapCreate to use
   a private heap here.  */
void *
_pth_malloc (size_t n)
{
  void *p;
  p = malloc (n);
  return p;
}

void *
_pth_calloc (size_t n, size_t m)
{
  void *p;
  p = calloc (n, m);
  return p;
}

void
_pth_free (void *p)
{
  if (p)
    free (p);
}


/* Return a string from the Win32 Registry or NULL in case of error.
   The returned string is allocated using a plain malloc and thus the
   caller needs to call the standard free().  The string is looked up
   under HKEY_LOCAL_MACHINE.  */
#ifdef HAVE_W32CE_SYSTEM
static char *
w32_read_registry (const wchar_t *dir, const wchar_t *name)
{
  HKEY handle;
  DWORD n, nbytes;
  wchar_t *buffer = NULL;
  char *result = NULL;
  
  if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, dir, 0, KEY_READ, &handle))
    {
      return NULL; /* No need for a RegClose, so return immediately. */
    }

  nbytes = 1;
  if (RegQueryValueEx (handle, name, 0, NULL, NULL, &nbytes))
    {
      goto leave;
    }
  buffer = malloc ((n=nbytes+2));
  if (!buffer)
    {
      goto leave;
    }
  if (RegQueryValueEx (handle, name, 0, NULL, (PBYTE)buffer, &n))
    {
      free (buffer);
      buffer = NULL;
      goto leave;
    }
  
  n = WideCharToMultiByte (CP_UTF8, 0, buffer, nbytes, NULL, 0, NULL, NULL);
  if (n < 0 || (n+1) <= 0)
    {
      goto leave;
    }
  result = malloc (n+1);
  if (!result)
    {
      goto leave;
    }
  n = WideCharToMultiByte (CP_UTF8, 0, buffer, nbytes, result, n, NULL, NULL);
  if (n < 0)
    {
      free (result);
      result = NULL;
      goto leave;
    }
  result[n] = 0;

 leave:
  free (buffer);
  RegCloseKey (handle);
  return result;
}
#endif /*HAVE_W32CE_SYSTEM*/

#ifdef HAVE_W32CE_SYSTEM
/* Replacement for getenv which takes care of the our use of getenv.
   The code is not thread safe but we expect it to work in all cases
   because it is called for the first time early enough.  */
static char *
getenv (const char *name)
{
  static int initialized;
  static char *val_debug;

  if (!initialized)
    {
      val_debug = w32_read_registry (L"\\Software\\GNU\\w32pth",
                                     L"debug");
      initialized = 1;
    }


  if (!strcmp (name, "PTH_DEBUG"))
    return val_debug;
  else
    return NULL;
}
#endif /*HAVE_W32CE_SYSTEM*/



static char *
w32_strerror (char *strerr, size_t strerrsize)
{
#ifdef HAVE_W32CE_SYSTEM
  /* There is only a wchar_t FormatMessage.  It does not make much
     sense to play the conversion game; we print only the code.  */
  snprintf (strerr, strerrsize, "ec=%d", (int)GetLastError ());
#else
  if (strerrsize > 1)
    FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL, (int)GetLastError (),
                   MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                   strerr, strerrsize, NULL);
#endif
  return strerr;
}

static char *
wsa_strerror (char *strerr, size_t strerrsize)
{
#ifdef HAVE_W32CE_SYSTEM
  snprintf (strerr, strerrsize, "ec=%d", (int)WSAGetLastError ());
#else
  if (strerrsize > 1)
    FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM, NULL, (int)WSAGetLastError (),
                   MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                   strerr, strerrsize, NULL);
#endif
  return strerr;
}


int
map_wsa_to_errno (long wsa_err)
{
  switch (wsa_err)
    {
    case 0:
      return 0;

    case WSAEINTR:
      return EINTR;

    case WSAEBADF:
      return EBADF;

    case WSAEACCES:
      return EACCES;

    case WSAEFAULT:
      return EFAULT;

    case WSAEINVAL:
      return EINVAL;

    case WSAEMFILE:
      return EMFILE;

    case WSAEWOULDBLOCK:
      return EAGAIN;

    case WSAENAMETOOLONG:
      return ENAMETOOLONG;

    case WSAENOTEMPTY:
      return ENOTEMPTY;

    default:
      return EIO;
    }
}


int
map_w32_to_errno (DWORD w32_err)
{
  switch (w32_err)
    {
    case 0:
      return 0;

    case ERROR_FILE_NOT_FOUND:
      return ENOENT;

    case ERROR_PATH_NOT_FOUND:
      return ENOENT;

    case ERROR_ACCESS_DENIED:
      return EPERM;

    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_BLOCK:
      return EINVAL;

    case ERROR_NOT_ENOUGH_MEMORY:
      return ENOMEM;
      
    case ERROR_NO_DATA:
      return EPIPE;

    default:
      return EIO;
    }
}


static int
fd_is_socket (int fd)
{
  int is_socket;
  int optval;
  int optlen;

  if (_pth_get_reader_ev (fd) != INVALID_HANDLE_VALUE
      || _pth_get_writer_ev (fd) != INVALID_HANDLE_VALUE)
    is_socket = 0;
  else
    {
      /* This implemenation strategy is taken from glib.
	 Unfortunately, it does not work with pipes, as getsockopt can
	 block on those.  So we test for pipes above first.  */
      optlen = sizeof (optval);
      is_socket = (getsockopt (fd, SOL_SOCKET, SO_TYPE,
			       (char *) &optval, &optlen) != SOCKET_ERROR);
    }

  if (DBG_INFO)
    _pth_debug (0, "fd_is_socket: fd %i is a %s.\n",
                fd, is_socket ? "socket" : "file");

  return is_socket;
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
        _pth_debug (0, "CreateEvent failed: %s\n",
                    w32_strerror (strerr, sizeof strerr));
      return NULL;
    }
#ifdef HAVE_W32CE_SYSTEM
  h2 = h;
#else
  if (!DuplicateHandle (GetCurrentProcess(), h,
                        GetCurrentProcess(), &h2,
                        EVENT_MODIFY_STATE|SYNCHRONIZE, FALSE, 0 ) ) 
    {
      if (DBG_ERROR)
        _pth_debug (0, "setting synchronize for event object %p failed: %s\n",
                 h, w32_strerror (strerr, sizeof strerr));
      CloseHandle (h);
      return NULL;
    }
  CloseHandle (h);
#endif
  if (DBG_INFO)
    {
      _pth_debug (0, "CreateEvent(%p) succeeded\n", h2);
    }
  return h2;
}


#ifdef TEST
static void
set_event (HANDLE h)
{
  char strerr[256];

  if (!SetEvent (h))
    {
      if (DBG_ERROR)
        _pth_debug (0, "SetEvent(%p) failed: %s\n",
                    h, w32_strerror (strerr, sizeof strerr));
    }
  else if (DBG_INFO)
    {
      _pth_debug (0, "SetEvent(%p) succeeded\n", h);
    }
}
#endif

static void
reset_event (HANDLE h)
{
  char strerr[256];

  if (!ResetEvent (h))
    {
      if (DBG_ERROR)
        _pth_debug (0, "ResetEvent(%p) failed: %s\n",
                    h, w32_strerror (strerr, sizeof strerr));
    }
  else if (DBG_INFO)
    {
      _pth_debug (0, "ResetEvent(%p) succeeded\n", h);
    }
}


#ifdef HAVE_W32CE_SYSTEM
/* WindowsCE does not provide CreateWaitableTimer et al.  Thus we have
   to roll our own.  The timer is not fully correct because we use
   GetTickCount and don't adjust for the time it takes to call it.  */
static DWORD CALLBACK 
w32ce_timer_thread (void *arg)
{
  int idx;
  DWORD timeout, elapsed, lasttick;

  (void)arg;

  lasttick = GetTickCount ();
  for (;;)
    {
      elapsed = lasttick;  /* Get start time.  */
      timeout = INFINITE;
      EnterCriticalSection (&w32ce_timer_cs);
      for (idx=0; idx < DIM (w32ce_timer); idx++)
        {
          if (w32ce_timer[idx].event && w32ce_timer[idx].active)
            {
              if (w32ce_timer[idx].remaining < timeout)
                timeout = w32ce_timer[idx].remaining;
            }
        }
      LeaveCriticalSection (&w32ce_timer_cs);
      if (timeout != INFINITE && timeout > 0x7fffffff)
        timeout = 0x7fffffff;
      switch (WaitForSingleObject (w32ce_timer_ev, (DWORD)timeout))
        {
        case WAIT_OBJECT_0:
        case WAIT_TIMEOUT:
          break;
        default:
          if (DBG_ERROR)
            _pth_debug (0, 
                     "w32ce_timer_thread: WFSO failed: rc=%d\n",
                     (int)GetLastError ());
          Sleep (500); /* Failsafe pause. */
          break;
        }
      EnterCriticalSection (&w32ce_timer_cs);
      lasttick = GetTickCount ();
      elapsed = lasttick - elapsed;
      for (idx=0; idx < DIM (w32ce_timer); idx++)
        {
          if (w32ce_timer[idx].event && w32ce_timer[idx].active)
            {
              if (w32ce_timer[idx].remaining > elapsed)
                w32ce_timer[idx].remaining -= elapsed;
              else
                {
                  w32ce_timer[idx].remaining = 0;
                  w32ce_timer[idx].active = 0;
                  if (!SetEvent (w32ce_timer[idx].event))
                    {
                      if (DBG_ERROR)
                        _pth_debug (0, "w32ce_timer_thread: SetEvent(%p) "
                                 "failed: rc=%d\n",
                                 (int)GetLastError ());
                    }
                }
            }
        }
      LeaveCriticalSection (&w32ce_timer_cs);
    }
  
  return 0; /*NOTREACHED*/
}
#endif /*HAVE_W32CE_SYSTEM*/


/* Create a timer event. */
static HANDLE
create_timer (void)
{
  HANDLE h;

#ifdef HAVE_W32CE_SYSTEM
  int idx;

  EnterCriticalSection (&w32ce_timer_cs);
  if (!w32ce_timer_thread_lauchend)
    {
      h = CreateThread (NULL, 0, w32ce_timer_thread, NULL, 0, NULL);
      if (!h)
        {
          if (DBG_ERROR)
            _pth_debug (0, "create_timer: CreateThread failed: rc=%d\n",
                        (int)GetLastError ());
          goto leave;
        }
      CloseHandle (h);
      w32ce_timer_thread_lauchend = 1;
    }
  h = NULL;
  for (idx = 0; idx < DIM (w32ce_timer); idx++)
    if (!w32ce_timer[idx].event)
      break;
  if (idx == DIM (w32ce_timer))
    {
      SetLastError (ERROR_TOO_MANY_OPEN_FILES);
      goto leave;
    }
  /* We create a manual reset event.  */
  w32ce_timer[idx].event = CreateEvent (NULL, TRUE, FALSE, NULL);
  if (!w32ce_timer[idx].event)
    goto leave;
  w32ce_timer[idx].active = 0;
  h = w32ce_timer[idx].event;

leave:
  LeaveCriticalSection (&w32ce_timer_cs);

#else /* Plain Windows.  */

  SECURITY_ATTRIBUTES sa;

  memset (&sa, 0, sizeof sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  sa.nLength = sizeof sa;
  h = CreateWaitableTimer (&sa, TRUE, NULL);

#endif /* Plain Windows.  */

  if (!h)
    {
      if (DBG_ERROR)
        _pth_debug (0, "CreateWaitableTimer failed: rc=%d\n",
                    (int)GetLastError ());
    }
  else if (DBG_INFO)
    {
      _pth_debug (0, "CreateWaitableTimer(%p) succeeded\n",
                  h);
    }
  return h;
}

static int
set_timer (HANDLE hd, DWORD milliseconds)
{
#ifdef HAVE_W32CE_SYSTEM
  int idx;
  int result = -1;

  EnterCriticalSection (&w32ce_timer_cs);
  for (idx = 0; idx < DIM (w32ce_timer); idx++)
    if (hd && w32ce_timer[idx].event == hd)
      {
        w32ce_timer[idx].remaining = milliseconds;
        if (!ResetEvent (w32ce_timer[idx].event))
          {
            if (DBG_ERROR)
              _pth_debug (0, "set_timer: ResetEvent(%p) failed: rc=%d\n",
                          (int)GetLastError ());
          }
        else if (!SetEvent (w32ce_timer_ev))
          {
            if (DBG_ERROR)
              _pth_debug (0, "set_timer: SetEvent(%p) failed: rc=%d\n",
                          (int)GetLastError ());
          }
        else
          {
            w32ce_timer[idx].active = 1;
            result = 0;
          }
        break;
      }
  if (idx == DIM (w32ce_timer))
    SetLastError (ERROR_INVALID_HANDLE);
  LeaveCriticalSection (&w32ce_timer_cs);
  return result;
#else /* Plain Windows.  */
  LARGE_INTEGER ll;
  char strerr[256];

  if (DBG_CALLS)
    _pth_debug (DEBUG_CALLS, "set_timer hd=%p ms=%lu\n", 
                hd, (unsigned long)milliseconds);
  
  ll.QuadPart = ((unsigned long)milliseconds * ((long long)-10000LL));
  if (!SetWaitableTimer (hd, &ll, 0, NULL, NULL, FALSE))
    {
      if (DBG_ERROR)
        _pth_debug (0,"%s: SetWaitableTimer failed: %s\n",
                    __func__,
                    w32_strerror (strerr, sizeof strerr));
      return -1;
    }
  return 0;
#endif /* Plain Windows.  */
}

static void
destroy_timer (HANDLE h)
{
#ifdef HAVE_W32CE_SYSTEM
  int idx;

  EnterCriticalSection (&w32ce_timer_cs);
  for (idx = 0; idx < DIM (w32ce_timer); idx++)
    if (w32ce_timer[idx].event == h)
      {
        CloseHandle (w32ce_timer[idx].event);
        w32ce_timer[idx].event = NULL;
        break;
      }
  LeaveCriticalSection (&w32ce_timer_cs);
#else /* Plain Windows.  */
  CloseHandle (h);
#endif /* Plain Windows.  */
}







int
pth_init (void)
{
  WSADATA wsadat;
  const char *s;

  if (pth_initialized)
    return TRUE;

  _pth_sema_subsystem_init ();

  s = getenv ("PTH_DEBUG");
  if (s && (debug_level = atoi (s)))
    {
#ifdef HAVE_W32CE_SYSTEM
      ActivateDevice (L"Drivers\\GnuPG_Log", 0);
      /* Ignore a filename and write the debug output to the GPG2:
         device.  */
      dbghd = CreateFile (L"GPG2:", GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#else
      const char *s1, *s2;
      char *p;

      s1 = strchr (s, ';');
      if (s1)
        {
          s1++;
          if (!(s2 = strchr (s1, ';')))
            s2 = s1 + strlen (s1);
          p = _pth_malloc (s2 - s1 + 1);
          if (p)
            {
              memcpy (p, s1, s2-s1);
              p[s2-s1] = 0;
              dbgfp = fopen (p, "a");
              if (dbgfp)
                setvbuf (dbgfp, NULL, _IOLBF, 0);
              _pth_free (p);
            }
        }
#endif
    }
#ifndef HAVE_W32CE_SYSTEM
  if (!dbgfp)
    dbgfp = stderr;
#endif
  if (debug_level)
    _pth_debug (DEBUG_ERROR, "pth_init called (level=%d)\n", debug_level);

  if (WSAStartup (0x202, &wsadat))
    return FALSE;
  pth_signo = 0;
  InitializeCriticalSection (&pth_shd);
  if (pth_signo_ev)
    CloseHandle (pth_signo_ev);

  pth_signo_ev = create_event ();
  if (!pth_signo_ev)
    return FALSE;

#ifdef HAVE_W32CE_SYSTEM
  InitializeCriticalSection (&w32ce_timer_cs);
  w32ce_timer_ev = CreateEvent (NULL, FALSE, FALSE, NULL);
  if (!w32ce_timer_ev)
    return FALSE;
#endif /*HAVE_W32CE_SYSTEM*/  


  pth_initialized = 1;
  thread_counter = 1;
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
    {
#ifdef HAVE_W32CE_SYSTEM
      DeleteCriticalSection (&w32ce_timer_cs);
#endif /*HAVE_W32CE_SYSTEM*/  
      DeleteCriticalSection (&pth_shd);
    }
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
    _pth_debug (DEBUG_CALLS, "enter_pth (%s)\n", function? function:"");
  LeaveCriticalSection (&pth_shd);
}


static void
leave_pth (const char *function)
{
  EnterCriticalSection (&pth_shd);
  if (DBG_CALLS)
    _pth_debug (DEBUG_CALLS, "leave_pth (%s)\n", function? function:"");
}


void
pth_enter (void)
{
  implicit_init ();
  if (enter_leave_api_sentinel)
    {
      fprintf (stderr, "pth_enter called while already in pth\n");
      abort ();
    }
  enter_leave_api_sentinel++;
  enter_pth (__FUNCTION__);
}


void
pth_leave (void)
{
  leave_pth (__FUNCTION__);
  if (enter_leave_api_sentinel != 1)
    {
      fprintf (stderr, "pth_leave was called while not in pth\n");
      abort ();
    }
  enter_leave_api_sentinel--;
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
      return -1;

    case PTH_CTRL_GETTHREADS_NEW:
      return 0; /* Not strictly correct.  */

    case PTH_CTRL_GETTHREADS_READY:
      return thread_counter? (thread_counter-1):0;

    case PTH_CTRL_GETTHREADS_RUNNING:
      return thread_counter? 1:0;

    case PTH_CTRL_GETTHREADS_WAITING:
      return -1; /* We don't have this info.  */

    case PTH_CTRL_GETTHREADS_SUSPENDED:
      return -1; /* We don't have this info.  */

    case PTH_CTRL_GETTHREADS_DEAD:
      return 0;

    case PTH_CTRL_GETTHREADS:
      return thread_counter;

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


/* Return true if HD refers to a socket.  */
static int
is_socket_2 (int hd)
{
#ifdef HAVE_W32CE_SYSTEM
  (void)hd;
  return 1; /* Assume socket.  */
#else
  /* We need to figure out whether we are working on a socket or on a
     handle.  A trivial way would be to check for the return code of
     recv and see if it is WSAENOTSOCK.  However the recv may block
     after the server process died and thus the destroy_reader will
     hang.  Another option is to use getsockopt to test whether it is
     a socket.  The bug here is that once a socket with a certain
     values has been opened, closed and later a CreatePipe returned
     the same value (i.e. handle), getsockopt still believes it is a
     socket.  What we do now is to use a combination of GetFileType
     and GetNamedPipeInfo.  The specs say that the latter may be used
     on anonymous pipes as well.  Note that there are claims that
     since winsocket version 2 ReadFile may be used on a socket but
     only if it is supported by the service provider.  Tests on a
     stock XP using a local TCP socket show that it does not work.  */
  DWORD dummyflags, dummyoutsize, dummyinsize, dummyinst;
  if (GetFileType ((HANDLE)hd) == FILE_TYPE_PIPE
      && !GetNamedPipeInfo ((HANDLE)hd, &dummyflags, &dummyoutsize,
                            &dummyinsize, &dummyinst))
    return 1; /* Function failed; thus we assume it is a socket.  */
  else
    return 0; /* Success; this is not a socket.  */
#endif
}


static int
pipe_is_not_connected (void)
{  
#ifdef HAVE_W32CE_SYSTEM
  switch (GetLastError ())
    {
    case ERROR_PIPE_NOT_CONNECTED:
      /* This may happen while one pipe end is still dangling
         because the child process has not yet completed the
         pipe creation.  */
    case ERROR_BUSY:
      /* Returned by the device manager? */
      Sleep (100);
      return 1;

    }
#endif
  return 0;
}


static int
do_pth_read (int fd,  void * buffer, size_t size)
{
  int n;
  HANDLE hd;
  int use_readfile = 0;

  TRACE_BEG (DEBUG_INFO, "do_pth_read", fd);

  /* We have to check for internal pipes first, as socket operations
     can block on these.  */
  hd = _pth_get_reader_ev (fd);
  TRACE_LOG1 ("  hd=%p", hd);
  if (hd != INVALID_HANDLE_VALUE)
    n = _pth_io_read (fd, buffer, size);
  else
    {
      if (is_socket_2 (fd))
        {
          TRACE_LOG1 ("  recv size=%d", (int)size);
          n = recv (fd, buffer, size, 0);
          TRACE_LOG1 ("  recv res=%d", n);
#ifdef HAVE_W32CE_SYSTEM          
          if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
            use_readfile = 1; /* Fallback to ReadFile.  */
#endif
        }
      else
        {
          TRACE_LOG1 ("  use readfile size=%d", (int)size);
          n = -1;
          use_readfile = 1;
        }
      if (n == -1 && use_readfile)
	{
	  DWORD nread = 0;

          do
            {

              TRACE_LOG2 ("  ReadFile on %p size=%d", (HANDLE)fd, (int)size);
              n = ReadFile ((HANDLE)fd, buffer, size, &nread, NULL);
              TRACE_LOG2 ("           n=%d nread=%d", n, (int)nread);
            }
          while (!n && pipe_is_not_connected ());
	  if (!n)
	    {
	      char strerr[256];
	      
	      if (DBG_ERROR)
		_pth_debug (0, "pth_read(0x%x) ReadFile failed: %s\n",
			 fd, w32_strerror (strerr, sizeof strerr));
	      n = -1;
	      set_errno (map_w32_to_errno (GetLastError ()));
	    }
	  else
	    n = (int) nread;
	}
      else if (n == -1)
        {
          if (DBG_ERROR)
            _pth_debug (0, "pth_read(0x%x) recv failed: ec=%d\n",
                        fd, (int)WSAGetLastError ());
          set_errno (map_wsa_to_errno (WSAGetLastError ()));
        }
    }

  TRACE_SYSRES (n);
  return n;
}


int
pth_read_ev (int fd, void *buffer, size_t size, pth_event_t ev_extra)
{
  static pth_key_t ev_key = PTH_KEY_INIT;
  pth_event_t ev;
  int n = 0;

  implicit_init ();
  enter_pth (__FUNCTION__);

  /* FIXME: Consider fdmode, and other stuff (see GNU pth).  */

  ev = do_pth_event (PTH_EVENT_FD | PTH_UNTIL_FD_READABLE | PTH_MODE_STATIC,
		     &ev_key, fd);
  if (! ev)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }

  if (ev_extra)
    pth_event_concat (ev, ev_extra, NULL);

  do_pth_wait (ev);

  if (ev_extra)
    {
      pth_event_isolate (ev);
      if (ev->status != PTH_STATUS_OCCURRED)
	{
#ifdef NO_PTH_MODE_STATIC
	  do_pth_event_free (ev, PTH_FREE_THIS);
#endif
	  set_errno (EINTR);
	  leave_pth (__FUNCTION__);
	  return -1;
	}
    }
#ifdef NO_PTH_MODE_STATIC
  do_pth_event_free (ev, PTH_FREE_THIS);
#endif

  n = do_pth_read (fd, buffer, size);

  leave_pth (__FUNCTION__);
  return n;
}


int
pth_read (int fd,  void * buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);

  n = do_pth_read (fd, buffer, size);

  leave_pth (__FUNCTION__);
  return n;
}


static int
do_pth_write (int fd, const void *buffer, size_t size)
{
  int n;
  HANDLE hd;
  int use_writefile = 0;

  TRACE_BEG (DEBUG_INFO, "do_pth_write", fd);

  /* We have to check for internal pipes first, as socket operations
     can block on these.  */
  hd = _pth_get_writer_ev (fd);
  TRACE_LOG1 ("  hd=%p", hd);
  if (hd != INVALID_HANDLE_VALUE)
    n = _pth_io_write (fd, buffer, size);
  else
    {
      if (is_socket_2 (fd))
        {
          TRACE_LOG1 ("  send size=%d", (int)size);
          n = send (fd, buffer, size, 0);
          TRACE_LOG1 ("  send res=%d", n);
#ifdef HAVE_W32CE_SYSTEM          
          if (n == -1 && WSAGetLastError () == WSAENOTSOCK)
            use_writefile = 1; /* Fallback to ReadFile.  */
#endif
        }
      else
        {
          TRACE_LOG1 ("  use writefile size=%d", (int)size);
          n = -1;
          use_writefile = 1;
        }
      if (n == -1 && use_writefile)
	{
	  DWORD nwrite;
	  char strerr[256];
	  
	  /* This is no real error because we first need to figure out
	     if we have a handle or a socket.  */
          TRACE_LOG2 ("  WriteFile on %p size=%d", (HANDLE)fd, (int)size);
	  if (!WriteFile ((HANDLE)fd, buffer, size, &nwrite, NULL))
	    {
	      n = -1;
	      set_errno (map_w32_to_errno (GetLastError ()));
	      if (DBG_ERROR)
		_pth_debug (0, "pth_write(0x%x) failed in write: %s\n",
                            fd, w32_strerror (strerr, sizeof strerr));
	    }
	  else
            {
              TRACE_LOG2 ("           n=%d nwritten=%d", n, (int)nwrite);
              n = (int) nwrite;
            }
	}
      else if (n == -1)
        {
          if (DBG_ERROR)
            _pth_debug (0, "pth_write(0x%x) send failed: ec=%d\n",
                        fd, (int)WSAGetLastError ());
          set_errno (map_wsa_to_errno (WSAGetLastError ()));
        }
    }

  TRACE_SYSRES (n);
  return n;
}


int
pth_write_ev (int fd, const void *buffer, size_t size, pth_event_t ev_extra)
{
  static pth_key_t ev_key = PTH_KEY_INIT;
  pth_event_t ev;
  int n = 0;

  implicit_init ();
  enter_pth (__FUNCTION__);

  /* FIXME: Consider fdmode, and other stuff (see GNU pth).  */

  ev = do_pth_event (PTH_EVENT_FD | PTH_UNTIL_FD_WRITEABLE | PTH_MODE_STATIC,
		     &ev_key, fd);
  if (! ev)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }

  if (ev_extra)
    pth_event_concat (ev, ev_extra, NULL);

  do_pth_wait (ev);
  if (ev_extra)
    {
      pth_event_isolate (ev);

      if (pth_event_status(ev) != PTH_STATUS_OCCURRED)
	{
#ifdef NO_PTH_MODE_STATIC
          do_pth_event_free (ev, PTH_FREE_THIS);
#endif
          set_errno (EINTR);
	  leave_pth (__FUNCTION__);
	  return -1;
	}
    }
#ifdef NO_PTH_MODE_STATIC
  do_pth_event_free (ev, PTH_FREE_THIS);
#endif

  n = do_pth_write (fd, buffer, size);

  leave_pth (__FUNCTION__);
  return n;
}


int
pth_write (int fd, const void *buffer, size_t size)
{
  int n;

  implicit_init ();
  enter_pth (__FUNCTION__);

  n = do_pth_write (fd, buffer, size);

  leave_pth (__FUNCTION__);
  return n;
}


static void
show_event_ring (const char *text, pth_event_t ev)
{
  pth_event_t r;

  if (!ev)
    {
      _pth_debug (0, "show_event_ring(%s):  No ring\n", text);
      return;
    }

  r = ev;
  do
    {
      _pth_debug (0, "show_event_ring(%s): type=%d r=%p prev=%p next=%p\n",
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
      set_errno (EINTR);
    }

 leave:
  do_pth_event_free (ev, PTH_FREE_THIS);
  do_pth_event_free (ev_time, PTH_FREE_THIS);

  leave_pth (__FUNCTION__);
  return rc;
}


int
pth_select (int nfd, fd_set * rfds, fd_set * wfds, fd_set * efds,
	    const struct timeval * timeout)
{
  return pth_select_ev (nfd, rfds, wfds, efds, timeout, NULL);
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
#ifdef NO_PTH_MODE_STATIC
	      do_pth_event_free (ev, PTH_FREE_THIS);
#endif
              pth_fdmode (fd, fdmode);
              leave_pth (__FUNCTION__);
              return -1;
            }
        }
    }
#ifdef NO_PTH_MODE_STATIC
  if (ev)
    do_pth_event_free (ev, PTH_FREE_THIS);
#endif

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
        _pth_debug (0, "pth_release_mutex %p failed: %s\n",
                    *mutex, w32_strerror (strerr, sizeof strerr));
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
            _pth_debug (0, "pth_mutex_acquire for %p failed: %s\n",
                        *mutex, w32_strerror (strerr, sizeof strerr));
        }
        rc = FALSE;
        break;
        
      case WAIT_OBJECT_0:
        rc = TRUE;
        break;

      default:
        if (DBG_ERROR)
          _pth_debug (0, "WaitForSingleObject returned unexpected "
                      "code %d for mutex %p\n",
                      code, *mutex);
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


/* Destroy the mutex MUTEX.  */
int
pth_mutex_destroy (pth_mutex_t *mutex)
{
  implicit_init ();
  enter_pth (__FUNCTION__);

  CloseHandle (*mutex);

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
  hd = _pth_calloc (1, sizeof *hd);
  return hd;
}


int
pth_attr_destroy (pth_attr_t hd)
{
  if (!hd)
    return -1;
  implicit_init ();
  if (hd->name)
    _pth_free (hd->name);
  _pth_free (hd);
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
            _pth_debug (0, "pth_attr_set: PTH_ATTR_JOINABLE\n");
        }
      break;

    case PTH_ATTR_STACK_SIZE:
      val = va_arg (args, int);
      if (val)
        {
          hd->flags |= PTH_ATTR_STACK_SIZE;
          hd->stack_size = val;
          if (DBG_INFO)
            _pth_debug (0, "pth_attr_set: PTH_ATTR_STACK_SIZE %d\n",
                        val);
        }
      break;

    case PTH_ATTR_NAME:
      str = va_arg (args, char*);
      if (hd->name)
        _pth_free (hd->name);
      if (str)
        {
          hd->name = strdup (str);
          if (!hd->name)
            return FALSE;
          hd->flags |= PTH_ATTR_NAME;
          if (DBG_INFO)
            _pth_debug (0, "pth_attr_set: PTH_ATTR_NAME %s\n",
                        hd->name);
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

  ctx = _pth_calloc (1, sizeof *ctx);
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
    _pth_debug (0, "do_pth_spawn creating thread ...\n");
  th = CreateThread (&sa, hd->stack_size,
                     (LPTHREAD_START_ROUTINE)launch_thread,
                     ctx, CREATE_SUSPENDED, &tid);
  ctx->th = th;
  if (DBG_INFO)
    _pth_debug (0, "do_pth_spawn created thread %p\n", th);
  if (!th)
    _pth_free (ctx);
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


/* Special W32 function to cope with the problem that pth_self returns
   just a pseudo handle which is not very useful for debugging.  */
unsigned long 
pth_thread_id (void)
{
  return GetCurrentThreadId ();
}


int
pth_join (pth_t hd, void **value)
{
#warning fixme: We need to implement this.
  return TRUE;
}


/* friendly */
int
pth_cancel (pth_t hd)
{
  int ok = 0;

  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  WaitForSingleObject (hd, 1000);
  if (TerminateThread (hd, 0))
    ok = 1;
  leave_pth (__FUNCTION__);
  if (ok)
    thread_counter--;
  return TRUE;
}


/* cruel */
int
pth_abort (pth_t hd)
{
  int ok = 0;

  if (!hd)
    return -1;
  implicit_init ();
  enter_pth (__FUNCTION__);
  if (TerminateThread (hd, 0))
    ok = 1;
  leave_pth (__FUNCTION__);
  if (ok)
    thread_counter--;
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
#ifdef NO_PTH_MODE_STATIC
      do_pth_event_free (ev, PTH_FREE_THIS);
#endif
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


#if 0 /* Not yet used.  */
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
    _pth_debug (0, "sig_handler=%d\n", pth_signo);
  return TRUE;
}
#endif


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
        _pth_debug (0, "pth_event spec=%lx - not supported\n", 
                    spec);
      return NULL; /* Not supported.  */
    }

  if (DBG_INFO)
    _pth_debug (0, "pth_event spec=%lx\n", spec);

  ev = _pth_calloc (1, sizeof *ev);
  if (!ev)
    return NULL;
  ev->next = ev;
  ev->prev = ev;
  if ( !(spec & PTH_EVENT_HANDLE) )
    {
      if ((spec & PTH_EVENT_TIME))
        ev->hd = create_timer ();
      else
        ev->hd = create_event ();
      if (!ev->hd)
        {
          _pth_free (ev);
          return NULL;
        }
    }

  /* We don't support static yet but we need to consume the
     argument.  */
  if ((spec & PTH_MODE_STATIC))
    {
      ev->flags |= PTH_MODE_STATIC;
      va_arg (arg, pth_key_t *);
    }

  ev->status = PTH_STATUS_PENDING;

  if (spec == 0)
    ;
  else if (spec & PTH_EVENT_HANDLE)
    {
      ev->u_type = PTH_EVENT_HANDLE;
      ev->hd = va_arg (arg, void *);
    }
  else if (spec & PTH_EVENT_SIGS)
    {
      ev->u_type = PTH_EVENT_SIGS;
      ev->u.sig.set = va_arg (arg, struct sigset_s *);
      ev->u.sig.signo = va_arg (arg, int *);	
      /* The signal handler is disabled for now.  */
      rc = 0/*SetConsoleCtrlHandler (sig_handler, TRUE)*/;
      if (DBG_INFO)
        _pth_debug (0, "pth_event: sigs rc=%d\n", rc);
    }
  else if (spec & PTH_EVENT_FD)
    {
      if (spec & PTH_UNTIL_FD_READABLE)
        ev->flags |= PTH_UNTIL_FD_READABLE;
      ev->u_type = PTH_EVENT_FD;
      ev->u.fd = va_arg (arg, int);
      if (DBG_INFO)
        _pth_debug (0, "pth_event: fd=0x%x\n", ev->u.fd);
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
                _pth_debug (0, 
                            "pth_event: WSAEventSelect(%d[%d]) failed: %s\n",
                            i, fdarray[i].fd,
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


static void *
launch_thread (void *arg)
{
  struct thread_info_s *c = arg;

  if (c)
    {
      leave_pth (__FUNCTION__);

      thread_counter++;
      c->thread (c->arg);
      if (!c->joinable && c->th)
        {
          CloseHandle (c->th);
          c->th = NULL;
        }
      thread_counter--;

      /* FIXME: We would badly fail if someone accesses the now
         deallocated handle. Don't use it directly but setup proper
         scheduling queues.  */
      enter_pth (__FUNCTION__);
      _pth_free (c);
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
          if (cur->u_type == PTH_EVENT_TIME)
            destroy_timer (cur->hd);
          else if (cur->u_type != PTH_EVENT_HANDLE)
            CloseHandle (cur->hd);
          cur->hd = NULL;
          _pth_free (cur);
          cur = next;
        }
      while (cur != ev);
    }
  else if (mode == PTH_FREE_THIS)
    {
      ev->prev->next = ev->next;
      ev->next->prev = ev->prev;
      if (ev->u_type == PTH_EVENT_TIME)
        destroy_timer (ev->hd);
      else if (ev->u_type != PTH_EVENT_HANDLE)
        CloseHandle (ev->hd);
      ev->hd = NULL;	    
      _pth_free (ev);
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



static int
do_pth_wait (pth_event_t ev)
{
  char strerr[256];
  HANDLE waitbuf[MAXIMUM_WAIT_OBJECTS/2];
  pth_event_t evarray[MAXIMUM_WAIT_OBJECTS/2];
  DWORD n;
  int pos, idx, thlstidx, i;
  pth_event_t r;
  int count;

  TRACE_BEG (DEBUG_INFO, "do_pth_wait", ev);

  if (!ev)
    return TRACE_SYSRES (0);

  n = event_count (ev);
  if (n > MAXIMUM_WAIT_OBJECTS/2)
    return TRACE_SYSRES (-1);

  TRACE_LOG1 ("cnt %lu", n);

  /* Set all events to pending.  */
  r = ev;
  do 
    {
      r->status = PTH_STATUS_PENDING;
      r = r->next;
    }
  while (r != ev);

  /* Prepare all events which requires to launch helper threads for
     some types.  This creates an array of handles which are later
     passed to WFMO. */
  pos = thlstidx = 0;
  r = ev;
  do
    {
      switch (r->u_type)
        {
        case PTH_EVENT_SIGS:
          TRACE_LOG ("add signal event");
          /* Register the global signal event.  */
          evarray[pos] = r;  
          waitbuf[pos++] = pth_signo_ev;
          break;
          
        case PTH_EVENT_FD:
	  {
	    int res;
	    int fd = r->u.fd;
	    /* FIXME: Could be optimised a bit, as we call
	       _pth_get_reader_ev twice in the reader case.  */
	    int is_socket = fd_is_socket (fd);

	    if (is_socket)
	      {
		WSAEVENT sockevent = WSACreateEvent ();
		long flags;

		/* Note: This restricts us to one event in one active
		   wait per socket.  But that's commonly the case
		   anyway.  */
		if (r->flags & PTH_UNTIL_FD_READABLE)
		  flags = FD_READ | FD_ACCEPT;
		else
		  flags = FD_WRITE;

		res = WSAEventSelect (fd, sockevent, flags);
		if (res)
		  {
		    if (DBG_ERROR)
		      _pth_debug (0, "can't set event for FD 0x%x "
                                  "(ignored)\n", fd);
		  }
		else
		  {
		    TRACE_LOG2 ("socket event for FD 0x%x is %p", fd, sockevent);
		    evarray[pos] = r;
		    waitbuf[pos++] = sockevent;
		  }
	      }
	    else
	      {

		if (r->flags & PTH_UNTIL_FD_READABLE)
		  {
		    HANDLE reader_ev = _pth_get_reader_ev (fd);

		    if (reader_ev == INVALID_HANDLE_VALUE)
		      {
			if (DBG_ERROR)
			  _pth_debug (0, "no reader for FD 0x%x "
                                      "(ignored)\n", fd);
		      }
		    else
		      {
			TRACE_LOG2 ("reader for FD 0x%x is %p", fd, reader_ev);
			evarray[pos] = r;
			waitbuf[pos++] = reader_ev;
		      }
		  }
		else
		  {
		    HANDLE writer_ev = _pth_get_writer_ev (fd);
		    
		    if (writer_ev == INVALID_HANDLE_VALUE)
		      {
			if (DBG_ERROR)
			  _pth_debug (0, "no writer for FD 0x%x "
                                      "(ignored)\n", fd);
		      }
		    else
		      {
			TRACE_LOG2 ("writer for FD 0x%x is %p", fd, writer_ev);
			evarray[pos] = r;  
			waitbuf[pos++] = writer_ev;
		      }
		  }
	      }
	  }
          break;
          
        case PTH_EVENT_TIME:
          TRACE_LOG ("adding timer event");
          if (set_timer (r->hd, (r->u.tv.tv_sec * 1000
                                 + (r->u.tv.tv_usec+500) / 1000 )))
            return TRACE_SYSRES (-1);
          evarray[pos] = r;  
          waitbuf[pos++] = r->hd;
          break;

        case PTH_EVENT_SELECT:
          TRACE_LOG ("adding select event");
          evarray[pos] = r;  
          waitbuf[pos++] = r->hd;
          break;

        case PTH_EVENT_HANDLE:
          TRACE_LOG ("adding handle event");
          evarray[pos] = r;  
          waitbuf[pos++] = r->hd;
          break;

        case PTH_EVENT_MUTEX:
          if (DBG_ERROR)
            _pth_debug (0, "pth_wait: ignoring mutex event.\n");
          break;

	default:
          if (DBG_ERROR)
            _pth_debug (0, "pth_wait: unhandled event type 0x%x.\n",
		     r->u_type);
	  break;
        }
      r = r->next;
    }
  while (r != ev);

  TRACE_LOG ("dump list");
  if (_pth_debug_trace ())
    {
      TRACE_LOG1 ("WFMO n=%d", pos);
      for (i = 0; i < pos; i++)
        TRACE_LOG2 ("      %d=%p", i, waitbuf[i]);
    }
  TRACE_LOG ("now wait");
  n = WaitForMultipleObjects (pos, waitbuf, FALSE, INFINITE);
  TRACE_LOG1 ("WFMO returned %ld", n);
  count = 0;

  /* Walk over all events with an assigned handle and update the
     status.  Note: This may override the return value of WFMO.  */
  for (idx = 0; idx < pos; idx++)
    {
      r = evarray[idx];
      
      if (WaitForSingleObject (waitbuf[idx], 0) == WAIT_OBJECT_0)
	{
	  TRACE_LOG2 ("setting %d ev=%p", idx, r);
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
		nfdarray = build_fdarray (fdarray, nfdarray, r->u.sel.rfds, 0);
		nfdarray = build_fdarray (fdarray, nfdarray, r->u.sel.wfds, 0);
		nfdarray = build_fdarray (fdarray, nfdarray, r->u.sel.efds, 0);
		
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
			  _pth_debug (0, 
                                      "pth_wait: WSAEnumNetworkEvents(%d[%d])"
                                      " failed: %s\n",
                                      i, fdarray[i].fd,
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
		    /* Fixme: Do this only if the socket was in
		       blocking mode.  */
		    if (WSAEventSelect (fdarray[i].fd, NULL, 0))
		      {
			if (DBG_ERROR)
			  _pth_debug (0, 
				   "pth_wait: WSAEventSelect(%d[%d]-clear)"
				   " failed: %s\n",
				   i, fdarray[i].fd,
				   wsa_strerror (strerr, sizeof strerr));
		      }
		    
		    val = 0;
		    if (ioctlsocket (fdarray[i].fd, FIONBIO, &val)
			== SOCKET_ERROR)
		      {
			if (DBG_ERROR)
			  _pth_debug (0, 
				   "pth_wait: ioctlsocket(%d[%d])"
				   " failed: %s\n",
				   i, fdarray[i].fd,
				   wsa_strerror (strerr, sizeof strerr));
		      }
		  }
		*r->u.sel.rc = ntotal;
	      }
	      break;
	    }

	  /* We don't reset Timer events and I don't know whether
	     resetEvent will work at all.  SetWaitableTimer resets the
	     timer.  FIXME.  Note by MB: Resetting the event here
	     seems wrong in most (all?)  cases, as the event is still
	     "hot" for all we know: A second pth_wait with the same
	     events should return with the same results as the
	     previous one immediatetly.  For example, data on a socket
	     or pipe is still readable after.  Reset should happen in
	     pth_read/pth_write in this case, but these functions need
	     to do a quick poll as well.  Consider for example a
	     pth_read_ev where multiple events occur.  See w32-io.c
	     how this works for pipes.  FIXME: Frankly, this is a
	     mess.  For example, make sure the below is fine with the
	     global signal event.  Note: This is related to
	     edge-triggered vs level-triggered.  Level triggered is
	     doubleplusgood.  */
	  if (r->u_type != PTH_EVENT_TIME && r->u_type != PTH_EVENT_FD)
	    reset_event (waitbuf[idx]);
	}

      /* Clean up allocated resources in any case.  */
      switch (r->u_type)
	{
	case PTH_EVENT_FD:
	  {
	    int fd = r->u.fd;
	    int is_socket = fd_is_socket (fd);
	    
	    if (is_socket)
	      {
		WSAEventSelect (fd, NULL, 0);
		WSACloseEvent (waitbuf[idx]);
		waitbuf[idx] = NULL;
	      }
	    /* Nothing to be done for pipes.  */
	  }
	  break;
	}
    }

  if (count)
    return TRACE_SYSRES (count);
  else if (n == WAIT_TIMEOUT)
    return TRACE_SYSRES (0);
  else
    return TRACE_SYSRES (-1);
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
#ifdef NO_PTH_MODE_STATIC
  do_pth_event_free (ev, PTH_FREE_THIS);
#endif

  leave_pth (__FUNCTION__);
  return 0;
}


int
pth_usleep (unsigned int usec)
{
  static pth_key_t ev_key = PTH_KEY_INIT;
  pth_event_t ev;

  implicit_init ();
  enter_pth (__FUNCTION__);

  if (usec == 0)
    {
      leave_pth (__FUNCTION__);
      return 0;
    }

  ev = do_pth_event (PTH_EVENT_TIME | PTH_MODE_STATIC, &ev_key,
                     pth_timeout (0, usec));
  if (ev == NULL)
    {
      leave_pth (__FUNCTION__);
      return -1;
    }
  do_pth_wait (ev);
#ifdef NO_PTH_MODE_STATIC
  do_pth_event_free (ev, PTH_FREE_THIS);
#endif

  leave_pth (__FUNCTION__);
  return 0;
}


int
pth_sigmask (int how, const sigset_t *set, sigset_t *old)
{

  return 0;
}


int
pth_yield (pth_t tid)
{
  implicit_init ();
  enter_pth (__FUNCTION__);
  Sleep (0);
  leave_pth (__FUNCTION__);
  return TRUE;
}


/* 
   Some simple tests.  
 */
#ifdef TEST
#include <stdio.h>

void *
thread (void * c)
{

  Sleep (2000);
  set_event (((pth_event_t)c)->hd);
  fprintf (stderr, "\n\nhallo!.\n");
  pth_exit (NULL);
  return NULL;
}


int
main_1 (int argc, char ** argv)
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


#ifndef HAVE_W32CE_SYSTEM
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
#endif

int
main (int argc, char ** argv)
{
  pth_init ();
  pth_sleep (5);
  pth_kill ();
  return 0;
}
#endif

