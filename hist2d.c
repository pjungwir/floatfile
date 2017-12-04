/**
 * hist2d.c - We want to keep most of the work
 * in a separately-compilable unit
 * with no Postgres dependencies
 * so that we can more easily test and profile it.
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <postgres.h>
#include <catalog/pg_type.h>

#include "hist2d.h"

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

// posix_fadvise is not available on macOS,
// so just turn this on for Linux for now:
// It doesn't seem to make much difference anyway....
#ifdef __linux__
#define CAN_FADVISE
#endif

// macOS has a max stack frame of 8MB:
// If this were an executable we could use
//
//     LDFLAGS=-Wl,-stack_size,1000000
//
// but that won't work when building an .so.
// #define HIST_BUFFER 1024*1024
#define HIST_BUFFER 512*512
// #define HIST_BUFFER BUFSIZ

/**
 * load_dimension - loads the vals and nulls from a floatfile.
 *
 * Returns the number of values read (not the number of bytes read),
 * or -1 on an error.
 */
static int load_dimension(int already_read, int vals_fd, int nulls_fd, float8 *vals, bool *nulls, char **errstr) {
  ssize_t bytes_read;
  int vals_read;

  bytes_read = read(vals_fd, vals, HIST_BUFFER*sizeof(float8));
  if (bytes_read == 0) {
    return 0;
  } else if (bytes_read == -1) {
    *errstr = strerror(errno);
    return -1;
  }

  vals_read = bytes_read / sizeof(float8);
  already_read += vals_read;
#ifdef CAN_FADVISE
  if (posix_fadvise(vals_fd, already_read * sizeof(float8), HIST_BUFFER, POSIX_FADV_WILLNEED)) {
    *errstr = "can't give advise to vals_fd";
    return -1;
  }
#endif

  bytes_read = read(nulls_fd, nulls, HIST_BUFFER*sizeof(bool));
  if (bytes_read == -1) {
    *errstr = strerror(errno);
    return -1;
  } else if (bytes_read != vals_read*sizeof(bool)) {
    *errstr = "nulls count doesn't equal val count";
    return -1;
  }
#ifdef CAN_FADVISE
  if (posix_fadvise(nulls_fd, already_read * sizeof(bool), HIST_BUFFER, POSIX_FADV_WILLNEED)) {
    *errstr = "can't give advise to nulls_fd";
    return -1;
  }
#endif

  return vals_read;
}

static void count_vals(int more_vals, int64 *counts, float8 *xs, bool *x_nulls, float8 x_min, float8 x_width, int x_count, float8 *ys, bool *y_nulls, float8 y_min, float8 y_width, int y_count) {
  size_t i;
  float8 x, y;
  float8 x_pos, y_pos;

  for (i = 0; i < more_vals; i += 1) {
    if (x_nulls[i] || y_nulls[i]) continue;
    x = xs[i];
    y = ys[i];

    x_pos = (x - x_min) / x_width;
    y_pos = (y - y_min) / y_width;

    if (x_pos >= 0 && x_pos < x_count && y_pos >= 0 && y_pos < y_count) {
      counts[(int)x_pos * y_count + (int)y_pos] += 1;
    }
  }
}

int build_histogram(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                    int y_fd, int y_nulls_fd, float8 y_min, float8 y_width, int32 y_count,
                    int64 *counts, char **errstr) {
  // TODO: int64 or int32 depending....
  float8 xs[HIST_BUFFER];
  float8 ys[HIST_BUFFER];
  bool x_nulls[HIST_BUFFER];
  bool y_nulls[HIST_BUFFER];
  int already_read = 0, x_vals_read, y_vals_read;
#ifdef PROFILING
  struct timespec last_tp, tp;
  long elapsed;
#endif

#ifdef PROFILING
  fprintf(stderr, "another run\n");
  if (clock_gettime(CLOCK_MONOTONIC, &last_tp)) { perror("clock failed"); exit(1); }
#endif
  while ((x_vals_read = load_dimension(already_read, x_fd, x_nulls_fd, xs, x_nulls, errstr))) {
    if (x_vals_read == -1) return -1;   // errstr is already set

    y_vals_read = load_dimension(already_read, y_fd, y_nulls_fd, ys, y_nulls, errstr);
    if (y_vals_read == -1) return -1;   // errstr is already set
    if (x_vals_read != y_vals_read) {
      *errstr = "read unequals xs and ys";
      return -1;
    }
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "reading files: %ld ns\n", elapsed);
    last_tp = tp;
#endif

    already_read += x_vals_read;

    count_vals(x_vals_read, counts, xs, x_nulls, x_min, x_width, x_count, ys, y_nulls, y_min, y_width, y_count);
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "counting vals: %ld ns\n", elapsed);
    last_tp = tp;
#endif
  }

  return 0;
}
