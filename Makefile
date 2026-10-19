# BoxForge - minimal educational Linux container runtime
# Force GCC (explicitly required by the project specification).
CC      := gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
CFLAGS  += -D_GNU_SOURCE -Iinclude
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
TESTSRC := $(wildcard tests/*.c)
TESTBIN := $(patsubst tests/%.c,build/tests/%,$(TESTSRC))

.PHONY: all clean test preflight rootfs testbins audit acceptance distclean

all: boxforge

boxforge: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c $(wildcard include/*.h) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build build/tests:
	mkdir -p $@

# Test helpers are linked statically so they run inside any rootfs.
build/tests/%: tests/%.c | build/tests
	$(CC) -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -static -o $@ $<

testbins: $(TESTBIN)

preflight:
	./scripts/preflight.sh

rootfs:
	./scripts/create-rootfs.sh

audit:
	./scripts/audit-host.sh

# Needs root: re-executes itself through sudo when necessary.
test: all testbins
	@if [ "$$(id -u)" -ne 0 ]; then exec sudo -E ./tests/run_tests.sh; else exec ./tests/run_tests.sh; fi

# Full acceptance test script (separate from unit tests, uses the live binary).
acceptance: all testbins
	@if [ "$$(id -u)" -ne 0 ]; then exec sudo -E ./tests/acceptance.sh; else exec ./tests/acceptance.sh; fi

clean:
	rm -rf build boxforge

distclean: clean
	rm -rf rootfs/* runtime/*
	touch rootfs/.gitkeep runtime/.gitkeep
