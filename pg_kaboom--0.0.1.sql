CREATE FUNCTION pg_kaboom(method text, payload jsonb default NULL)
RETURNS boolean AS 'MODULE_PATHNAME', 'pg_kaboom'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_kaboom_arsenal()
RETURNS TABLE (weapon_name text, description text)
AS 'MODULE_PATHNAME', 'pg_kaboom_arsenal'
LANGUAGE C STRICT;
