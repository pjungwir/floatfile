MODULE_big = floatfile
EXTENSION = floatfile
EXTENSION_VERSION = 1.1.0
DATA = $(EXTENSION)--$(EXTENSION_VERSION).sql
REGRESS = $(EXTENSION)_test
OBJS = floatfile.o hist2d.o $(WIN32RES)
# PG_CPPFLAGS = -pg
# LDFLAGS_SL += -pg
EXTRA_CLEAN = bencher bencher.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

bencher: hist2d.o bencher.o

bench: bencher
	./bencher 2>&1 | grep counting | cut -d ' ' -f 3 | awk '{total += $$1 } END { print total/NR }'

README.html: README.md
	jq --slurp --raw-input '{"text": "\(.)", "mode": "markdown"}' < README.md | curl --data @- https://api.github.com/markdown > README.html

release:
	git archive --format zip --prefix=$(EXTENSION)-$(EXTENSION_VERSION)/ --output $(EXTENSION)-$(EXTENSION_VERSION).zip master

.PHONY: release

