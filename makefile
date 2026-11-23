#
# FILE            makefile
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corMcp - an MCP server for NGSI-LD brokers
#
#   make                 the corMcp binary (in this directory)
#   make test            build, and run the tests against a broker (BROKER_URL, default http://localhost:1026)
#   make install         $(PREFIX)/bin/corMcp (PREFIX=/usr/local; DESTDIR for packaging)
#   make uninstall
#   make clean
#
# Built on the cor-libs, as coraine is: SIBLING checkouts (../corRest, ../corJson, ...) built there,
# `-I..` and the archives linked by path. COR_LIBS=<dir> for checkouts elsewhere.
#
CC          = gcc
COR_LIBS   ?= ..
PREFIX     ?= /usr/local
DESTDIR    ?=
BUILD      ?= release

ifeq ($(BUILD),debug)
  OPT       = -g -O0
else
  OPT       = -O2
endif

CFLAGS      = $(OPT) -Wall -Werror -Wundef -Wno-format-truncation -fstack-protector-all -I$(COR_LIBS) -MMD -MP $(EXTRA_CFLAGS)

SOURCES     = src/corMcp.c src/rpc.c src/tools.c src/shape.c src/backendHttp.c src/str.c src/brokerTools.c src/docs.c src/sdm.c src/rules.c
OBJDIR      = obj/$(BUILD)
OBJECTS     = $(SOURCES:src/%.c=$(OBJDIR)/%.o)
DEPS        = $(OBJECTS:.o=.d)

#
# Link order: a library before the ones it uses
#
COR_ARCHIVES = $(COR_LIBS)/corRest/libcorRest.a   \
               $(COR_LIBS)/corJson/libcorJson.a   \
               $(COR_LIBS)/corTree/libcorTree.a   \
               $(COR_LIBS)/corArgs/libcorArgs.a   \
               $(COR_LIBS)/corLog/libcorLog.a     \
               $(COR_LIBS)/corAlloc/libcorAlloc.a \
               $(COR_LIBS)/corBase/libcorBase.a

LIBS        = $(COR_ARCHIVES) -lssl -lcrypto -lz -lpthread -lm

#
# macOS: clang (no -Wformat-truncation: unknown there; corAlloc.h and CorAlloc.h are one file, either spelling
# finds it) and Homebrew's OpenSSL - OPENSSL=<prefix> for another
#
ifeq ($(shell uname -s),Darwin)
  OPENSSL    ?= $(shell brew --prefix openssl@3 2>/dev/null)
  CFLAGS     += -Wno-unknown-warning-option -Wno-nonportable-include-path -I$(OPENSSL)/include
  LDFLAGS    += -L$(OPENSSL)/lib
endif

all: corMcp

corMcp: $(OBJECTS) $(COR_ARCHIVES)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(LIBS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COR_ARCHIVES):
	@echo "missing $@ - corMcp builds against sibling checkouts of the cor-libs, built there (make in each), or COR_LIBS=<dir>"
	@false

test: corMcp
	test/run.sh

install: corMcp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corMcp $(DESTDIR)$(PREFIX)/bin/corMcp

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/corMcp

clean:
	rm -rf obj corMcp

.PHONY: all test install uninstall clean

-include $(DEPS)
