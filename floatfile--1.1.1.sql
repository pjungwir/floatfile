/* floatfile--1.1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION floatfile" to load this file. \quit


CREATE OR REPLACE FUNCTION
save_floatfile(text, float[])
RETURNS void
AS 'floatfile', 'save_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
load_floatfile(text)
RETURNS float[]
AS 'floatfile', 'load_floatfile'
LANGUAGE c STABLE;

CREATE OR REPLACE FUNCTION
extend_floatfile(text, float[])
RETURNS void
AS 'floatfile', 'extend_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
drop_floatfile(text)
RETURNS void
AS 'floatfile', 'drop_floatfile'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(text, text, float, float, float, float, int, int)
RETURNS int[]
AS 'floatfile', 'floatfile_to_hist2d'
LANGUAGE c VOLATILE;


CREATE OR REPLACE FUNCTION
save_floatfile(text, text, float[])
RETURNS void
AS 'floatfile', 'save_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
load_floatfile(text, text)
RETURNS float[]
AS 'floatfile', 'load_floatfile_from_tablespace'
LANGUAGE c STABLE;

CREATE OR REPLACE FUNCTION
extend_floatfile(text, text, float[])
RETURNS void
AS 'floatfile', 'extend_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
drop_floatfile(text, text)
RETURNS void
AS 'floatfile', 'drop_floatfile_in_tablespace'
LANGUAGE c VOLATILE;

CREATE OR REPLACE FUNCTION
floatfile_to_hist2d(text, text, text, text, float, float, float, float, int, int)
RETURNS int[]
AS 'floatfile', 'floatfile_in_tablespace_to_hist2d'
LANGUAGE c VOLATILE;
