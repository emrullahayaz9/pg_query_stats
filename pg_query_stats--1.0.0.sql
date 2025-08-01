-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_query_stats" to load this file. \quit

CREATE FUNCTION pg_query_stats()
RETURNS SETOF record
AS 'pg_query_stats', 'pg_query_stats'
LANGUAGE C STRICT;

CREATE FUNCTION pg_query_stats_reset()
RETURNS void
AS 'pg_query_stats', 'pg_query_stats_reset'
LANGUAGE C STRICT;

CREATE VIEW pg_query_stats AS
SELECT 
    query_text::text,
    calls::bigint,
    total_time::double precision AS total_time_ms,
    (total_time/calls)::double precision AS avg_time_ms,
    min_time::double precision AS min_time_ms,
    max_time::double precision AS max_time_ms
FROM pg_query_stats() AS (
    query_text text,
    calls bigint,
    total_time double precision,
    min_time double precision,
    max_time double precision
);