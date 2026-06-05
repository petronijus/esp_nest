# Host-side unit tests for pure-logic modules (no ESP-IDF needed).
# Usage: make -C test/host run

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Werror -g -O1
MAIN    := ../../main

test_light_sync: test_light_sync.c $(MAIN)/light_sync.c $(MAIN)/light_sync.h
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ test_light_sync.c $(MAIN)/light_sync.c

.PHONY: run clean
run: test_light_sync
	./test_light_sync

clean:
	rm -f test_light_sync
