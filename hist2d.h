int build_histogram(int x_fd, int x_nulls_fd, float8 x_min, float8 x_width, int32 x_count,
                    int y_fd, int y_nulls_fd, float8 y_min, float8 y_width, int32 y_count,
                    int64 *counts, char **errstr);
