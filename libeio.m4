AC_SEARCH_LIBS(
   pthread_create,
   [pthread pthreads pthreadVC2],
   ,
   [AC_MSG_ERROR(pthread functions not found)]
)

AC_CACHE_CHECK(for futimes, ac_cv_futimes, [AC_LINK_IFELSE([[
#include <sys/types.h>
#include <sys/time.h>
#include <utime.h>
struct timeval tv[2];
int res;
int fd;
int main(void)
{
   res = futimes (fd, tv);
   return 0;
}
]],ac_cv_futimes=yes,ac_cv_futimes=no)])
test $ac_cv_futimes = yes && AC_DEFINE(HAVE_FUTIMES, 1, futimes(2) is available)

AC_CACHE_CHECK(for readahead, ac_cv_readahead, [AC_LINK_IFELSE([
#define _GNU_SOURCE
#include <fcntl.h>
int main(void)
{
   int fd = 0;
   off64_t offset = 1;
   size_t count = 2;
   ssize_t res;
   res = readahead (fd, offset, count);
   return 0;
}
],ac_cv_readahead=yes,ac_cv_readahead=no)])
test $ac_cv_readahead = yes && AC_DEFINE(HAVE_READAHEAD, 1, readahead(2) is available (linux))
test $ac_cv_readahead = yes && AC_DEFINE(_GNU_SOURCE, 1, _GNU_SOURCE required for readahead (linux))

AC_CACHE_CHECK(for fdatasync, ac_cv_fdatasync, [AC_LINK_IFELSE([
#include <unistd.h>
int main(void)
{
   int fd = 0;
   fdatasync (fd);
   return 0;
}
],ac_cv_fdatasync=yes,ac_cv_fdatasync=no)])
test $ac_cv_fdatasync = yes && AC_DEFINE(HAVE_FDATASYNC, 1, fdatasync(2) is available)

AC_CACHE_CHECK(for pread and pwrite, ac_cv_preadwrite, [AC_LINK_IFELSE([
#include <unistd.h>
int main(void)
{
   int fd = 0;
   size_t count = 1;
   char buf;
   off_t offset = 1;
   ssize_t res;
   res = pread (fd, &buf, count, offset);
   res = pwrite (fd, &buf, count, offset);
   return 0;
}
],ac_cv_preadwrite=yes,ac_cv_preadwrite=no)])
test $ac_cv_preadwrite = yes && AC_DEFINE(HAVE_PREADWRITE, 1, pread(2) and pwrite(2) are available)

AC_CACHE_CHECK(for readdir_r, ac_cv_readdir_r, [AC_LINK_IFELSE([
#include <dirent.h>
int main(void)
{
   DIR *dir = 0;
   struct dirent ent, *eres;
   int res = readdir_r (dir, &ent, &eres);
   return 0;
}
],ac_cv_readdir_r=yes,ac_cv_readdir_r=no)])
test $ac_cv_readdir_r = yes && AC_DEFINE(HAVE_READDIR_R, 1, readdir_r is available)

AC_CACHE_CHECK(for sendfile, ac_cv_sendfile, [AC_LINK_IFELSE([
# include <sys/types.h>
#if __linux
# include <sys/sendfile.h>
#elif __freebsd
# include <sys/socket.h>
# include <sys/uio.h>
#elif __hpux
# include <sys/socket.h>
#else
# error unsupported architecture
#endif
int main(void)
{
   int fd = 0;
   off_t offset = 1;
   size_t count = 2;
   ssize_t res;
#if __linux
   res = sendfile (fd, fd, offset, count);
#elif __freebsd
   res = sendfile (fd, fd, offset, count, 0, &offset, 0);
#elif __hpux
   res = sendfile (fd, fd, offset, count, 0, 0);
#endif
   return 0;
}
],ac_cv_sendfile=yes,ac_cv_sendfile=no)])
test $ac_cv_sendfile = yes && AC_DEFINE(HAVE_SENDFILE, 1, sendfile(2) is available and supported)

