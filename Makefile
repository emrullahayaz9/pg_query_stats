EXTENSION = pg_query_stats
DATA = pg_query_stats--1.0.0.sql
REGRESS = pg_query_stats-regress
MODULES = pg_query_stats
PG_CONFIG  ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)