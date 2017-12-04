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
#include <utils/acl.h>
#include <utils/lsyscache.h>
#include <utils/builtins.h>
#include <catalog/pg_type.h>
#include <catalog/catalog.h>
#include <catalog/pg_tablespace.h>
#include <commands/tablespace.h>


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

#include "hist2d.h"

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
// http://www.postgresql-archive.org/Is-float8-a-reference-type-td5984977.html
// but it works for Linux and Mac on x86,
// and should work anywhere as far as I can tell.
// I suppose if there are ever 16-byte-wide Datums
// we'll have a problem, but there aren't today.
#define SAFE_TO_CAST_FLOATS_AND_DATUMS FLOAT8PASSBYVAL
// #define SAFE_TO_CAST_FLOATS_AND_DATUMS false



static void floatfile_root_path(const char *tablespace, char *path, int path_len) {
  int chars_wrote;
  const char *root_directory;
  Oid tablespace_oid;
  char *tablespace_location;

  // If tablespace is NULL then use the default tablespace:
  if (tablespace) {
    tablespace_oid = get_tablespace_oid(tablespace, false);
  } else {
    tablespace_oid = InvalidOid;  // Used by pg_tablespace_location to indicate the default tablespace.
  }

  // Permissions check: Follow the logic in DefineRelation
  // from src/backend/commands/tablecmds.c:
  if (OidIsValid(tablespace_oid) && tablespace_oid != MyDatabaseTableSpace) {
    AclResult aclresult;
    aclresult = pg_tablespace_aclcheck(tablespace_oid, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK) {
      aclcheck_error(aclresult, ACL_KIND_TABLESPACE, get_tablespace_name(tablespace_oid));
    }
  }
  if (tablespace_oid == GLOBALTABLESPACE_OID) {
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("only shared relations can be placed in pg_global tablespace")));
  }

  tablespace_location = DatumGetCString(DirectFunctionCall1(textout,
                          DirectFunctionCall1(pg_tablespace_location, ObjectIdGetDatum(tablespace_oid))));

  if (strcmp(tablespace_location, "") == 0) {
    // We set restrict_superuser to false here
    // because we aren't going to share the location with the user:
    root_directory = GetConfigOption("data_directory", false, false);
    // Be a little paranoid:
    if (root_directory[0] != '/') elog(ERROR, "data_directory is not an absolute path");
    if (strlen(root_directory) < MINIMUM_SANE_DATA_DIR) elog(ERROR, "data_directory is too short");

    chars_wrote = snprintf(path, path_len, "%s", root_directory);
    if (chars_wrote == -1 || chars_wrote >= path_len) elog(ERROR, "floatfile root path was too long");

  } else {
    chars_wrote = snprintf(path, path_len, "%s/%s", tablespace_location, TABLESPACE_VERSION_DIRECTORY);
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
  // TODO: Should test this on large files,
  // and maybe cap the buffer at something reasonable.
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
 * Cleans up the container directories if necessary.
 *
 * Parameters:
 *
 * `root` - What not to delete.
 * `path` - Start with the `basename` of this and keep deleting
 *          until we get to `root` or can't remove something
 *          because it has other files.
 *
 * We want to support all these use cases:
 *
 *  root      | path
 * -----------|------------
 *  $data_dir | floatfile/12345/foo.n
 *  $data_dir | floatfile/12345/bar/foo.n
 *  $data_dir | floatfile/12345/bar/baz/foo.n
 *
 * So we find the last `/` in `path` and delete all the dirs down.
 */
static int rmdirs_for_floatfile(const char *root, const char *path) {
  char mypath[FLOATFILE_MAX_PATH + 1];
  char *pos;
  int rootfd;
  int result;

  rootfd = open(root, O_RDONLY);
  if (rootfd == -1) return -1;

  strncpy(mypath, path, FLOATFILE_MAX_PATH + 1);

  while ((pos = strrchr(mypath, '/'))) {
    *pos = '\0';
    result = unlinkat(rootfd, mypath, AT_REMOVEDIR);
    if (result == -1) {
      if (errno == ENOTEMPTY) {
        break;  // All done!
      } else {
        result = errno;
        close(rootfd); // ignore error
        errno = result;
        return -1;
      }
    }
  }

  return close(rootfd);
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
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd == -1) return -1;

  bytes_written = write(fd, nulls, array_len * sizeof(bool));
  if (bytes_written != array_len * sizeof(bool)) goto bail;

  // TODO: fsync before closing?
  if (close(fd)) return -1;


  // Save the floats:

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
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
  fd = open(path, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) return -1;

  bytes_written = write(fd, nulls, array_len * sizeof(bool));
  if (bytes_written != array_len * sizeof(bool)) goto bail;

  if (close(fd)) return -1;


  // Save the floats:

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  fd = open(path, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
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


static ArrayType *_load_floatfile(const char *tablespace, const char *filename) {
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  ArrayType *result = NULL;
  int dims[1];
  int lbs[1];
  int i;

  filename_hash = hash_filename(filename);

  // We use Postgres advisory locks instead of POSIX file locking
  // so that our locks mesh well with other Postgres locking:
  // they show up in pg_locks and we get free deadlock detection.
  DirectFunctionCall2(pg_advisory_lock_shared_int4, FLOATFILE_LOCK_PREFIX, filename_hash);

  PG_TRY();
  {
    arrlen = load_file_to_floats(tablespace, filename, &floats, &nulls);
    if (arrlen < 0) {
      ereport(ERROR, (errmsg("Failed to load floatfile %s: %s", filename, strerror(errno))));
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
  }
  PG_CATCH();
  {
    DirectFunctionCall2(pg_advisory_unlock_shared_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
    PG_RE_THROW();
  }
  PG_END_TRY();

  DirectFunctionCall2(pg_advisory_unlock_shared_int4, FLOATFILE_LOCK_PREFIX, filename_hash);

  return result;
}



Datum load_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(load_floatfile);
/**
 * load_floatfile - Loads an array of floats from a given file.
 *
 * Parameters:
 *
 *   `file` - the name of the file, relative to the default tablespace + our prefix.
 */
Datum
load_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;

  if (PG_ARGISNULL(0)) PG_RETURN_NULL();
  filename_arg = PG_GETARG_TEXT_P(0);

  filename = GET_STR(filename_arg);
  PG_RETURN_ARRAYTYPE_P(_load_floatfile(NULL, filename));
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
  text *tablespace_arg;
  char *tablespace;
  text *filename_arg;
  char *filename;

  if (PG_ARGISNULL(0)) {
    tablespace = NULL;
  } else {
    tablespace_arg = PG_GETARG_TEXT_P(0);
    tablespace = GET_STR(tablespace_arg);
  }

  if (PG_ARGISNULL(1)) PG_RETURN_NULL();
  filename_arg = PG_GETARG_TEXT_P(1);
  filename = GET_STR(filename_arg);

  PG_RETURN_ARRAYTYPE_P(_load_floatfile(tablespace, filename));
}



static void _save_floatfile(const char *tablespace, const char *filename, ArrayType *vals) {
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  Oid valsType;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  int i;

  filename_hash = hash_filename(filename);

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
  PG_TRY();
  {
    if (save_file_from_floats(tablespace, filename, floats, nulls, arrlen)) {
      ereport(ERROR, (errmsg("Failed to save floatfile %s: %s", filename, strerror(errno))));
    }
  }
  PG_CATCH();
  {
    DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
    PG_RE_THROW();
  }
  PG_END_TRY();

  DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
}

Datum save_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(save_floatfile);
/**
 * save_floatfile - Saves an array of floats to a file in the data directory.
 *
 * Parameters:
 *   `filename` - The name of the file to use. Must not already exist!
 *   `vals` - The array of floats to save.
 */
Datum
save_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;
  ArrayType *vals;

  if (PG_ARGISNULL(0)) PG_RETURN_VOID();
  if (PG_ARGISNULL(1)) PG_RETURN_VOID();

  filename_arg = PG_GETARG_TEXT_P(0);
  filename = GET_STR(filename_arg);

  vals = PG_GETARG_ARRAYTYPE_P(1);

  _save_floatfile(NULL, filename, vals);

  PG_RETURN_VOID();
}



Datum save_floatfile_in_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(save_floatfile_in_tablespace);
/**
 * save_floatfile_in_tablespace - Saves an array of floats to a file in the tablespace.
 */
Datum
save_floatfile_in_tablespace(PG_FUNCTION_ARGS)
{
  text *tablespace_arg;
  char *tablespace;
  text *filename_arg;
  char *filename;
  ArrayType *vals;

  if (PG_ARGISNULL(1)) PG_RETURN_VOID();
  if (PG_ARGISNULL(2)) PG_RETURN_VOID();

  if (PG_ARGISNULL(0)) {
    tablespace = NULL;
  } else {
    tablespace_arg = PG_GETARG_TEXT_P(0);
    tablespace = GET_STR(tablespace_arg);
  }

  filename_arg = PG_GETARG_TEXT_P(1);
  filename = GET_STR(filename_arg);

  vals = PG_GETARG_ARRAYTYPE_P(2);

  _save_floatfile(tablespace, filename, vals);

  PG_RETURN_VOID();
}



static void _extend_floatfile(const char *tablespace, const char *filename, ArrayType *vals) {
  int32 filename_hash;
  bool *nulls;
  float8 *floats;
  Datum* datums;
  int arrlen;
  Oid valsType;
  int16 floatTypeWidth;
  bool floatTypeByValue;
  char floatTypeAlignmentCode;
  int i;

  filename_hash = hash_filename(filename);

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
  PG_TRY();
  {
    if (extend_file_from_floats(tablespace, filename, floats, nulls, arrlen)) {
      ereport(ERROR, (errmsg("Failed to extend floatfile %s: %s", filename, strerror(errno))));
    }
  }
  PG_CATCH();
  {
    DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
    PG_RE_THROW();
  }
  PG_END_TRY();

  DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
}

Datum extend_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(extend_floatfile);
/**
 * extend_floatfile - Appends to an existing floatfile file in the data directory.
 */
Datum
extend_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;
  ArrayType *vals;

  if (PG_ARGISNULL(0)) PG_RETURN_VOID();
  if (PG_ARGISNULL(1)) PG_RETURN_VOID();

  filename_arg = PG_GETARG_TEXT_P(0);
  filename = GET_STR(filename_arg);

  vals = PG_GETARG_ARRAYTYPE_P(1);

  _extend_floatfile(NULL, filename, vals);

  PG_RETURN_VOID();
}



Datum extend_floatfile_in_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(extend_floatfile_in_tablespace);
/**
 * extend_floatfile_in_tablespace - Appends to an existing floatfile file in the tablespace.
 */
Datum
extend_floatfile_in_tablespace(PG_FUNCTION_ARGS) {
  text *tablespace_arg;
  char *tablespace;
  text *filename_arg;
  char *filename;
  ArrayType *vals;

  if (PG_ARGISNULL(1)) PG_RETURN_VOID();
  if (PG_ARGISNULL(2)) PG_RETURN_VOID();

  if (PG_ARGISNULL(0)) {
    tablespace = NULL;
  } else {
    tablespace_arg = PG_GETARG_TEXT_P(0);
    tablespace = GET_STR(tablespace_arg);
  }

  filename_arg = PG_GETARG_TEXT_P(1);
  filename = GET_STR(filename_arg);

  vals = PG_GETARG_ARRAYTYPE_P(2);

  _extend_floatfile(tablespace, filename, vals);

  PG_RETURN_VOID();
}



static void _drop_floatfile(const char *tablespace, const char *filename) {
  char root_directory[FLOATFILE_MAX_PATH + 1],
       relative_target[FLOATFILE_MAX_PATH + 1],
       path[FLOATFILE_MAX_PATH + 1];
  int pathlen;
  int32 filename_hash;

  filename_hash = hash_filename(filename);

  validate_target_filename(filename);
  pathlen = floatfile_filename_to_full_path(tablespace, filename, path, FLOATFILE_MAX_PATH + 1);

  DirectFunctionCall2(pg_advisory_lock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
  PG_TRY();
  {
    if (unlink(path)) ereport(ERROR, (errmsg("Failed to delete floatfile %s: %s", filename, strerror(errno))));

    path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
    if (unlink(path)) ereport(ERROR, (errmsg("Failed to delete floatfile %s: %s", filename, strerror(errno))));

    // If that was the last file, remove the floatfile dir too
    // so users can drop the tablespace:

    floatfile_root_path(tablespace, root_directory, FLOATFILE_MAX_PATH + 1);
    floatfile_relative_target_path(filename, relative_target, FLOATFILE_MAX_PATH + 1);
    if (rmdirs_for_floatfile(root_directory, relative_target)) {
      ereport(ERROR, (errmsg("Failed in rmdirs_for_floatfile: %s", strerror(errno))));
    }
  }
  PG_CATCH();
  {
    DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
    PG_RE_THROW();
  }
  PG_END_TRY();

  DirectFunctionCall2(pg_advisory_unlock_int4, FLOATFILE_LOCK_PREFIX, filename_hash);
}

Datum drop_floatfile(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(drop_floatfile);
/**
 * drop_floatfile - Deletes the files used by this floatfile.
 *
 * Parameters:
 *   `filename` - The name of the file to use. Must not already exist!
 */
Datum
drop_floatfile(PG_FUNCTION_ARGS)
{
  text *filename_arg;
  char *filename;

  if (PG_ARGISNULL(0)) PG_RETURN_VOID();

  filename_arg = PG_GETARG_TEXT_P(0);
  filename = GET_STR(filename_arg);

  _drop_floatfile(NULL, filename);

  PG_RETURN_VOID();
}



Datum drop_floatfile_in_tablespace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(drop_floatfile_in_tablespace);
/**
 * drop_floatfile_in_tablespace - Deletes the files used by this floatfile.
 *
 * Parameters:
 *   `filename` - The name of the file to use. Must not already exist!
 */
Datum
drop_floatfile_in_tablespace(PG_FUNCTION_ARGS)
{
  text *tablespace_arg;
  char *tablespace;
  text *filename_arg;
  char *filename;

  if (PG_ARGISNULL(1)) PG_RETURN_VOID();

  if (PG_ARGISNULL(0)) {
    tablespace = NULL;
  } else {
    tablespace_arg = PG_GETARG_TEXT_P(0);
    tablespace = GET_STR(tablespace_arg);
  }

  filename_arg = PG_GETARG_TEXT_P(1);
  filename = GET_STR(filename_arg);

  _drop_floatfile(tablespace, filename);

  PG_RETURN_VOID();
}


static int open_floatfile_for_reading(char *tablespace, char *filename, int *vals_fd, int *nulls_fd) {
  char path[FLOATFILE_MAX_PATH + 1];
  int pathlen;

  validate_target_filename(filename);
  pathlen = floatfile_filename_to_full_path(tablespace, filename, path, FLOATFILE_MAX_PATH + 1);

  *nulls_fd = open(path, O_RDONLY);
  if (*nulls_fd == -1) return -1;

  path[pathlen - 1] = FLOATFILE_FLOATS_SUFFIX;
  *vals_fd = open(path, O_RDONLY);
  if (*vals_fd == -1) {
    close(*nulls_fd);
    return -1;
  }

  return 0;
}

Datum floatfile_to_hist2d(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(floatfile_to_hist2d);
/**
 * floatfile_to_hist2d - Uses two floatfiles to build a 2d histogram.
 */
Datum
floatfile_to_hist2d(PG_FUNCTION_ARGS)
{
  // TODO: float4 instead of float8??
  char *xs_filename;
  char *ys_filename;
  int32 xs_filename_hash, ys_filename_hash;
  int x_fd = 0, x_nulls_fd = 0, y_fd = 0, y_nulls_fd = 0;
  float8 x_min, y_min, x_width, y_width;
  int32 x_count, y_count;
  // Make sure `counts` has the same width as Datum
  // so we can avoid a memcpy:
#ifdef SAFE_TO_CAST_FLOATS_AND_DATUMS
  int64 *counts = NULL;
#else
  int32 *counts = NULL;
#endif
  char *errstr = NULL;
  Datum *histContent;
  int arrayLength;
  ArrayType *histVals;
  int16 histTypeWidth;
  bool histTypeByValue;
  char histTypeAlignmentCode;
  bool *histNulls = NULL;
  int dims[2];
  int lbs[2];     // Lower Bounds of each dimension

  if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) ||
      PG_ARGISNULL(4) || PG_ARGISNULL(5) || PG_ARGISNULL(6) || PG_ARGISNULL(7)) {
    PG_RETURN_NULL();
  }

  xs_filename = GET_STR(PG_GETARG_TEXT_P(0));
  ys_filename = GET_STR(PG_GETARG_TEXT_P(1));

  x_min = PG_GETARG_FLOAT8(2);
  y_min = PG_GETARG_FLOAT8(3);
  x_width = PG_GETARG_FLOAT8(4);
  y_width = PG_GETARG_FLOAT8(5);
  x_count = PG_GETARG_INT32(6);
  y_count = PG_GETARG_INT32(7);

  xs_filename_hash = hash_filename(xs_filename);
  ys_filename_hash = hash_filename(ys_filename);
  // TODO: Should go from least to greatest to avoid deadlocks:
  DirectFunctionCall2(pg_advisory_lock_shared_int4, FLOATFILE_LOCK_PREFIX, xs_filename_hash);
  DirectFunctionCall2(pg_advisory_lock_shared_int4, FLOATFILE_LOCK_PREFIX, ys_filename_hash);

  if (open_floatfile_for_reading(NULL, xs_filename, &x_fd, &x_nulls_fd) == -1) {
    errstr = strerror(errno);
    goto bail;
  }
  if (open_floatfile_for_reading(NULL, ys_filename, &y_fd, &y_nulls_fd) == -1) {
    errstr = strerror(errno);
    goto bail;
  }

  arrayLength = x_count * y_count;
  counts = palloc0(sizeof(counts[0]) * arrayLength);
  histNulls = palloc0(sizeof(bool) * arrayLength);


  build_histogram(x_fd, x_nulls_fd, x_min, x_width, x_count,
                  y_fd, y_nulls_fd, y_min, y_width, y_count,
                  counts, &errstr);

bail:
  if (x_fd && close(x_fd)) errstr = "Can't close x_fd";
  if (x_nulls_fd && close(x_nulls_fd)) errstr = "Can't close x_nulls_fd";
  if (y_fd && close(y_fd)) errstr = "Can't close y_fd";
  if (y_nulls_fd && close(y_nulls_fd)) errstr = "Can't close y_nulls_fd";
  DirectFunctionCall2(pg_advisory_unlock_shared_int4, FLOATFILE_LOCK_PREFIX, xs_filename_hash);
  DirectFunctionCall2(pg_advisory_unlock_shared_int4, FLOATFILE_LOCK_PREFIX, ys_filename_hash);
  if (errstr) elog(ERROR, "%s", errstr);

  // Wrap the buckets in a new PostgreSQL array object.
  histContent = (Datum*)counts;   // safe as long as counts is int64. TODO support 32-bit systems
  lbs[0] = 1;
  lbs[1] = 1;
  dims[0] = x_count;
  dims[1] = y_count;
  get_typlenbyvalalign(INT4OID, &histTypeWidth, &histTypeByValue, &histTypeAlignmentCode);
  histVals = construct_md_array(histContent, histNulls, 2, dims, lbs, INT4OID, histTypeWidth, histTypeByValue, histTypeAlignmentCode);
  PG_RETURN_ARRAYTYPE_P(histVals);
}
