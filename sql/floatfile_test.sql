CREATE EXTENSION floatfile;

SELECT save_floatfile('test', '{1,2,3,NULL,4,NULL}'::float[]);
SELECT load_floatfile('test');
SELECT extend_floatfile('test', '{NULL,5}'::float[]);
SELECT load_floatfile('test');
