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

-- Histogram tests:

SELECT save_floatfile('a', '{1,1,1,1,NULL}'::float[]);
SELECT floatfile_to_hist('a', 0::float, 1::float, 5);
SELECT drop_floatfile('a');

SELECT save_floatfile('a', '{1,1}'::float[]);
SELECT floatfile_to_hist('a', -0.115::float, 0.23::float, 10);
SELECT drop_floatfile('a');

-- Histogram with tablespace tests:

CREATE TABLESPACE testspace LOCATION '/tmp/testspace';

SELECT save_floatfile('testspace', 'a', '{1,1,1,1,NULL}'::float[]);
SELECT floatfile_to_hist('testspace', 'a', 0::float, 1::float, 5);
SELECT drop_floatfile('testspace', 'a');

SELECT save_floatfile('testspace', 'a', '{1,1}'::float[]);
SELECT floatfile_to_hist('testspace', 'a', -0.115::float, 0.23::float, 10);
SELECT drop_floatfile('testspace', 'a');

SELECT save_floatfile(NULL, 'a', '{1,1}'::float[]);
SELECT floatfile_to_hist(NULL, 'a', -0.115::float, 0.23::float, 10);
SELECT drop_floatfile(NULL, 'a');

DROP TABLESPACE testspace;

-- 2D Histogram tests:

SELECT save_floatfile('a', '{1,1,1,1,NULL}'::float[]);
SELECT save_floatfile('b', '{1,1,0,NULL,1}'::float[]);
SELECT floatfile_to_hist2d('a', 'b', 0::float, 0::float, 1::float, 1::float, 5, 2);
SELECT drop_floatfile('a');
SELECT drop_floatfile('b');

SELECT save_floatfile('a', '{1,1}'::float[]);
SELECT save_floatfile('b', '{1,2}'::float[]);
SELECT floatfile_to_hist2d('a', 'b', -0.115::float, 0.944444::float, 0.23::float, 0.111111::float, 10, 10);
SELECT drop_floatfile('a');
SELECT drop_floatfile('b');

-- 2D histogram with tablespace tests:

CREATE TABLESPACE testspace LOCATION '/tmp/testspace';

SELECT save_floatfile('testspace', 'a', '{1,1,1,1,NULL}'::float[]);
SELECT save_floatfile('testspace', 'b', '{1,1,0,NULL,1}'::float[]);
SELECT floatfile_to_hist2d('testspace', 'a', 'testspace', 'b', 0::float, 0::float, 1::float, 1::float, 5, 2);
SELECT drop_floatfile('testspace', 'a');
SELECT drop_floatfile('testspace', 'b');

SELECT save_floatfile('testspace', 'a', '{1,1}'::float[]);
SELECT save_floatfile('testspace', 'b', '{1,2}'::float[]);
SELECT floatfile_to_hist2d('testspace', 'a', 'testspace', 'b', -0.115::float, 0.944444::float, 0.23::float, 0.111111::float, 10, 10);
SELECT drop_floatfile('testspace', 'a');
SELECT drop_floatfile('testspace', 'b');

SELECT save_floatfile(NULL, 'a', '{1,1}'::float[]);
SELECT save_floatfile(NULL, 'b', '{1,2}'::float[]);
SELECT floatfile_to_hist2d(NULL, 'a', NULL, 'b', -0.115::float, 0.944444::float, 0.23::float, 0.111111::float, 10, 10);
SELECT drop_floatfile(NULL, 'a');
SELECT drop_floatfile(NULL, 'b');

DROP TABLESPACE testspace;
