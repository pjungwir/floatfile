CREATE EXTENSION floatfile;

SELECT save_floatfile('test', '{1,2,3,NULL,4,NULL}'::float[]);
SELECT load_floatfile('test');
SELECT extend_floatfile('test', '{NULL,5}'::float[]);
SELECT load_floatfile('test');
SELECT drop_floatfile('test');

-- Can't overwrite a file without removing it first:

SELECT save_floatfile('newtest', '{7,7,7}'::float[]);
SELECT save_floatfile('newtest', '{7,7,7}'::float[]);
SELECT load_floatfile('newtest');
SELECT drop_floatfile('newtest');

-- Appending creates the file if necessary:

SELECT extend_floatfile('newtest', '{NULL,5}'::float[]);
SELECT load_floatfile('newtest');
SELECT drop_floatfile('newtest');

-- Tablespace tests:

DROP TABLESPACE IF EXISTS testspace;
\! test -d /tmp/testspace || echo "* * * Can't test tablespaces unless you mkdir /tmp/testspace and chown it to the postgres user * * *"
CREATE TABLESPACE testspace LOCATION '/tmp/testspace';

SELECT save_floatfile('testspace', 'test', '{1,2,3,NULL,4,NULL}'::float[]);
SELECT load_floatfile('testspace', 'test');
SELECT extend_floatfile('testspace', 'test', '{NULL,5}'::float[]);
SELECT load_floatfile('testspace', 'test');
SELECT drop_floatfile('testspace', 'test');

DROP TABLESPACE testspace;

-- Tests about releasing advisory locks:

SELECT  classid, objid
FROM    pg_locks
WHERE   database = (SELECT oid FROM pg_database WHERE datname = current_database())
AND     locktype = 'advisory';

SELECT load_floatfile('nofile');

SELECT  classid, objid
FROM    pg_locks
WHERE   database = (SELECT oid FROM pg_database WHERE datname = current_database())
AND     locktype = 'advisory';

SELECT save_floatfile('nofile', '{1,2,3}'::float[]);
SELECT save_floatfile('nofile', '{1,2,3}'::float[]);

SELECT  classid, objid
FROM    pg_locks
WHERE   database = (SELECT oid FROM pg_database WHERE datname = current_database())
AND     locktype = 'advisory';

SELECT drop_floatfile('nofile');
SELECT drop_floatfile('nofile');

SELECT  classid, objid
FROM    pg_locks
WHERE   database = (SELECT oid FROM pg_database WHERE datname = current_database())
AND     locktype = 'advisory';
