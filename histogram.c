/**
 * histogram.c - We want to keep most of the work
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

#include "histogram.h"

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

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
static int load_dimension(int already_read, int vals_fd, int nulls_fd, float8 *vals, bool *nulls, ssize_t max_vals_to_read, char **errstr) {
  ssize_t bytes_read;
  int vals_read;

  max_vals_to_read = min(max_vals_to_read, HIST_BUFFER);

  bytes_read = read(vals_fd, vals, max_vals_to_read*sizeof(float8));
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

  bytes_read = read(nulls_fd, nulls, max_vals_to_read*sizeof(bool));
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

static void count_vals(int more_vals, int64 *counts, float8 *xs, bool *x_nulls, float8 x_min, float8 x_width, int x_count) {
  size_t i;
  float8 x;
  float8 x_pos;

  for (i = 0; i < more_vals; i += 1) {
    if (x_nulls[i]) continue;
    x = xs[i];

    x_pos = (x - x_min) / x_width;

    if (x_pos >= 0 && x_pos < x_count) {
      counts[(int)x_pos] += 1;
    }
  }
}

static void count_vals_2d(int more_vals, int64 *counts, float8 *xs, bool *x_nulls, float8 x_min, float8 x_width, int x_count, float8 *ys, bool *y_nulls, float8 y_min, float8 y_width, int y_count) {
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
                    int64 *counts, char **errstr) {
  // TODO: int64 or int32 depending....
  float8 xs[HIST_BUFFER];
  bool x_nulls[HIST_BUFFER];
  int already_read = 0, x_vals_read;
#ifdef PROFILING
  struct timespec last_tp, tp;
  long elapsed;
#endif

#ifdef PROFILING
  fprintf(stderr, "another run\n");
  if (clock_gettime(CLOCK_MONOTONIC, &last_tp)) { perror("clock failed"); exit(1); }
#endif
  while ((x_vals_read = load_dimension(already_read, x_fd, x_nulls_fd, xs, x_nulls, HIST_BUFFER, errstr))) {
    if (x_vals_read == -1) return -1;   // errstr is already set

#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "reading files: %ld ns\n", elapsed);
    last_tp = tp;
#endif

    already_read += x_vals_read;

    count_vals(x_vals_read, counts, xs, x_nulls, x_min, x_width, x_count);
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "counting vals: %ld ns\n", elapsed);
    last_tp = tp;
#endif
  }

  return 0;
}

int build_histogram_with_bounds(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                    int64 *counts, ssize_t min_pos, ssize_t max_pos, char **errstr) {
  // TODO: int64 or int32 depending....
  float8 xs[HIST_BUFFER];
  bool x_nulls[HIST_BUFFER];
  int already_read = 0, x_vals_read;
  ssize_t max_vals_to_read;
#ifdef PROFILING
  struct timespec last_tp, tp;
  long elapsed;
#endif


#ifdef PROFILING
  fprintf(stderr, "another run\n");
  if (clock_gettime(CLOCK_MONOTONIC, &last_tp)) { perror("clock failed"); exit(1); }
#endif

  lseek(x_nulls_fd, min_pos * sizeof(bool),   SEEK_SET);
  lseek(x_fd,       min_pos * sizeof(float8), SEEK_SET);

  max_vals_to_read = max_pos - min_pos + 1;
  while (max_vals_to_read > 0 &&
         (x_vals_read = load_dimension(already_read, x_fd, x_nulls_fd, xs, x_nulls, max_vals_to_read, errstr))) {
    if (x_vals_read == -1) return -1;   // errstr is already set

#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "reading files: %ld ns\n", elapsed);
    last_tp = tp;
#endif

    already_read += x_vals_read;
    max_vals_to_read -= x_vals_read;

    count_vals(x_vals_read, counts, xs, x_nulls, x_min, x_width, x_count);
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "counting vals: %ld ns\n", elapsed);
    last_tp = tp;
#endif
  }

  return 0;
}

/**
 * find_bounds_start_end - returns the start/stop file positions of values within the given range.
 * Used to limit what is included in a histogram.
 * If everything is less than the requested min_t, then min_pos will be -1.
 * If everything is greater than the requested max_t, then max_pos will be -1.
 * So if either of those parameters come back as -1, then no values are in range.
 */
int find_bounds_start_end(int t_fd, int t_nulls_fd, float min_t, float max_t, ssize_t *min_pos, ssize_t *max_pos, char **errstr) {
  float8 ts[HIST_BUFFER];
  bool t_nulls[HIST_BUFFER];
  int already_read = 0, t_vals_read;
  size_t i;
  float8 t;
  bool found_start = false;

  *min_pos = -1;
  *max_pos = -1;
  while ((t_vals_read = load_dimension(already_read, t_fd, t_nulls_fd, ts, t_nulls, HIST_BUFFER, errstr))) {
    if (t_vals_read == -1) return -1;   // errstr is already set

    for (i = 0; i < t_vals_read; i += 1) {
      if (t_nulls[i]) continue;
      t = ts[i];

      if (!found_start) {
        if (t >= min_t) {
          *min_pos = already_read + i;
          found_start = true;
        }
      }
      if (t > max_t) {
        *max_pos = already_read + i - 1;  // could be -1
        return 0;
      }
    }

    already_read += t_vals_read;
  }

  *max_pos = already_read;
  return 0;
}

int build_histogram_2d(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
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
  while ((x_vals_read = load_dimension(already_read, x_fd, x_nulls_fd, xs, x_nulls, HIST_BUFFER, errstr))) {
    if (x_vals_read == -1) return -1;   // errstr is already set

    y_vals_read = load_dimension(already_read, y_fd, y_nulls_fd, ys, y_nulls, HIST_BUFFER, errstr);
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

    count_vals_2d(x_vals_read, counts, xs, x_nulls, x_min, x_width, x_count, ys, y_nulls, y_min, y_width, y_count);
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "counting vals: %ld ns\n", elapsed);
    last_tp = tp;
#endif
  }

  return 0;
}

int build_histogram_2d_with_bounds(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                                   int y_fd, int y_nulls_fd, float8 y_min, float8 y_width, int32 y_count,
                                   int64 *counts, ssize_t min_pos, ssize_t max_pos, char **errstr) {
  // TODO: int64 or int32 depending....
  float8 xs[HIST_BUFFER];
  float8 ys[HIST_BUFFER];
  bool x_nulls[HIST_BUFFER];
  bool y_nulls[HIST_BUFFER];
  int already_read = 0, x_vals_read, y_vals_read;
  ssize_t max_vals_to_read;
#ifdef PROFILING
  struct timespec last_tp, tp;
  long elapsed;
#endif

#ifdef PROFILING
  fprintf(stderr, "another run\n");
  if (clock_gettime(CLOCK_MONOTONIC, &last_tp)) { perror("clock failed"); exit(1); }
#endif
  lseek(x_nulls_fd, min_pos * sizeof(bool),   SEEK_SET);
  lseek(x_fd,       min_pos * sizeof(float8), SEEK_SET);
  lseek(y_nulls_fd, min_pos * sizeof(bool),   SEEK_SET);
  lseek(y_fd,       min_pos * sizeof(float8), SEEK_SET);

  max_vals_to_read = max_pos - min_pos + 1;
  while (max_vals_to_read > 0 &&
         (x_vals_read = load_dimension(already_read, x_fd, x_nulls_fd, xs, x_nulls, max_vals_to_read, errstr))) {
    if (x_vals_read == -1) return -1;   // errstr is already set

    y_vals_read = load_dimension(already_read, y_fd, y_nulls_fd, ys, y_nulls, max_vals_to_read, errstr);
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
    max_vals_to_read -= x_vals_read;

    count_vals_2d(x_vals_read, counts, xs, x_nulls, x_min, x_width, x_count, ys, y_nulls, y_min, y_width, y_count);
#ifdef PROFILING
    if (clock_gettime(CLOCK_MONOTONIC, &tp)) { perror("clock failed"); exit(1); }
    elapsed = 1000000000*(tp.tv_sec - last_tp.tv_sec) + (tp.tv_nsec - last_tp.tv_nsec);
    fprintf(stderr, "counting vals: %ld ns\n", elapsed);
    last_tp = tp;
#endif
  }

  return 0;
}
