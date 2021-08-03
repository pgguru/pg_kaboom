CREATE FUNCTION pg_kaboom(method text, payload jsonb default NULL)
RETURNS boolean AS 'MODULE_PATHNAME', 'pg_kaboom'
LANGUAGE C VOLATILE;

