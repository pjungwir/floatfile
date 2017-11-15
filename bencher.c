#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>

#include <postgres.h>
#include <catalog/pg_type.h>

#include "hist2d.h"

int main(int argc, char **argv) {
  int i;
  int x_fd, x_nulls_fd, y_fd, y_nulls_fd;
  int64 *counts;
  char *errstr;


  for (i = 0; i < 100; i++) {
    x_fd       = open("/usr/local/var/lib/postgresql/9.6/main/floatfile/18048/ds3820ch1.v", O_RDONLY);
    if (x_fd == -1) { perror("x_fd"); exit(1); }
    x_nulls_fd = open("/usr/local/var/lib/postgresql/9.6/main/floatfile/18048/ds3820ch1.n", O_RDONLY);
    if (x_nulls_fd == -1) { perror("x_nulls_fd"); exit(1); }
    y_fd       = open("/usr/local/var/lib/postgresql/9.6/main/floatfile/18048/ds3820ch2.v", O_RDONLY);
    if (y_fd == -1) { perror("y_fd"); exit(1); }
    y_nulls_fd = open("/usr/local/var/lib/postgresql/9.6/main/floatfile/18048/ds3820ch2.n", O_RDONLY);
    if (y_nulls_fd == -1) { perror("y_nulls_fd"); exit(1); }

    counts = calloc(10*10, sizeof(counts[0]));

    build_histogram(x_fd, x_nulls_fd, -44, 40, 10,
                    y_fd, y_nulls_fd,   3, .1, 10,
                    counts, &errstr);

    if (close(x_fd)) { perror("close x_fd"); exit(1); }
    if (close(y_fd)) { perror("close y_fd"); exit(1); }
    if (close(x_nulls_fd)) { perror("close y_fd"); exit(1); }
    if (close(y_nulls_fd)) { perror("close y_nulls_fd"); exit(1); }

    // Do something just to convince the compiler that we're using the result:
    printf("%ld...", counts[99]);
  }

  return 0;
}
