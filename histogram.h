int find_bounds_start_end(int t_fd, int t_nulls_fd, float min_t, float max_t, ssize_t *min_pos, ssize_t *max_pos, char **errstr);

int build_histogram(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                    int64 *counts, char **errstr);

int build_histogram_2d(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                       int y_fd, int y_nulls_fd, float8 y_min, float8 y_width, int32 y_count,
                       int64 *counts, char **errstr);

int build_histogram_with_bounds(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                                int64 *counts, ssize_t min_pos, ssize_t max_pos, char **errstr);

int build_histogram_2d_with_bounds(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                                   int y_fd, int y_nulls_fd, float8 y_min, float8 y_width, int32 y_count,
                                   int64 *counts, ssize_t min_pos, ssize_t max_pos, char **errstr);
