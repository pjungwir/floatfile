`floatfile`
===========

A Postgres extension to store float arrays in individual files,
providing very fast querying (especially when combined with
[`aggs_for_arrays`](https://github.com/pjungwir/aggs_for_arrays),
[`aggs_for_vecs`](https://github.com/pjungwir/aggs_for_vecs),
or [`floatvec`](https://github.com/pjungwir/floatvec))
without paying a huge cost to keep updating them.

This extension offers several functions:

`save_floatfile(filename TEXT, vals FLOAT[])` - Saves an array to a new file.

This creates a new file inside your Postgres data directory with the values of the array you provide. (Technically it is two files: one for the floats and one for the nulls.) If either `filename` or `vals` is `NULL` then this does nothing. If `vals` has some `NULL` elements, they will be remembered.

`load_floatfile(filename TEXT)` - Returns a float array with the contents of the file.

`extend_floatfile(filename TEXT, newvals FLOAT[])` - Adds `newvals` to the end of `filename`. If `filename` doesn't exist yet, it will be created.

In addition there are tablespace versions of these functions so you can put the files somewhere else:

`save_floatfile(tablespace TEXT, filename TEXT, vals FLOAT[])` - Saves an array to a new file in `tablespace`.

`load_floatfile(tablespace TEXT, filename TEXT)` - Loads an array from `filename` in `tablespace`.

`extend_floatfile(tablespace TEXT, filename TEXT, vals FLOAT[])` - Extends an array to `filename` in `tablespace`.

Note in all cases `tablespace` should be the *name* of the tablespace, not its location on disk.
If it is `NULL` then the data directory is used.

All these functions use [Postgres advisory locks](https://www.postgresql.org/docs/current/static/explicit-locking.html#ADVISORY-LOCKS). `load_floatfile` takes a shared lock, and `save` and `load` take an exclusive one. They use [the two-arg versions of the functions](https://www.postgresql.org/docs/current/static/functions-admin.html#FUNCTIONS-ADVISORY-LOCKS), using `0xF107F11E` for the first arg and the [djb2 hash of the user-provided filename](http://www.cse.yorku.ca/~oz/hash.html) for the second one. (See the source code comments for my thoughts on birthday collisions.) You can change the value of the first arg by compiling with a different `FLOATFILE_LOCK_PREFIX`.
If you really can't stand that this uses advisory locks at all,
then I could probably add a compile-time option to use POSIX file locking instead,
but then you won't see those locks in `pg_locks`
and they won't be covered by pg's deadlock detection.



Pros
----

These functions are extremely fast compared to processing millions of rows,
even using [`cstore_fdw`](https://github.com/citusdata/cstore_fdw).
They are also a mite faster than storing the whole array in a single row of a regular Postgres table,
but unlike that approach they also have very fast `UPDATE`s.
This makes them great for timeseries data,
so you get low-latency queries combined with low-cost appends.



Cons
----

This extension is designed for very fast queries
without paying a high price to keep extending the array,
but there are some drawbacks:

- **Updates:** You can append to the end of an array, but you can't change elements in the middle of it. I suppose when I (or you) need this I will add a function to remove a file or replace a file. That will be a little expensive, but random write access is not really the intended use of this extension.

- **Security:** Anyone who has `EXECUTE` permission on our functions can open *any* `floatfile` in the current database (reading or writing depends on which function). So make sure that's okay before using this extension!

- **Replication:** Your `floatfile`s are not replicated! If you rely on replication, I'd make sure that you only use `floatfiles` for derived data, so that you can reconstruct them if necessary. (You could even run a job that builds them on both the master and the slave. It is legal to create `floatfiles` on a warm standby since they aren't doing any writing that Postgres knows about. TODO: Make sure this is true of taking advisory locks!)

- **Backups:** These files won't appear in your `pg_dump` output, so if you are using that for backups, you need to do something extra to include these files.

- **Durability:** Your `floatfiles` can get corrupted if there is a crash. So again, don't use them except as derived data that you can rebuild from your core operational source. By the way if you have any suggestions to improve the story here, let me know. I'm thinking I could keep a third file that stores just the length of the array, and update it as the last step of each save/extend operation. Then if a future save/extend fails partway through, the bad data will just get ignored. I'd still need to write the length file atomically though, but I think I can do that with a `rename`.

- **Selectivity:** If you want to load something, you load all of it. To do further processing you should use some vector masking functions. (I will probably add these to [`floatvec`](https://github.com/pjungwir/floatvec) by the way, R or Pandas style. . . .) But really this is no different than regular Postgres arrays.

- **Portability:** The on-disk format is the same as the in-memory format. That means you can't move the files from a big-endian to a little-endian system, or between systems with different `sizeof(bool)`. (OTOH `sizeof(float8)` won't change.) This is probably not something you'd care about anyway, but there it is!
Of all these cons this is the easiest to fix, but I'm not sure I care enough to do it, and it will cost a little performance.



FAQ
---

**Why not a Foreign Data Wrapper?**

Well, maybe I will port it to one. :-)
I haven't written an FDW before,
and that way seemed longer to finish v1.
An FDW would improve the cons for Security,
but it still wouldn't support Replication,
and (I'm not sure, but I believe) it still wouldn't improve Durability.
Also I'd need to compare the performance of this vs an FDW.
If I do switch to an FDW, I'll probably use [Andrew Dunstan's `file_text_array_fdw`](https://github.com/adunstan/file_text_array_fdw) as a guide.

**Can this extension read or overwrite my `/etc/passwd`?**

I hope not! I take pains to confine file access to a `floatfile` subdirectory
in either your main data directory or the directory of a given tablespace.
You are not allowed to name files with `..` anywhere in the name---sort of a bigger hammer than required, but it makes the string validation very obvious.
You also can't start a filename with `/`.
If you find a security hole though, I'd appreciate your [private email](mailto:pj@illuminatedcomputing.com).

**Does this work on Mac?**

Yes.

**Does this work on Linux?**

Of course.

**Does this work on Windows?**

I doubt it.
(I am available for hire if you need this though. :-)



Author
------

Paul A. Jungwirth
