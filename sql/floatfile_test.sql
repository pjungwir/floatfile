CREATE EXTENSION floatfile;

SELECT save_floatfile('test', '{1,2,3,NULL,4,NULL}'::float[]);
SELECT load_floatfile('test');
SELECT extend_floatfile('test', '{NULL,5}'::float[]);
SELECT load_floatfile('test');
SELECT drop_floatfile('test');

DROP TABLESPACE IF EXISTS testspace;
\! test -d /tmp/testspace || echo "* * * Can't test tablespaces unless you mkdir /tmp/testspace and chown it to the postgres user * * *"
CREATE TABLESPACE testspace LOCATION '/tmp/testspace';

SELECT save_floatfile('testspace', 'test', '{1,2,3,NULL,4,NULL}'::float[]);
SELECT load_floatfile('testspace', 'test');
SELECT extend_floatfile('testspace', 'test', '{NULL,5}'::float[]);
SELECT load_floatfile('testspace', 'test');
SELECT drop_floatfile('testspace', 'test');

DROP TABLESPACE testspace;
