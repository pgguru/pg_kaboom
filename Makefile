EXTENSION = pg_kaboom
MODULE_big = pg_kaboom
DATA = pg_kaboom--0.0.1.sql
OBJS = pg_kaboom.o 
PG_CONFIG = pg_config
PG_CFLAGS := -DPGBINDIR=\"$(shell $(PG_CONFIG) --bindir)\" -Wno-missing-prototypes -Wno-deprecated-declarations
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
