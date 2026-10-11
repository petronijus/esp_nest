# Host-side unit tests for pure-logic modules (no ESP-IDF needed).
# Usage: make -C test/host run
#
# One binary per module: build/test_<module> is built from test_<module>.c
# and $(MAIN)/<module>.c, plus the module sources listed as extra
# prerequisites below. `run` executes every binary and fails if any of
# them fails. All build output stays in build/ (gitignored), so nothing
# else in this directory is ever generated.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Werror -g -O1 -pthread
MAIN    := ../../esphome/esp-nest-components/esp_nest
BUILD   := build

TESTS   := test_light_sync test_mqtt_payload test_fan_sync test_blinds_sync \
           test_input_fsm test_backlight_policy test_settings_word test_status_line \
           test_status_handoff
BINS    := $(addprefix $(BUILD)/,$(TESTS))
HEADERS := check.h $(wildcard $(MAIN)/*.h)

# Modules a test links in addition to its own
$(BUILD)/test_mqtt_payload: $(MAIN)/light_sync.c
$(BUILD)/test_fan_sync:     $(MAIN)/mqtt_payload.c $(MAIN)/light_sync.c
$(BUILD)/test_blinds_sync:  $(MAIN)/mqtt_payload.c $(MAIN)/light_sync.c

$(BUILD)/test_%: test_%.c $(MAIN)/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ $(filter %.c,$^)

$(BUILD):
	mkdir -p $@

.PHONY: all run clean
all: $(BINS)

run: $(BINS)
	@failed=0; \
	for t in $(BINS); do \
		echo "=== $$t"; \
		./$$t || failed=$$((failed + 1)); \
	done; \
	echo "=== $(words $(BINS)) test binaries, $$failed failed"; \
	[ $$failed -eq 0 ]

clean:
	rm -rf $(BUILD)
