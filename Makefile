MODULE_big = floatfile
EXTENSION = floatfile
EXTENSION_VERSION = 1.3.1
DATA = $(EXTENSION)--$(EXTENSION_VERSION).sql $(EXTENSION)--1.3.0--1.3.1.sql
REGRESS = $(EXTENSION)_test
OBJS = floatfile.o histogram.o $(WIN32RES)
# PG_CPPFLAGS = -pg
# LDFLAGS_SL += -pg
EXTRA_CLEAN = bencher bencher.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

bencher: histogram.o bencher.o

bench: bencher
	./bencher 2>&1 | grep counting | cut -d ' ' -f 3 | awk '{total += $$1 } END { print total/NR }'

README.html: README.md
	jq --slurp --raw-input '{"text": "\(.)", "mode": "markdown"}' < README.md | curl --data @- https://api.github.com/markdown > README.html

release:
	git tag v$(EXTENSION_VERSION)
	git archive --format zip --prefix=$(EXTENSION)-$(EXTENSION_VERSION)/ --output $(EXTENSION)-$(EXTENSION_VERSION).zip master

.PHONY: release

