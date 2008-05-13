#ifndef EIO_H_
#define EIO_H_

#include <stddef.h>
#include <sys/types.h>

typedef struct eio_req eio_req;

typedef int (*eio_cb)(eio_req *req);

#ifndef EIO_REQ_MEMBERS
# define EIO_REQ_MEMBERS
#endif

#ifndef EIO_STRUCT_STAT
# define EIO_STRUCT_STAT struct stat
#endif

enum {
  EIO_QUIT,
  EIO_OPEN, EIO_CLOSE, EIO_DUP2,
  EIO_READ, EIO_WRITE,
  EIO_READAHEAD, EIO_SENDFILE,
  EIO_STAT, EIO_LSTAT, EIO_FSTAT,
  EIO_TRUNCATE, EIO_FTRUNCATE,
  EIO_UTIME, EIO_FUTIME,
  EIO_CHMOD, EIO_FCHMOD,
  EIO_CHOWN, EIO_FCHOWN,
  EIO_SYNC, EIO_FSYNC, EIO_FDATASYNC,
  EIO_UNLINK, EIO_RMDIR, EIO_MKDIR, EIO_RENAME,
  EIO_MKNOD, EIO_READDIR,
  EIO_LINK, EIO_SYMLINK, EIO_READLINK,
  EIO_GROUP, EIO_NOP,
  EIO_BUSY,
};

typedef double eio_tstamp; /* feel free to use double in your code directly */

/* eio request structure */
/* this structure is mostly read-only */
struct eio_req
{
  eio_req volatile *next; /* private */

  ssize_t result;  /* result of syscall, e.g. result = read (... */
  off_t offs;      /* read, write, truncate, readahead: file offset; mknod: dev_t */
  size_t size;     /* read, write, readahead, sendfile: length */
  void *ptr1;      /* all applicable requests: pathname, old name */
  void *ptr2;      /* all applicable requests: new name or memory buffer */
  eio_tstamp nv1;  /* utime, futime: atime; busy: sleep time */
  eio_tstamp nv2;  /* utime, futime: mtime */

  int type;        /* EIO_xxx constant */
  int int1;        /* all applicable requests: file descriptor; sendfile: output fd; open: flags */
  long int2;       /* chown, fchown: uid; sendfile: input fd; open, chmod, mkdir, mknod: file mode */
  long int3;       /* chown, fchown: gid */
  int errorno;     /* errno value on syscall return */

  unsigned char flags; /* private */
  signed char pri;     /* the priority */

  void *data;
  eio_cb finish;
  void (*destroy)(eio_req *req); /* called when requets no longer needed */
  void (*feed)(eio_req *req);    /* only used for group requests */

  EIO_REQ_MEMBERS

  eio_req *grp, *grp_prev, *grp_next, *grp_first; /* private */
};

enum {
  EIO_FLAG_CANCELLED = 0x01, /* request was cancelled */
  EIO_FLAG_PTR1_FREE = 0x02, /* need to free(ptr1) */
  EIO_FLAG_PTR2_FREE = 0x04  /* need to free(ptr2) */
};

enum {
  EIO_PRI_MIN     = -4,
  EIO_PRI_MAX     =  4,

  EIO_PRI_DEFAULT = 0,
  EIO_PRI_BIAS    = -EIO_PRI_MIN,
  EIO_NUM_PRI     = EIO_PRI_MAX + EIO_PRI_BIAS + 1
};

/* returns < 0 on error, errno set
 * need_poll, if non-zero, will be called when results are available
 * and eio_poll_cb needs to be invoked (it MUST NOT call eio_poll_cb itself).
 * done_poll is called when the need to poll is gone.
 */
int eio_init (void (*want_poll)(void), void (*done_poll)(void));

/* must be called regularly to handle pending requests */
/* returns 0 if all requests were handled, -1 if not, or the value of EIO_FINISH if != 0 */
int eio_poll (void);

/* stop polling if poll took longer than duration seconds */
void eio_set_max_poll_time (eio_tstamp nseconds);
/* do not handle more then count requests in one call to eio_poll_cb */
void eio_set_max_poll_reqs (unsigned int nreqs);

/* set minimum required number
 * maximum wanted number
 * or maximum idle number of threads */
void eio_set_min_parallel (unsigned int nthreads);
void eio_set_max_parallel (unsigned int nthreads);
void eio_set_max_idle     (unsigned int nthreads);

unsigned int eio_nreqs    (void); /* number of requests in-flight */
unsigned int eio_nready   (void); /* number of not-yet handled requests */
unsigned int eio_npending (void); /* numbe rof finished but unhandled requests */
unsigned int eio_nthreads (void); /* number of worker threads in use currently */

/*****************************************************************************/
/* high-level request API */

eio_req *eio_nop       (int pri, eio_cb cb, void *data); /* does nothing except go through the whole process */
eio_req *eio_busy      (eio_tstamp delay, int pri, eio_cb cb, void *data); /* ties a thread for this long, simulating busyness */
eio_req *eio_sync      (int pri, eio_cb cb, void *data);
eio_req *eio_fsync     (int fd, int pri, eio_cb cb, void *data);
eio_req *eio_fdatasync (int fd, int pri, eio_cb cb, void *data);
eio_req *eio_close     (int fd, int pri, eio_cb cb, void *data);
eio_req *eio_readahead (int fd, off_t offset, size_t length, int pri, eio_cb cb, void *data);
eio_req *eio_read      (int fd, void *buf, size_t length, off_t offset, int pri, eio_cb cb, void *data);
eio_req *eio_write     (int fd, void *buf, size_t length, off_t offset, int pri, eio_cb cb, void *data);
eio_req *eio_fstat     (int fd, int pri, eio_cb cb, void *data); /* stat buffer=ptr2 allocated dynamically */
eio_req *eio_futime    (int fd, eio_tstamp atime, eio_tstamp mtime, int pri, eio_cb cb, void *data);
eio_req *eio_ftruncate (int fd, off_t offset, int pri, eio_cb cb, void *data);
eio_req *eio_fchmod    (int fd, mode_t mode, int pri, eio_cb cb, void *data);
eio_req *eio_fchown    (int fd, uid_t uid, gid_t gid, int pri, eio_cb cb, void *data);
eio_req *eio_dup2      (int fd, int fd2, int pri, eio_cb cb, void *data);
eio_req *eio_sendfile  (int out_fd, int in_fd, off_t in_offset, size_t length, int pri, eio_cb cb, void *data);
eio_req *eio_open      (const char *path, int flags, mode_t mode, int pri, eio_cb cb, void *data);
eio_req *eio_utime     (const char *path, eio_tstamp atime, eio_tstamp mtime, int pri, eio_cb cb, void *data);
eio_req *eio_truncate  (const char *path, off_t offset, int pri, eio_cb cb, void *data);
eio_req *eio_chown     (const char *path, uid_t uid, gid_t gid, int pri, eio_cb cb, void *data);
eio_req *eio_chmod     (const char *path, mode_t mode, int pri, eio_cb cb, void *data);
eio_req *eio_mkdir     (const char *path, mode_t mode, int pri, eio_cb cb, void *data);
eio_req *eio_readdir   (const char *path, int pri, eio_cb cb, void *data); /* result=ptr2 allocated dynamically */
eio_req *eio_rmdir     (const char *path, int pri, eio_cb cb, void *data);
eio_req *eio_unlink    (const char *path, int pri, eio_cb cb, void *data);
eio_req *eio_readlink  (const char *path, int pri, eio_cb cb, void *data); /* result=ptr2 allocated dynamically */
eio_req *eio_stat      (const char *path, int pri, eio_cb cb, void *data); /* stat buffer=ptr2 allocated dynamically */
eio_req *eio_lstat     (const char *path, int pri, eio_cb cb, void *data); /* stat buffer=ptr2 allocated dynamically */
eio_req *eio_mknod     (const char *path, mode_t mode, dev_t dev, int pri, eio_cb cb, void *data);
eio_req *eio_link      (const char *path, const char *new_path, int pri, eio_cb cb, void *data);
eio_req *eio_symlink   (const char *path, const char *new_path, int pri, eio_cb cb, void *data);
eio_req *eio_rename    (const char *path, const char *new_path, int pri, eio_cb cb, void *data);

/* for groups */
eio_req *eio_grp       (eio_cb cb, void *data);
void eio_grp_feed      (eio_req *grp, void (*feed)(eio_req *req), int limit);
void eio_grp_limit     (eio_req *grp, int limit);
void eio_grp_add       (eio_req *grp, eio_req *req);
void eio_grp_cancel    (eio_req *grp); /* cancels all sub requests but not the group */

/* cancel a request as soon fast as possible */
void eio_cancel (eio_req *req);
/* destroy a request that has never been submitted */
void eio_destroy (eio_req *req);

/* true if the request was cancelled, useful in the invoke callback */
#define EIO_CANCELLED(req) ((req)->flags & EIO_FLAG_CANCELLED)

#define EIO_RESULT(req)    ((req)->result)
/* returns a pointer to the result buffer allocated by eio */
#define EIO_BUF(req)       ((req)->ptr2)
#define EIO_STAT_BUF(req)  ((EIO_STRUCT_STAT *)EIO_BUF(req))
#define EIO_PATH(req)      ((char *)(req)->ptr1)

/*****************************************************************************/
/* low-level request API */

/* must be used to initialise eio_req's */
#define EIO_INIT(req,prio,finish_cb, destroy_cb)	\
  memset ((req), 0, sizeof (eio_req));	\
  (req)->pri = (prio) + EIO_PRI_BIAS;	\
  (req)->finish  = (finish_cb);		\
  (req)->destroy = (destroy_cb)

/* submit a request for execution */
void eio_submit (eio_req *req);

/*****************************************************************************/
/* convinience functions */

ssize_t eio_sendfile_sync (int ofd, int ifd, off_t offset, size_t count);

#endif

