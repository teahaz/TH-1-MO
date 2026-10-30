# bus-replay: one static library per directory, linked into one binary (main.c and
# the modes in modes/). A library directory with no sources yet builds an empty archive.
#
# Everything that exists for the UI is under ui/, and nothing outside it includes it; with
# ui/ gone, bus-replay is the headless binary. Its plain C libraries (ui/store/,
# ui/session/) and tests (ui/test/) are built whenever they are there. Only ui/cel/ needs
# CEL, which is proprietary and not part of the repository: it is built in when both
# vendor/CEL-b1 and ui/cel/ are there, and needs Lua 5.4 and libcurl then; without them
# bus-replay is built without the ui mode. A library in a subdirectory is named after its
# path, ui/store as build/libui_store.a.

CC := gcc
AR := ar
BUILD := build
GEN := $(BUILD)/gen
CEL := vendor/CEL-b1

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -D_GNU_SOURCE -I. -MMD -MP
LDLIBS := -pthread -lm

# In link order: a library before the ones it uses.
LIBS := $(foreach d,ui/session ui/store,$(if $(wildcard $(d)/*.c),$(d))) codec capture
LIB_FILES := $(foreach l,$(LIBS),$(BUILD)/lib$(subst /,_,$(l)).a)
TESTS := $(patsubst %.c,$(BUILD)/%,$(wildcard test/*.c ui/test/*.c))
APP_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,main.c $(wildcard modes/*.c))

lib_srcs = $(wildcard $(1)/*.c $(1)/*/*.c)
lib_objs = $(patsubst %.c,$(BUILD)/obj/%.o,$(call lib_srcs,$(1)))

UI := $(and $(wildcard $(CEL)/src/runtime.c),$(wildcard ui/cel/ui.c))

ifneq ($(UI),)
LUA_CFLAGS := $(shell pkg-config --cflags lua5.4)
UI_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(wildcard ui/cel/*.c))
APP_OBJS += $(UI_OBJS)
# The bindings reach every Lua state CEL makes through a wrap of luaL_openlibs.
LDLIBS += -Wl,--wrap=luaL_openlibs $(shell pkg-config --libs lua5.4) $(shell curl-config --libs) -ldl

# CEL's Lua sources and the UI program as C arrays, as CEL's own Makefile makes them.
CEL_LUA := shared widget router cli
CEL_HEADERS := $(CEL_LUA:%=$(GEN)/lua/%.h) $(GEN)/lua/lunajson.h \
	$(GEN)/lua/lunajson_decoder.h $(GEN)/lua/lunajson_encoder.h $(GEN)/lua/lunajson_sax.h
endif

.PHONY: all test clean
.SECONDARY:

all: $(BUILD)/bus-replay

$(BUILD)/bus-replay: $(APP_OBJS) $(LIB_FILES)
	$(CC) $(APP_OBJS) $(LIB_FILES) $(LDLIBS) -o $@

.SECONDEXPANSION:
$(BUILD)/lib%.a: $$(call lib_objs,$$(subst _,/,$$*))
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTS): $(BUILD)/%: $(BUILD)/obj/%.o $(LIB_FILES)
	@mkdir -p $(@D)
	$(CC) $< $(LIB_FILES) $(LDLIBS) -o $@

ifneq ($(UI),)
$(UI_OBJS): CFLAGS += $(LUA_CFLAGS) -I$(GEN)

# The CEL runtime: its own code, built as it is, without warnings.
$(BUILD)/obj/ui/cel/cel.o: CFLAGS := -std=gnu11 -O2 -w -I. -I$(GEN) -I$(CEL)/src -I$(CEL)/vendor \
	-I$(CEL)/vendor/civetweb $(LUA_CFLAGS) $(shell curl-config --cflags) -MMD -MP
$(BUILD)/obj/ui/cel/cel.o: $(CEL_HEADERS)
$(BUILD)/obj/ui/cel/ui.o: $(GEN)/ui/cel/bus_replay_cel.h

$(GEN)/ui/cel/bus_replay_cel.h: ui/cel/bus_replay.cel
	@mkdir -p $(@D)
	cd ui/cel && xxd -i bus_replay.cel > $(abspath $@)

$(GEN)/lua/%.h: $(CEL)/src/lua/%.cel
	@mkdir -p $(@D)
	cd $(CEL)/src/lua && xxd -i $*.cel | sed s/$*_cel/lua_$*/g > $(abspath $@)

$(GEN)/lua/lunajson.h: $(CEL)/vendor/lunajson/lunajson.lua
	@mkdir -p $(@D)
	cd $(CEL)/vendor/lunajson && xxd -i lunajson.lua | sed s/lunajson_lua/lua_lunajson/g > $(abspath $@)

$(GEN)/lua/lunajson_%.h: $(CEL)/vendor/lunajson/%.lua
	@mkdir -p $(@D)
	cd $(CEL)/vendor/lunajson && xxd -i $*.lua | sed s/$*_lua/lua_lunajson_$*/g > $(abspath $@)
endif

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
