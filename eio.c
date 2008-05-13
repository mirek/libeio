#include "eio.h"
#include "xthread.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <assert.h>

#ifndef EIO_FINISH
# define EIO_FINISH(req)  ((req)->finish) && !EIO_CANCELLED (req) ? (req)->finish (req) : 0
#endif

#ifndef EIO_DESTROY
# define EIO_DESTROY(req) do { if ((req)->destroy) (req)->destroy (req); } while (0)
#endif

#ifndef EIO_FEED
# define EIO_FEED(req)    do { if ((req)->feed   ) (req)->feed    (req); } while (0)
#endif

#ifdef _WIN32

  /*doh*/

#else

# include "config.h"
# include <sys/time.h>
# include <sys/select.h>
# include <unistd.h>
# include <utime.h>
# include <signal.h>
# include <dirent.h>

# ifndef EIO_STRUCT_DIRENT
#  define EIO_STRUCT_DIRENT struct dirent
# endif

#endif

#if HAVE_SENDFILE
# if __linux
#  include <sys/sendfile.h>
# elif __freebsd
#  include <sys/socket.h>
#  include <sys/uio.h>
# elif __hpux
#  include <sys/socket.h>
# elif __solaris /* not yet */
#  include <sys/sendfile.h>
# else
#  error sendfile support requested but not available
# endif
#endif

/* number of seconds after which an idle threads exit */
#define IDLE_TIMEOUT 10

/* used for struct dirent, AIX doesn't provide it */
#ifndef NAME_MAX
# define NAME_MAX 4096
#endif

/* buffer size for various temporary buffers */
#define EIO_BUFSIZE 65536

#define dBUF	 				\
  char *eio_buf;				\
  X_LOCK (wrklock);				\
  self->dbuf = eio_buf = malloc (EIO_BUFSIZE);	\
  X_UNLOCK (wrklock);				\
  errno = ENOMEM;				\
  if (!eio_buf)					\
    return -1;

#define EIO_TICKS ((1000000 + 1023) >> 10)

static void (*want_poll_cb) (void);
static void (*done_poll_cb) (void);

static unsigned int max_poll_time = 0;
static unsigned int max_poll_reqs = 0;

/* calculcate time difference in ~1/EIO_TICKS of a second */
static int tvdiff (struct timeval *tv1, struct timeval *tv2)
{
  return  (tv2->tv_sec  - tv1->tv_sec ) * EIO_TICKS
       + ((tv2->tv_usec - tv1->tv_usec) >> 10);
}

static unsigned int started, idle, wanted = 4;

/* worker threads management */
static mutex_t wrklock = X_MUTEX_INIT;

typedef struct worker
{
  /* locked by wrklock */
  struct worker *prev, *next;

  thread_t tid;

  /* locked by reslock, reqlock or wrklock */
  eio_req *req; /* currently processed request */
  void *dbuf;
  DIR *dirp;
} worker;

static worker wrk_first = { &wrk_first, &wrk_first, 0 };

static void worker_clear (worker *wrk)
{
  if (wrk->dirp)
    {
      closedir (wrk->dirp);
      wrk->dirp = 0;
    }

  if (wrk->dbuf)
    {
      free (wrk->dbuf);
      wrk->dbuf = 0;
    }
}

static void worker_free (worker *wrk)
{
  wrk->next->prev = wrk->prev;
  wrk->prev->next = wrk->next;

  free (wrk);
}

static volatile unsigned int nreqs, nready, npending;
static volatile unsigned int max_idle = 4;

static mutex_t reslock = X_MUTEX_INIT;
static mutex_t reqlock = X_MUTEX_INIT;
static cond_t  reqwait = X_COND_INIT;

unsigned int eio_nreqs (void)
{
  return nreqs;
}

unsigned int eio_nready (void)
{
  unsigned int retval;

  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  retval = nready;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);

  return retval;
}

unsigned int eio_npending (void)
{
  unsigned int retval;

  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  retval = npending;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);

  return retval;
}

unsigned int eio_nthreads (void)
{
  unsigned int retval;

  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  retval = started;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);

  return retval;
}

/*
 * a somewhat faster data structure might be nice, but
 * with 8 priorities this actually needs <20 insns
 * per shift, the most expensive operation.
 */
typedef struct {
  eio_req *qs[EIO_NUM_PRI], *qe[EIO_NUM_PRI]; /* qstart, qend */
  int size;
} reqq;

static reqq req_queue;
static reqq res_queue;

static int reqq_push (reqq *q, eio_req *req)
{
  int pri = req->pri;
  req->next = 0;

  if (q->qe[pri])
    {
      q->qe[pri]->next = req;
      q->qe[pri] = req;
    }
  else
    q->qe[pri] = q->qs[pri] = req;

  return q->size++;
}

static eio_req *reqq_shift (reqq *q)
{
  int pri;

  if (!q->size)
    return 0;

  --q->size;

  for (pri = EIO_NUM_PRI; pri--; )
    {
      eio_req *req = q->qs[pri];

      if (req)
        {
          if (!(q->qs[pri] = (eio_req *)req->next))
            q->qe[pri] = 0;

          return req;
        }
    }

  abort ();
}

static void grp_try_feed (eio_req *grp)
{
  while (grp->size < grp->int2 && !EIO_CANCELLED (grp))
    {
      int old_len = grp->size;

      EIO_FEED (grp);

      /* stop if no progress has been made */
      if (old_len == grp->size)
        {
          grp->feed = 0;
          break;
        }
    }
}

static int eio_finish (eio_req *req);

static int grp_dec (eio_req *grp)
{
  --grp->size;

  /* call feeder, if applicable */
  grp_try_feed (grp);

  /* finish, if done */
  if (!grp->size && grp->int1)
    return eio_finish (grp);
  else
    return 0;
}

void eio_destroy (eio_req *req)
{
  if ((req)->flags & EIO_FLAG_PTR1_FREE) free (req->ptr1);
  if ((req)->flags & EIO_FLAG_PTR2_FREE) free (req->ptr2);

  EIO_DESTROY (req);
}

static int eio_finish (eio_req *req)
{
  int res = EIO_FINISH (req);

  if (req->grp)
    {
      int res2;
      eio_req *grp = req->grp;

      /* unlink request */
      if (req->grp_next) req->grp_next->grp_prev = req->grp_prev;
      if (req->grp_prev) req->grp_prev->grp_next = req->grp_next;

      if (grp->grp_first == req)
        grp->grp_first = req->grp_next;

      res2 = grp_dec (grp);

      if (!res && res2)
        res = res2;
    }

  eio_destroy (req);

  return res;
}

void eio_grp_cancel (eio_req *grp)
{
  for (grp = grp->grp_first; grp; grp = grp->grp_next)
    eio_cancel (grp);
}

void eio_cancel (eio_req *req)
{
  X_LOCK   (wrklock);
  req->flags |= EIO_FLAG_CANCELLED;
  X_UNLOCK (wrklock);

  eio_grp_cancel (req);
}

X_THREAD_PROC (eio_proc);

static void start_thread (void)
{
  worker *wrk = calloc (1, sizeof (worker));

  /*TODO*/
  assert (("unable to allocate worker thread data", wrk));

  X_LOCK (wrklock);

  if (thread_create (&wrk->tid, eio_proc, (void *)wrk))
    {
      wrk->prev = &wrk_first;
      wrk->next = wrk_first.next;
      wrk_first.next->prev = wrk;
      wrk_first.next = wrk;
      ++started;
    }
  else
    free (wrk);

  X_UNLOCK (wrklock);
}

static void maybe_start_thread (void)
{
  if (eio_nthreads () >= wanted)
    return;
  
  /* todo: maybe use idle here, but might be less exact */
  if (0 <= (int)eio_nthreads () + (int)eio_npending () - (int)eio_nreqs ())
    return;

  start_thread ();
}

void eio_submit (eio_req *req)
{
  req->pri += EIO_PRI_BIAS;

  if (req->pri < EIO_PRI_MIN + EIO_PRI_BIAS) req->pri = EIO_PRI_MIN + EIO_PRI_BIAS;
  if (req->pri > EIO_PRI_MAX + EIO_PRI_BIAS) req->pri = EIO_PRI_MAX + EIO_PRI_BIAS;

  ++nreqs;

  X_LOCK (reqlock);
  ++nready;
  reqq_push (&req_queue, req);
  X_COND_SIGNAL (reqwait);
  X_UNLOCK (reqlock);

  maybe_start_thread ();
}

static void end_thread (void)
{
  eio_req *req = calloc (1, sizeof (eio_req));

  req->type = EIO_QUIT;
  req->pri  = EIO_PRI_MAX + EIO_PRI_BIAS;

  X_LOCK (reqlock);
  reqq_push (&req_queue, req);
  X_COND_SIGNAL (reqwait);
  X_UNLOCK (reqlock);

  X_LOCK (wrklock);
  --started;
  X_UNLOCK (wrklock);
}

void eio_set_max_poll_time (double nseconds)
{
  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  max_poll_time = nseconds;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);
}

void eio_set_max_poll_reqs (unsigned int maxreqs)
{
  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  max_poll_reqs = maxreqs;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);
}

void eio_set_max_idle (unsigned int nthreads)
{
  if (WORDACCESS_UNSAFE) X_LOCK   (reqlock);
  max_idle = nthreads <= 0 ? 1 : nthreads;
  if (WORDACCESS_UNSAFE) X_UNLOCK (reqlock);
}

void eio_set_min_parallel (unsigned int nthreads)
{
  if (wanted < nthreads)
    wanted = nthreads;
}

void eio_set_max_parallel (unsigned int nthreads)
{
  if (wanted > nthreads)
    wanted = nthreads;

  while (started > wanted)
    end_thread ();
}

int eio_poll (void)
{
  int maxreqs = max_poll_reqs;
  struct timeval tv_start, tv_now;
  eio_req *req;

  if (max_poll_time)
    gettimeofday (&tv_start, 0);

  for (;;)
    {
      maybe_start_thread ();

      X_LOCK (reslock);
      req = reqq_shift (&res_queue);

      if (req)
        {
          --npending;

          if (!res_queue.size && done_poll_cb)
            done_poll_cb ();
        }

      X_UNLOCK (reslock);

      if (!req)
        return 0;

      --nreqs;

      if (req->type == EIO_GROUP && req->size)
        {
          req->int1 = 1; /* mark request as delayed */
          continue;
        }
      else
        {
          int res = eio_finish (req);
          if (res)
            return res;
        }

      if (maxreqs && !--maxreqs)
        break;

      if (max_poll_time)
        {
          gettimeofday (&tv_now, 0);

          if (tvdiff (&tv_start, &tv_now) >= max_poll_time)
            break;
        }
    }

  errno = EAGAIN;
  return -1;
}

/*****************************************************************************/
/* work around various missing functions */

#if !HAVE_PREADWRITE
# define pread  eio__pread
# define pwrite eio__pwrite

/*
 * make our pread/pwrite safe against themselves, but not against
 * normal read/write by using a mutex. slows down execution a lot,
 * but that's your problem, not mine.
 */
static mutex_t preadwritelock = X_MUTEX_INIT;

static ssize_t
eio__pread (int fd, void *buf, size_t count, off_t offset)
{
  ssize_t res;
  off_t ooffset;

  X_LOCK (preadwritelock);
  ooffset = lseek (fd, 0, SEEK_CUR);
  lseek (fd, offset, SEEK_SET);
  res = read (fd, buf, count);
  lseek (fd, ooffset, SEEK_SET);
  X_UNLOCK (preadwritelock);

  return res;
}

static ssize_t
eio__pwrite (int fd, void *buf, size_t count, off_t offset)
{
  ssize_t res;
  off_t ooffset;

  X_LOCK (preadwritelock);
  ooffset = lseek (fd, 0, SEEK_CUR);
  lseek (fd, offset, SEEK_SET);
  res = write (fd, buf, count);
  lseek (fd, offset, SEEK_SET);
  X_UNLOCK (preadwritelock);

  return res;
}
#endif

#ifndef HAVE_FUTIMES

# define utimes(path,times)  eio__utimes  (path, times)
# define futimes(fd,times)   eio__futimes (fd, times)

static int
eio__utimes (const char *filename, const struct timeval times[2])
{
  if (times)
    {
      struct utimbuf buf;

      buf.actime  = times[0].tv_sec;
      buf.modtime = times[1].tv_sec;

      return utime (filename, &buf);
    }
  else
    return utime (filename, 0);
}

static int eio__futimes (int fd, const struct timeval tv[2])
{
  errno = ENOSYS;
  return -1;
}

#endif

#if !HAVE_FDATASYNC
# define fdatasync fsync
#endif

#if !HAVE_READAHEAD
# define readahead(fd,offset,count) eio__readahead (fd, offset, count, self)

static ssize_t
eio__readahead (int fd, off_t offset, size_t count, worker *self)
{
  size_t todo = count;
  dBUF;

  while (todo > 0)
    {
      size_t len = todo < EIO_BUFSIZE ? todo : EIO_BUFSIZE;

      pread (fd, eio_buf, len, offset);
      offset += len;
      todo   -= len;
    }

  errno = 0;
  return count;
}

#endif

#if !HAVE_READDIR_R
# define readdir_r eio__readdir_r

static mutex_t readdirlock = X_MUTEX_INIT;
  
static int
eio__readdir_r (DIR *dirp, EIO_STRUCT_DIRENT *ent, EIO_STRUCT_DIRENT **res)
{
  EIO_STRUCT_DIRENT *e;
  int errorno;

  X_LOCK (readdirlock);

  e = readdir (dirp);
  errorno = errno;

  if (e)
    {
      *res = ent;
      strcpy (ent->d_name, e->d_name);
    }
  else
    *res = 0;

  X_UNLOCK (readdirlock);

  errno = errorno;
  return e ? 0 : -1;
}
#endif

/* sendfile always needs emulation */
static ssize_t
eio__sendfile (int ofd, int ifd, off_t offset, size_t count, worker *self)
{
  ssize_t res;

  if (!count)
    return 0;

#if HAVE_SENDFILE
# if __linux
  res = sendfile (ofd, ifd, &offset, count);

# elif __freebsd
  /*
   * Of course, the freebsd sendfile is a dire hack with no thoughts
   * wasted on making it similar to other I/O functions.
   */
  {
    off_t sbytes;
    res = sendfile (ifd, ofd, offset, count, 0, &sbytes, 0);

    if (res < 0 && sbytes)
      /* maybe only on EAGAIN: as usual, the manpage leaves you guessing */
      res = sbytes;
  }

# elif __hpux
  res = sendfile (ofd, ifd, offset, count, 0, 0);

# elif __solaris
  {
    struct sendfilevec vec;
    size_t sbytes;

    vec.sfv_fd   = ifd;
    vec.sfv_flag = 0;
    vec.sfv_off  = offset;
    vec.sfv_len  = count;

    res = sendfilev (ofd, &vec, 1, &sbytes);

    if (res < 0 && sbytes)
      res = sbytes;
  }

# endif
#else
  res = -1;
  errno = ENOSYS;
#endif

  if (res <  0
      && (errno == ENOSYS || errno == EINVAL || errno == ENOTSOCK
#if __solaris
          || errno == EAFNOSUPPORT || errno == EPROTOTYPE
#endif
         )
      )
    {
      /* emulate sendfile. this is a major pain in the ass */
      dBUF;

      res = 0;

      while (count)
        {
          ssize_t cnt;
          
          cnt = pread (ifd, eio_buf, count > EIO_BUFSIZE ? EIO_BUFSIZE : count, offset);

          if (cnt <= 0)
            {
              if (cnt && !res) res = -1;
              break;
            }

          cnt = write (ofd, eio_buf, cnt);

          if (cnt <= 0)
            {
              if (cnt && !res) res = -1;
              break;
            }

          offset += cnt;
          res    += cnt;
          count  -= cnt;
        }
    }

  return res;
}

/* read a full directory */
static void
eio__scandir (eio_req *req, worker *self)
{
  DIR *dirp;
  union
  {    
    EIO_STRUCT_DIRENT d;
    char b [offsetof (EIO_STRUCT_DIRENT, d_name) + NAME_MAX + 1];
  } *u;
  EIO_STRUCT_DIRENT *entp;
  char *name, *names;
  int memlen = 4096;
  int memofs = 0;
  int res = 0;

  X_LOCK (wrklock);
  self->dirp = dirp = opendir (req->ptr1);
  self->dbuf = u = malloc (sizeof (*u));
  req->flags |= EIO_FLAG_PTR2_FREE;
  req->ptr2 = names = malloc (memlen);
  X_UNLOCK (wrklock);

  if (dirp && u && names)
    for (;;)
      {
        errno = 0;
        readdir_r (dirp, &u->d, &entp);

        if (!entp)
          break;

        name = entp->d_name;

        if (name [0] != '.' || (name [1] && (name [1] != '.' || name [2])))
          {
            int len = strlen (name) + 1;

            res++;

            while (memofs + len > memlen)
              {
                memlen *= 2;
                X_LOCK (wrklock);
                req->ptr2 = names = realloc (names, memlen);
                X_UNLOCK (wrklock);

                if (!names)
                  break;
              }

            memcpy (names + memofs, name, len);
            memofs += len;
          }
      }

  if (errno)
    res = -1;
  
  req->result = res;
}

/*****************************************************************************/

#define ALLOC(len)				\
  if (!req->ptr2)				\
    {						\
      X_LOCK (wrklock);				\
      req->flags |= EIO_FLAG_PTR2_FREE;		\
      X_UNLOCK (wrklock);			\
      req->ptr2 = malloc (len);			\
      if (!req->ptr2)				\
        {					\
          errno       = ENOMEM;			\
          req->result = -1;			\
          break;				\
        }					\
    }

X_THREAD_PROC (eio_proc)
{
  eio_req *req;
  struct timespec ts;
  worker *self = (worker *)thr_arg;

  /* try to distribute timeouts somewhat randomly */
  ts.tv_nsec = ((unsigned long)self & 1023UL) * (1000000000UL / 1024UL);

  for (;;)
    {
      ts.tv_sec  = time (0) + IDLE_TIMEOUT;

      X_LOCK (reqlock);

      for (;;)
        {
          self->req = req = reqq_shift (&req_queue);

          if (req)
            break;

          ++idle;

          if (X_COND_TIMEDWAIT (reqwait, reqlock, ts) == ETIMEDOUT)
            {
              if (idle > max_idle)
                {
                  --idle;
                  X_UNLOCK (reqlock);
                  X_LOCK (wrklock);
                  --started;
                  X_UNLOCK (wrklock);
                  goto quit;
                }

              /* we are allowed to idle, so do so without any timeout */
              X_COND_WAIT (reqwait, reqlock);
              ts.tv_sec  = time (0) + IDLE_TIMEOUT;
            }

          --idle;
        }

      --nready;

      X_UNLOCK (reqlock);
     
      errno = 0; /* strictly unnecessary */

      if (!EIO_CANCELLED (req))
        switch (req->type)
          {
            case EIO_READ:      ALLOC (req->size);
                                req->result = req->offs >= 0
                                            ? pread     (req->int1, req->ptr2, req->size, req->offs)
                                            : read      (req->int1, req->ptr2, req->size); break;
            case EIO_WRITE:     req->result = req->offs >= 0
                                            ? pwrite    (req->int1, req->ptr2, req->size, req->offs)
                                            : write     (req->int1, req->ptr2, req->size); break;

            case EIO_READAHEAD: req->result = readahead     (req->int1, req->offs, req->size); break;
            case EIO_SENDFILE:  req->result = eio__sendfile (req->int1, req->int2, req->offs, req->size, self); break;

            case EIO_STAT:      ALLOC (sizeof (EIO_STRUCT_STAT));
                                req->result = stat      (req->ptr1, (EIO_STRUCT_STAT *)req->ptr2); break;
            case EIO_LSTAT:     ALLOC (sizeof (EIO_STRUCT_STAT));
                                req->result = lstat     (req->ptr1, (EIO_STRUCT_STAT *)req->ptr2); break;
            case EIO_FSTAT:     ALLOC (sizeof (EIO_STRUCT_STAT));
                                req->result = fstat     (req->int1, (EIO_STRUCT_STAT *)req->ptr2); break;

            case EIO_CHOWN:     req->result = chown     (req->ptr1, req->int2, req->int3); break;
            case EIO_FCHOWN:    req->result = fchown    (req->int1, req->int2, req->int3); break;
            case EIO_CHMOD:     req->result = chmod     (req->ptr1, (mode_t)req->int2); break;
            case EIO_FCHMOD:    req->result = fchmod    (req->int1, (mode_t)req->int2); break;
            case EIO_TRUNCATE:  req->result = truncate  (req->ptr1, req->offs); break;
            case EIO_FTRUNCATE: req->result = ftruncate (req->int1, req->offs); break;

            case EIO_OPEN:      req->result = open      (req->ptr1, req->int1, (mode_t)req->int2); break;
            case EIO_CLOSE:     req->result = close     (req->int1); break;
            case EIO_DUP2:      req->result = dup2      (req->int1, req->int2); break;
            case EIO_UNLINK:    req->result = unlink    (req->ptr1); break;
            case EIO_RMDIR:     req->result = rmdir     (req->ptr1); break;
            case EIO_MKDIR:     req->result = mkdir     (req->ptr1, (mode_t)req->int2); break;
            case EIO_RENAME:    req->result = rename    (req->ptr1, req->ptr2); break;
            case EIO_LINK:      req->result = link      (req->ptr1, req->ptr2); break;
            case EIO_SYMLINK:   req->result = symlink   (req->ptr1, req->ptr2); break;
            case EIO_MKNOD:     req->result = mknod     (req->ptr1, (mode_t)req->int2, (dev_t)req->offs); break;

            case EIO_READLINK:  ALLOC (NAME_MAX);
                                req->result = readlink  (req->ptr1, req->ptr2, NAME_MAX); break;

            case EIO_SYNC:      req->result = 0; sync (); break;
            case EIO_FSYNC:     req->result = fsync     (req->int1); break;
            case EIO_FDATASYNC: req->result = fdatasync (req->int1); break;

            case EIO_READDIR:   eio__scandir (req, self); break;

            case EIO_BUSY:
#ifdef _WIN32
	      Sleep (req->nv1 * 1000.);
#else
              {
                struct timeval tv;

                tv.tv_sec  = req->nv1;
                tv.tv_usec = (req->nv1 - tv.tv_sec) * 1000000.;

                req->result = select (0, 0, 0, 0, &tv);
              }
#endif
              break;

            case EIO_UTIME:
            case EIO_FUTIME:
              {
                struct timeval tv[2];
                struct timeval *times;

                if (req->nv1 != -1. || req->nv2 != -1.)
                  {
                    tv[0].tv_sec  = req->nv1;
                    tv[0].tv_usec = (req->nv1 - tv[0].tv_sec) * 1000000.;
                    tv[1].tv_sec  = req->nv2;
                    tv[1].tv_usec = (req->nv2 - tv[1].tv_sec) * 1000000.;

                    times = tv;
                  }
                else
                  times = 0;


                req->result = req->type == EIO_FUTIME
                              ? futimes (req->int1, times)
                              : utimes  (req->ptr1, times);
              }

            case EIO_GROUP:
            case EIO_NOP:
              req->result = 0;
              break;

            case EIO_QUIT:
              goto quit;

            default:
              req->result = -1;
              break;
          }

      req->errorno = errno;

      X_LOCK (reslock);

      ++npending;

      if (!reqq_push (&res_queue, req) && want_poll_cb)
        want_poll_cb ();

      self->req = 0;
      worker_clear (self);

      X_UNLOCK (reslock);
    }

quit:
  X_LOCK (wrklock);
  worker_free (self);
  X_UNLOCK (wrklock);

  return 0;
}

/*****************************************************************************/

static void eio_atfork_prepare (void)
{
  X_LOCK (wrklock);
  X_LOCK (reqlock);
  X_LOCK (reslock);
#if !HAVE_PREADWRITE
  X_LOCK (preadwritelock);
#endif
#if !HAVE_READDIR_R
  X_LOCK (readdirlock);
#endif
}

static void eio_atfork_parent (void)
{
#if !HAVE_READDIR_R
  X_UNLOCK (readdirlock);
#endif
#if !HAVE_PREADWRITE
  X_UNLOCK (preadwritelock);
#endif
  X_UNLOCK (reslock);
  X_UNLOCK (reqlock);
  X_UNLOCK (wrklock);
}

static void eio_atfork_child (void)
{
  eio_req *prv;

  while (prv = reqq_shift (&req_queue))
    eio_destroy (prv);

  while (prv = reqq_shift (&res_queue))
    eio_destroy (prv);

  while (wrk_first.next != &wrk_first)
    {
      worker *wrk = wrk_first.next;

      if (wrk->req)
        eio_destroy (wrk->req);

      worker_clear (wrk);
      worker_free (wrk);
    }

  started  = 0;
  idle     = 0;
  nreqs    = 0;
  nready   = 0;
  npending = 0;

  eio_atfork_parent ();
}

int eio_init (void (*want_poll)(void), void (*done_poll)(void))
{
  want_poll_cb = want_poll;
  done_poll_cb = done_poll;

#ifdef _WIN32
  X_MUTEX_CHECK (wrklock);
  X_MUTEX_CHECK (reslock);
  X_MUTEX_CHECK (reqlock);
  X_MUTEX_CHECK (reqwait);
  X_MUTEX_CHECK (preadwritelock);
  X_MUTEX_CHECK (readdirlock);

  X_COND_CHECK  (reqwait);
#endif

  X_THREAD_ATFORK (eio_atfork_prepare, eio_atfork_parent, eio_atfork_child);
}

static void eio_api_destroy (eio_req *req)
{
  free (req);
}

#define REQ(rtype)                                            	\
  eio_req *req;                                                 \
                                                                \
  req = (eio_req *)calloc (1, sizeof *req);                     \
  if (!req)                                                     \
    return 0;                                                   \
                                                                \
  req->type    = rtype;                                         \
  req->pri     = pri;						\
  req->finish  = cb;						\
  req->data    = data;						\
  req->destroy = eio_api_destroy;

#define SEND eio_submit (req); return req

#define PATH							\
  req->flags |= EIO_FLAG_PTR1_FREE;				\
  req->ptr1 = strdup (path);					\
  if (!req->ptr1)						\
    {								\
      eio_api_destroy (req);					\
      return 0;							\
    }

eio_req *eio_nop (int pri, eio_cb cb, void *data)
{
  REQ (EIO_NOP); SEND;
}

eio_req *eio_busy (double delay, int pri, eio_cb cb, void *data)
{
  REQ (EIO_BUSY); req->nv1 = delay; SEND;
}

eio_req *eio_sync (int pri, eio_cb cb, void *data)
{
  REQ (EIO_SYNC); SEND;
}

eio_req *eio_fsync (int fd, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FSYNC); req->int1 = fd; SEND;
}

eio_req *eio_fdatasync (int fd, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FDATASYNC); req->int1 = fd; SEND;
}

eio_req *eio_close (int fd, int pri, eio_cb cb, void *data)
{
  REQ (EIO_CLOSE); req->int1 = fd; SEND;
}

eio_req *eio_readahead (int fd, off_t offset, size_t length, int pri, eio_cb cb, void *data)
{
  REQ (EIO_READAHEAD); req->int1 = fd; req->offs = offset; req->size = length; SEND;
}

eio_req *eio_read (int fd, void *buf, size_t length, off_t offset, int pri, eio_cb cb, void *data)
{
  REQ (EIO_READ); req->int1 = fd; req->offs = offset; req->size = length; req->ptr2 = buf; SEND;
}

eio_req *eio_write (int fd, void *buf, size_t length, off_t offset, int pri, eio_cb cb, void *data)
{
  REQ (EIO_WRITE); req->int1 = fd; req->offs = offset; req->size = length; req->ptr2 = buf; SEND;
}

eio_req *eio_fstat (int fd, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FSTAT); req->int1 = fd; SEND;
}

eio_req *eio_futime (int fd, double atime, double mtime, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FUTIME); req->int1 = fd; req->nv1 = atime; req->nv2 = mtime; SEND;
}

eio_req *eio_ftruncate (int fd, off_t offset, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FTRUNCATE); req->int1 = fd; req->offs = offset; SEND;
}

eio_req *eio_fchmod (int fd, mode_t mode, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FCHMOD); req->int1 = fd; req->int2 = (long)mode; SEND;
}

eio_req *eio_fchown (int fd, uid_t uid, gid_t gid, int pri, eio_cb cb, void *data)
{
  REQ (EIO_FCHOWN); req->int1 = fd; req->int2 = (long)uid; req->int3 = (long)gid; SEND;
}

eio_req *eio_dup2 (int fd, int fd2, int pri, eio_cb cb, void *data)
{
  REQ (EIO_DUP2); req->int1 = fd; req->int2 = fd2; SEND;
}

eio_req *eio_sendfile (int out_fd, int in_fd, off_t in_offset, size_t length, int pri, eio_cb cb, void *data)
{
  REQ (EIO_SENDFILE); req->int1 = out_fd; req->int2 = in_fd; req->offs = in_offset; req->size = length; SEND;
}

eio_req *eio_open (const char *path, int flags, mode_t mode, int pri, eio_cb cb, void *data)
{
  REQ (EIO_OPEN); PATH; req->int1 = flags; req->int2 = (long)mode; SEND;
}

eio_req *eio_utime (const char *path, double atime, double mtime, int pri, eio_cb cb, void *data)
{
  REQ (EIO_UTIME); PATH; req->nv1 = atime; req->nv2 = mtime; SEND;
}

eio_req *eio_truncate (const char *path, off_t offset, int pri, eio_cb cb, void *data)
{
  REQ (EIO_TRUNCATE); PATH; req->offs = offset; SEND;
}

eio_req *eio_chown (const char *path, uid_t uid, gid_t gid, int pri, eio_cb cb, void *data)
{
  REQ (EIO_CHOWN); PATH; req->int2 = (long)uid; req->int3 = (long)gid; SEND;
}

eio_req *eio_chmod (const char *path, mode_t mode, int pri, eio_cb cb, void *data)
{
  REQ (EIO_CHMOD); PATH; req->int2 = (long)mode; SEND;
}

eio_req *eio_mkdir (const char *path, mode_t mode, int pri, eio_cb cb, void *data)
{
  REQ (EIO_MKDIR); PATH; req->int2 = (long)mode; SEND;
}

static eio_req *
eio__1path (int type, const char *path, int pri, eio_cb cb, void *data)
{
  REQ (type); PATH; SEND;
}

eio_req *eio_readlink (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_READLINK, path, pri, cb, data);
}

eio_req *eio_stat (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_STAT, path, pri, cb, data);
}

eio_req *eio_lstat (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_LSTAT, path, pri, cb, data);
}

eio_req *eio_unlink (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_UNLINK, path, pri, cb, data);
}

eio_req *eio_rmdir (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_RMDIR, path, pri, cb, data);
}

eio_req *eio_readdir (const char *path, int pri, eio_cb cb, void *data)
{
  return eio__1path (EIO_READDIR, path, pri, cb, data);
}

eio_req *eio_mknod (const char *path, mode_t mode, dev_t dev, int pri, eio_cb cb, void *data)
{
  REQ (EIO_MKNOD); PATH; req->int2 = (long)mode; req->int2 = (long)dev; SEND;
}

static eio_req *
eio__2path (int type, const char *path, const char *new_path, int pri, eio_cb cb, void *data)
{
  REQ (type); PATH;

  req->flags |= EIO_FLAG_PTR2_FREE;
  req->ptr2 = strdup (new_path);
  if (!req->ptr2)
    {
      eio_api_destroy (req);
      return 0;
    }

  SEND;
}

eio_req *eio_link (const char *path, const char *new_path, int pri, eio_cb cb, void *data)
{
  return eio__2path (EIO_LINK, path, new_path, pri, cb, data);
}

eio_req *eio_symlink (const char *path, const char *new_path, int pri, eio_cb cb, void *data)
{
  return eio__2path (EIO_SYMLINK, path, new_path, pri, cb, data);
}

eio_req *eio_rename (const char *path, const char *new_path, int pri, eio_cb cb, void *data)
{
  return eio__2path (EIO_RENAME, path, new_path, pri, cb, data);
}

eio_req *eio_grp (eio_cb cb, void *data)
{
  const int pri = EIO_PRI_MAX;

  REQ (EIO_GROUP); SEND;
}

#undef REQ
#undef PATH
#undef SEND

/*****************************************************************************/
/* grp functions */

void eio_grp_feed (eio_req *grp, void (*feed)(eio_req *req), int limit)
{
  grp->int2 = limit;
  grp->feed = feed;

  grp_try_feed (grp);
}

void eio_grp_limit (eio_req *grp, int limit)
{
  grp->int2 = limit;

  grp_try_feed (grp);
}

void eio_grp_add (eio_req *grp, eio_req *req)
{
  assert (("cannot add requests to IO::AIO::GRP after the group finished", grp->int1 != 2));

  ++grp->size;
  req->grp = grp;

  req->grp_prev = 0;
  req->grp_next = grp->grp_first;

  if (grp->grp_first)
    grp->grp_first->grp_prev = req;

  grp->grp_first = req;
}

/*****************************************************************************/
/* misc garbage */

ssize_t eio_sendfile_sync (int ofd, int ifd, off_t offset, size_t count)
{
  worker wrk;

  wrk.dbuf = 0;

  eio__sendfile (ofd, ifd, offset, count, &wrk);

  if (wrk.dbuf)
    free (wrk.dbuf);
}

