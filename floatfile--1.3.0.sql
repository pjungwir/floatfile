/* floatfile--1.3.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION floatfile" to load this file. \quit


CREATE OR REPLACE FUNCTION
save_floatfile(filename text, vals float[])
RETURNS void
AS 'floatfile', 'save_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
load_floatfile(filename text)
RETURNS float[]
AS 'floatfile', 'load_floatfile'
LANGUAGE c STABLE;

CREATE OR REPLACE FUNCTION
extend_floatfile(filename text, vals float[])
RETURNS void
AS 'floatfile', 'extend_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
drop_floatfile(filename text)
RETURNS void
AS 'floatfile', 'drop_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist(
  filename text,
  buckets_start float,
  bucket_width float,
  bucket_count int)
RETURNS int[]
AS 'floatfile', 'floatfile_to_hist'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist(
  filename text,
  buckets_start float,
  bucket_width float,
  bucket_count int,
  timestamps_filename text,
  timestamps_start float,
  timestamps_end float)
RETURNS int[]
AS 'floatfile', 'floatfile_with_bounds_to_hist'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(
  x_filename text, y_filename text,
  x_buckets_start float, y_buckets_start float,
  x_bucket_width float, y_bucket_width float,
  x_bucket_count int, y_bucket_count int)
RETURNS int[]
AS 'floatfile', 'floatfile_to_hist2d'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(
  x_filename text, y_filename text,
  x_buckets_start float, y_buckets_start float,
  x_bucket_width float, y_bucket_width float,
  x_bucket_count int, y_bucket_count int,
  timestamps_filename text,
  timestamps_start float,
  timestamps_end float)
RETURNS int[]
AS 'floatfile', 'floatfile_with_bounds_to_hist2d'
LANGUAGE c VOLATILE;


CREATE OR REPLACE FUNCTION
save_floatfile(tablespace_name text, filename text, vals float[])
RETURNS void
AS 'floatfile', 'save_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
load_floatfile(tablespace_name text, filename text)
RETURNS float[]
AS 'floatfile', 'load_floatfile_from_tablespace'
LANGUAGE c STABLE;

CREATE OR REPLACE FUNCTION
extend_floatfile(tablespace_name text, filename text, vals float[])
RETURNS void
AS 'floatfile', 'extend_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
drop_floatfile(tablespace_name text, filename text)
RETURNS void
AS 'floatfile', 'drop_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist(
  tablespace_name text,
  filename text,
  buckets_start float,
  bucket_width float,
  bucket_count int)
RETURNS int[]
AS 'floatfile', 'floatfile_in_tablespace_to_hist'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist(
  tablespace_name text,
  filename text,
  buckets_start float,
  bucket_width float,
  bucket_count int,
  timestamps_tablespace_name text,
  timestamps_filename text,
  timestamps_start float,
  timestamps_end float)
RETURNS int[]
AS 'floatfile', 'floatfile_in_tablespace_with_bounds_to_hist'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(
  x_tablespace_name text, x_filename text,
  y_tablespace_name text, y_filename text,
  x_buckets_start float, y_buckets_start float,
  x_bucket_width float, y_bucket_width float,
  x_bucket_count int, y_bucket_count int)
RETURNS int[]
AS 'floatfile', 'floatfile_in_tablespace_to_hist2d'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(
  x_tablespace_name text, x_filename text,
  y_tablespace_name text, y_filename text,
  x_buckets_start float, y_buckets_start float,
  x_bucket_width float, y_bucket_width float,
  x_bucket_count int, y_bucket_count int,
  timestamps_tablespace_name text, timestamps_filename text,
  timestamps_start float, timestamps_end float)
RETURNS int[]
AS 'floatfile', 'floatfile_in_tablespace_with_bounds_to_hist2d'
LANGUAGE c VOLATILE;
