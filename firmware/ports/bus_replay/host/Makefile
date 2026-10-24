# bus-replay: one static library per directory, linked into one binary (main.c and
# the modes in modes/). A library directory with no sources yet builds an empty archive.

CC := gcc
AR := ar
BUILD := build

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -D_GNU_SOURCE -I. -MMD -MP
LDLIBS := -pthread -lm

LIBS := capture codec
LIB_FILES := $(LIBS:%=$(BUILD)/lib%.a)
TESTS := $(patsubst test/%.c,$(BUILD)/test/%,$(wildcard test/*.c))
APP_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,main.c $(wildcard modes/*.c))

lib_srcs = $(wildcard $(1)/*.c $(1)/*/*.c)
lib_objs = $(patsubst %.c,$(BUILD)/obj/%.o,$(call lib_srcs,$(1)))

.PHONY: all test clean
.SECONDARY:

all: $(BUILD)/bus-replay

$(BUILD)/bus-replay: $(APP_OBJS) $(LIB_FILES)
	$(CC) $(APP_OBJS) $(LIB_FILES) $(LDLIBS) -o $@

.SECONDEXPANSION:
$(BUILD)/lib%.a: $$(call lib_objs,$$*)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test/%: $(BUILD)/obj/test/%.o $(LIB_FILES)
	@mkdir -p $(@D)
	$(CC) $< $(LIB_FILES) $(LDLIBS) -o $@

# Runs every test program; fails if any of them fails. Some tests run the binary, given in
# BUS_REPLAY.
test: $(TESTS) $(BUILD)/bus-replay
	@failed=0; \
	for t in $(TESTS); do \
		if BUS_REPLAY=$(BUILD)/bus-replay $$t; then echo "PASS $$t"; else echo "FAIL $$t"; failed=1; fi; \
	done; \
	exit $$failed

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
