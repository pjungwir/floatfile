// Support for large files:
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>

#include <postgres.h>
#include <fmgr.h>
#include <pg_config.h>
#include <miscadmin.h>
#include <utils/array.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <catalog/pg_type.h>
#include <utils/builtins.h>

PG_MODULE_MAGIC;

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))
#define GET_TEXT(cstrp) DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(cstrp)))

#define FLOATFILE_MAX_PATH 255
#define FLOATFILE_PREFIX "floatfile"
#define FLOATFILE_PREFIX_LEN sizeof FLOATFILE_PREFIX
#define FLOATFILE_NULLS_SUFFIX  'n'
#define FLOATFILE_FLOATS_SUFFIX 'v'

#ifndef FLOATFILE_LOCK_PREFIX
#define FLOATFILE_LOCK_PREFIX 0xF107F11E
#endif

#ifndef MINIMUM_SANE_DATA_DIR
#define MINIMUM_SANE_DATA_DIR 3
#endif

// Datums can be eight or four bytes wide, depending on the machine.
// If they are eight wide, then float8s are pass-by-value.
// In that case an array of float8s and an array of Datums
// are the exact same bits.
// We can save a lot of looping and copying
// if we just re-interpret the float8s as Datums.
// On my own desktop, the loop adds about 300ms
// to load an array with 13 million elements
// (from 150ms to 450ms).
// This is a bit of hack from the sound of
// https://mailinglist-archive.mojah.be/pgsql-general/2017-09/msg00453.php
// but it works for Linux and Mac on x86,
// and should work anywhere as far as I can tell.
// I suppose if there are ever 16-byte-wide Datums
// we'll have a problem, but there aren't today.
#define SAFE_TO_CAST_FLOATS_AND_DATUMS FLOAT8PASSBYVAL
// #define SAFE_TO_CAST_FLOATS_AND_DATUMS false

static void validate_target_filename(const char *filename);
static void floatfile_root_path(const char *tablespace, char *path, int path_len);
static int mkdirs_for_floatfile(const char *root, const char *path);
static int floatfile_filename_to_full_path(const char *tablespace, const char *filename, char *path, int path_len);
static int load_file_to_floats(const char *tablespace, const char *filename, float8** vals, bool** nulls);
static int save_file_from_floats(const char *tablespace, const char *filename, float8* vals, bool* nulls, int array_len);
static int extend_file_from_floats(const char *tablespace, const char *filename, float8* vals, bool* nulls, int array_len);
static int32 hash_filename(const char *filename);



static void floatfile_root_path(const char *tablespace, char *path, int path_len) {
  int chars_wrote;
  const char *root_directory;

  if (tablespace) {
    // TODO tablespaces not supported yet
    elog(ERROR, "Tablespaces not supported yet");
  } else {
    // GetConfigOption returns a static buffer,
    // but since you can't change data_directory without restarting the server,
    // it is safe to use.
    root_directory = GetConfigOption("data_directory", false, true);
    // Be a little paranoid:
    if (root_directory[0] != '/') elog(ERROR, "data_directory is not an absolute path");
    if (strlen(root_directory) < MINIMUM_SANE_DATA_DIR) elog(ERROR, "data_directory is too short");

    chars_wrote = snprintf(path, path_len, "%s", root_directory);
    if (chars_wrote == -1 || chars_wrote >= path_len) elog(ERROR, "floatfile root path was too long");
  }
}



static void validate_target_filename(const char *filename) {
  // Very simple filename validation for now:
  if (strlen(filename) == 0)  ereport(ERROR, (errmsg("floatfile filename can't be empty")));
  if (strstr(filename, "..")) ereport(ERROR, (errmsg("floatfile filename can't contain ..")));
  if (filename[0] == '/')     ereport(ERROR, (errmsg("floatfile filename can't start with /")));
}



static void floatfile_relative_target_path(const char *filename, char *path, int path_len) {
  int chars_wrote = snprintf(path, path_len, "%s/%d/%s.n", FLOATFILE_PREFIX, MyDatabaseId, filename);
  if (chars_wrote == -1 || chars_wrote >= path_len) elog(ERROR, "floatfile relative path was too long");
}

/**
 * floatfile_filename_to_full_path - Converts a user-supplied filename to a full path.
 *
 * The path starts with either the data directory or the tablespace path,
 * and then we add `floatfile`, the current database name, and `filename`.
 *
 * The result ends with `.n`, pointing to the nulls file.
 * You can change the last char to a `v` to get the vals file.
 *
 * Parameters:
 *
 *   `tablespace` - The name of the tablespace to use,
 *                  or `NULL` to store the file in the data directory.
 *   `filename` - The path relative to our prefix to name the file.
 *   `path` - Buffer to hold the result.
 *   `path_len` - Length of `path`.
 *
 * Returns:
 *
 *   The length of the full path (excluding the null byte).
 *   We use elog and ereport for errors so if it returns at all it worked.
 */
static int floatfile_filename_to_full_path(
        const char *tablespace,
        const char *filename,
        char *path_buf,
        int path_len) {
  char root_directory[FLOATFILE_MAX_PATH + 1],
       relative_target[FLOATFILE_MAX_PATH + 1];
  int chars_wrote;

  floatfile_root_path(tablespace, root_directory, FLOATFILE_MAX_PATH + 1);
  floatfile_relative_target_path(filename, relative_target, FLOATFILE_MAX_PATH + 1);

  chars_wrote = snprintf(path_buf, path_len, "%s/%s", root_directory, relative_target);
  if (chars_wrote == -1 || chars_wrote >= path_len) elog(ERROR, "floatfile full path was too long");
  return chars_wrote;
}



/**
 * load_file_to_floats - Opens `filename` and reads the null flags and float values.
 *
 * Allocates space for flags and floats.
 * Returns the length of the array on success or -1 on failure (and sets errno).
 */
static int load_file_to_floats(const char *tablespace, const char *filename, float8** vals, bool** nulls) {
  char path[FLOATFILE_MAX_PATH + 1];
  int pathlen;
  int fd;
  struct stat fileinfo;
  ssize_t bytes_read;
  int array_len;
  int err;

  validate_target_filename(filename);
  pathlen = floatfile_filename_to_full_path(tablespace, filename, path, FLOATFILE_MAX_PATH + 1);


  // Load null flags:

  // path[pathlen - 1] = FLOATFILE_NULLS_SUFFIX;
  fd = open(path, O_RDONLY);
  if (fd == -1) return -1;

  if (fstat(fd, &fileinfo)) goto bail;
  array_len = fileinfo.st_size / sizeof(bool);

  *nulls = palloc(fileinfo.st_size);
  // palloc never returns NULL but calls elog to fail
  
  // TODO: Is mmap any faster? Anything else? Benchmark it for Mac and Linux!
  bytes_read = read(fd, *nulls, fileinfo.st_size);
  if (bytes_read != fileinfo.st_size) goto bail;

  if (close(fd)) return -1;


  // Load floats:

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  fd = open(path, O_RDONLY);
  if (fd == -1) return -1;

  if (fstat(fd, &fileinfo)) goto bail;
  if (array_len * sizeof(float8) != fileinfo.st_size) {
    close(fd);
    elog(ERROR, "floatfile found inconsistent file sizes: %d vs %lld", array_len, (long long int)fileinfo.st_size);
  }

  *vals = palloc(fileinfo.st_size);
  // palloc never returns NULL but calls elog to fail

  bytes_read = read(fd, *vals, fileinfo.st_size);
  if (bytes_read != fileinfo.st_size) goto bail;

  if (close(fd)) return -1;


  return array_len;


bail:
  err = errno;
  close(fd);    // Ignore the error since we've already seen one.
  errno = err;
  return -1;
}



/**
 * Makes the container directories if necessary.
 *
 * Parameters:
 *
 * `root` - We preface everything with this and just assume that it exists.
 * `path` - Create the `basename` of this inside `root` if it isn't there already.
 *
 * We want to support all these use cases:
 *
 *  root                      | path
 * ---------------------------|----------
 *  $data_dir                 | floatfile/12345/
 *  $data_dir/floatfile/12345 | foo.n
 *  $data_dir/floatfile/12345 | bar/foo.n
 *  $data_dir/floatfile/12345 | bar/baz/foo.n
 *
 * So we find the last `/` in `path` and create all the dirs up to there.
 *
 * Note this function trusts its inputs, so validate first!
 * (E.g. don't start `path` with a slash.)
 */
static int mkdirs_for_floatfile(const char *root, const char *path) {
  char mypath[FLOATFILE_MAX_PATH + 1];
  char *pos, *lastpos;
  int rootfd;
  int result;

  rootfd = open(root, O_RDONLY);
  if (rootfd == -1) return -1;

  strncpy(mypath, path, FLOATFILE_MAX_PATH + 1);
  lastpos = mypath;
  while ((pos = strchr(lastpos, '/'))) {
    *pos = '\0';
    result = mkdirat(rootfd, mypath, S_IRUSR | S_IWUSR | S_IXUSR);
    if (result == -1 && errno != EEXIST) {
      result = errno;
      close(rootfd); // ignore error
      errno = result;
      return -1;
    }
    lastpos = pos + 1;
    *pos = '/';
  }

  return close(rootfd);
}

/**
 * save_file_from_floats - Writes the null flags and float vals to their (new) files.
 *
 * `filename` should end with FLOATFILE_NULLS_SUFFIX.
 *
 * Returns 0 on success or -1 on failure (and sets errno).
 */
static int save_file_from_floats(const char *tablespace, const char *filename, float8* vals, bool* nulls, int array_len) {
  char root_directory[FLOATFILE_MAX_PATH + 1],
       relative_target[FLOATFILE_MAX_PATH + 1];
  char path[FLOATFILE_MAX_PATH + 1];
  int pathlen;
  int fd;
  ssize_t bytes_written;
  int err;

  validate_target_filename(filename);
  floatfile_root_path(tablespace, root_directory, FLOATFILE_MAX_PATH + 1);
  floatfile_relative_target_path(filename, relative_target, FLOATFILE_MAX_PATH + 1);

  mkdirs_for_floatfile(root_directory, relative_target);

  pathlen = floatfile_filename_to_full_path(tablespace, filename, path, FLOATFILE_MAX_PATH + 1);

  // Save the nulls:

  // path[pathlen - 1] = FLOATFILE_NULLS_SUFFIX;
  fd = open(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) return -1;

  bytes_written = write(fd, nulls, array_len * sizeof(bool));
  if (bytes_written != array_len * sizeof(bool)) goto bail;

  // TODO: fsync before closing?
  if (close(fd)) return -1;


  // Save the floats:

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  fd = open(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) return -1;

  bytes_written = write(fd, vals, array_len * sizeof(float8));
  if (bytes_written != array_len * sizeof(float8)) goto bail;

  if (close(fd)) return -1;

  return EXIT_SUCCESS;

bail:
  err = errno;
  close(fd);    // Ignore the error since we've already seen one.
  errno = err;
  return -1;
}



/**
 * extend_file_from_floats - Appends the null flags and float vals to their (existing) files.
 *
 * `filename` should end with FLOATFILE_NULLS_SUFFIX.
 *
 * Returns 0 on success or -1 on failure (and sets errno).
 */
static int extend_file_from_floats(const char *tablespace, const char *filename, float8* vals, bool* nulls, int array_len) {
  char path[FLOATFILE_MAX_PATH + 1];
  int pathlen;
  int fd;
  ssize_t bytes_written;
  int err;

  validate_target_filename(filename);
  pathlen = floatfile_filename_to_full_path(tablespace, filename, path, FLOATFILE_MAX_PATH + 1);


  // Save the nulls:

  // path[pathlen - 1] = FLOATFILE_NULLS_SUFFIX;
  fd = open(path, O_WRONLY | O_APPEND);
  if (fd == -1) return -1;

  bytes_written = write(fd, nulls, array_len * sizeof(bool));
  if (bytes_written != array_len * sizeof(bool)) goto bail;

  if (close(fd)) return -1;


  // Save the floats:

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  fd = open(path, O_WRONLY | O_APPEND);
  if (fd == -1) return -1;

  bytes_written = write(fd, vals, array_len * sizeof(float8));
  if (bytes_written != array_len * sizeof(float8)) goto bail;

  if (close(fd)) return -1;

  return EXIT_SUCCESS;

bail:
  err = errno;
  close(fd);    // Ignore the error since we've already seen one.
  errno = err;
  return -1;
}



/**
 * hash_filename - Returns an integer hash of the given filename,
 * suitable for taking an advisory lock on that file.
 *
 * We use the two-int32 versions of pg_advisory_lock,
 * passing FLOATFILE_LOCK_PREFIX for the first argument
 * (which you can override when you build this extension)
 * and the result of this function for the second.
 *
 * Unfortunately collisions are going to be unavoidable,
 * but the only consequence is a bit more lock contention.
 * (There should be no added possibility of deadlocks,
 * since we take and release the lock in the same function call.)
 * Consider that we have 2^32 (4294967296) possibilities.
 * According to https://en.wikipedia.org/wiki/Birthday_problem
 * the odds of a collision p(n;d) given d hashes and n tables is
 *
 *     p(n;d) \approx 1 - ( \frac{d - 1}{d} )^{n(n - 1) / 2}
 *
 * So we get these results:
 *
 *     n tables | p(n;d)
 *   -----------|----------
 *        1,000 | 0.00011629214406294608
 *       10,000 | 0.011572881058428464
 *      100,000 | 0.6878094613810533
 *    1,000,000 | 1.0
 *
 * Actually if our hash function is not perfectly uniform
 * things will be worse, so we try to get close to uniform.
 *
 * For the actual hash function we use djb2,
 * described at http://www.cse.yorku.ca/~oz/hash.html
 */
static int32 hash_filename(const char *filename) {
  unsigned long h = 5381;
  int c;

  while ((c = *filename++)) {
    h = ((h << 5) + h) + c;
  }

  // Take the bottom 32 bits and treat it as an int32:
  // I haven't given much thought to whether this works outside of x86,
  // but I guess it doesn't really matter which 32 bits we wind up taking.
  return (int32)(h & 0xffffffff);
}



Datum load_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(load_floatfile);
/**
 * load_floatfile - Loads an array of floats from a given file.
 *
 * Parameters:
 *
 *   `file` - the name of the file, relative to the data dir + our prefix.
 */
Datum
load_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  char *errstr = NULL;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  ArrayType *result = NULL;
  int dims[1];
  int lbs[1];
  int i;

  if (PG_ARGISNULL(0)) PG_RETURN_NULL();
  filename_arg = PG_GETARG_TEXT_P(0);

  // TODO: DRY this up with the tablespace version:

  filename = GET_STR(filename_arg);
  filename_hash = hash_filename(filename);

  // We use Postgres advisory locks instead of POSIX file locking
  // so that our locks mesh well with other Postgres locking:
  // they show up in pg_locks and we get free deadlock detection.
  DirectFunctionCall2(pg_advisory_lock_shared_int4, FLOATFILE_LOCK_PREFIX, filename_hash);

  arrlen = load_file_to_floats(NULL, filename, &floats, &nulls);
  if (arrlen < 0) {
    errstr = strerror(errno);
    goto quit;
  }
  
  // I don't think we can use the preprocessor here
  // because it is defined as `true` aka `((bool) 1)`,
  // and the preprocessor doesn't know what a `bool` is yet:
  if (SAFE_TO_CAST_FLOATS_AND_DATUMS) {
    datums = (Datum *)floats;
  } else {
    datums = palloc(arrlen * sizeof(Datum));
    for (i = 0; i < arrlen; i++) {
      datums[i] = Float8GetDatum(floats[i]);
    }
  }

  get_typlenbyvalalign(FLOAT8OID, &floatTypeWidth, &floatTypeByValue, &floatTypeAlignmentCode);
  dims[0] = arrlen;
  lbs[0] = 1;
  result = construct_md_array(datums, nulls, 1, dims, lbs, FLOAT8OID, floatTypeWidth, floatTypeByValue, floatTypeAlignmentCode);

quit:
  DirectFunctionCall2(pg_advisory_unlock_shared_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
  if (errstr) elog(ERROR, "%s", errstr);

  PG_RETURN_ARRAYTYPE_P(result);
}



Datum load_floatfile_from_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(load_floatfile_from_tablespace);
/**
 * load_floatfile_from_tablespace - Loads an array of floats from a given file located in the tablespace.
 *
 * Parameters:
 *
 *   `tablespace` - the name of the tablespace where the file is to be found.
 *   `file` - the name of the file, relative to the tablespace's directory + our prefix.
 */
Datum
load_floatfile_from_tablespace(PG_FUNCTION_ARGS)
{
  // TODO
  PG_RETURN_NULL();
}



Datum save_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(save_floatfile);
/**
 * save_floatfile - Saves an array of floats to a file in the data directory.
 *
 * Parameters:
 *   `filename` - The name of the file to use. Must not already exist!
 *   `vals` - The array of floats to save.
 *
 * Returns:
 *
 *   0 on success
 *   E TODO if the file already exists.
 *   any errors opening/writing/closing the file.
 */
Datum
save_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  char *errstr = NULL;
  ArrayType *vals;
  Oid valsType;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  int i;

  if (PG_ARGISNULL(0)) PG_RETURN_VOID();
  if (PG_ARGISNULL(1)) PG_RETURN_VOID();

  filename_arg = PG_GETARG_TEXT_P(0);

  // TODO: DRY this up with the tablespace version:

  filename = GET_STR(filename_arg);
  filename_hash = hash_filename(filename);

  vals = PG_GETARG_ARRAYTYPE_P(1);
  if (ARR_NDIM(vals) > 1) {
    ereport(ERROR, (errmsg("One-dimesional arrays are required")));
  }
  valsType = ARR_ELEMTYPE(vals);
  if (valsType != FLOAT8OID) {
    ereport(ERROR, (errmsg("save_floatfile takes an array of DOUBLE PRECISION values")));
  }
  get_typlenbyvalalign(FLOAT8OID, &floatTypeWidth, &floatTypeByValue, &floatTypeAlignmentCode);
  deconstruct_array(vals, FLOAT8OID, floatTypeWidth, floatTypeByValue, floatTypeAlignmentCode,
&datums, &nulls, &arrlen);

  if (SAFE_TO_CAST_FLOATS_AND_DATUMS) {
    floats = (float8 *)datums;
  } else {
    floats = palloc(arrlen * sizeof(float8));
    for (i = 0; i < arrlen; i++) {
      floats[i] = DatumGetFloat8(datums[i]);
    }
  }

  DirectFunctionCall2(pg_advisory_lock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);

  if (save_file_from_floats(NULL, filename, floats, nulls, arrlen)) {
    errstr = strerror(errno);
    goto quit;
  }

  // Nothing left to do actually!

quit:
  DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
  if (errstr) elog(ERROR, "%s", errstr);

  PG_RETURN_VOID();
}



Datum save_floatfile_in_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(save_floatfile_in_tablespace);
/**
 * save_floatfile_in_tablespace - Saves an array of floats to a file in the tablespace.
 * TODO
 */
Datum
save_floatfile_in_tablespace(PG_FUNCTION_ARGS)
{
  PG_RETURN_VOID();
}



Datum extend_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(extend_floatfile);
/**
 * extend_floatfile - Appends to an existing floatfile file in the data directory.
 * TODO
 */
Datum
extend_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  char *errstr = NULL;
  ArrayType *vals;
  Oid valsType;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  int i;

  if (PG_ARGISNULL(0)) PG_RETURN_VOID();
  if (PG_ARGISNULL(1)) PG_RETURN_VOID();

  filename_arg = PG_GETARG_TEXT_P(0);

  // TODO: DRY this up with the tablespace version, and maybe the save_ functions too:

  filename = GET_STR(filename_arg);
  filename_hash = hash_filename(filename);

  vals = PG_GETARG_ARRAYTYPE_P(1);
  if (ARR_NDIM(vals) > 1) {
    ereport(ERROR, (errmsg("One-dimesional arrays are required")));
  }
  valsType = ARR_ELEMTYPE(vals);
  if (valsType != FLOAT8OID) {
    ereport(ERROR, (errmsg("save_floatfile takes an array of DOUBLE PRECISION values")));
  }
  get_typlenbyvalalign(FLOAT8OID, &floatTypeWidth, &floatTypeByValue, &floatTypeAlignmentCode);
  deconstruct_array(vals, FLOAT8OID, floatTypeWidth, floatTypeByValue, floatTypeAlignmentCode,
&datums, &nulls, &arrlen);

  if (SAFE_TO_CAST_FLOATS_AND_DATUMS) {
    floats = (float8 *)datums;
  } else {
    floats = palloc(arrlen * sizeof(float8));
    for (i = 0; i < arrlen; i++) {
      floats[i] = DatumGetFloat8(datums[i]);
    }
  }

  DirectFunctionCall2(pg_advisory_lock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);

  if (extend_file_from_floats(NULL, filename, floats, nulls, arrlen)) {
    errstr = strerror(errno);
    goto quit;
  }

  // Nothing left to do actually!

quit:
  // TODO: Probably need to catch and reraise instead:
  DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
  if (errstr) elog(ERROR, "%s", errstr);

  PG_RETURN_VOID();
}



Datum extend_floatfile_in_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(extend_floatfile_in_tablespace);
/**
 * extend_floatfile_in_tablespace - Appends to an existing floatfile file in the tablespace.
 * TODO
 */
Datum
extend_floatfile_in_tablespace(PG_FUNCTION_ARGS) {
  PG_RETURN_VOID();
}