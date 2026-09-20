##
## Makefile
##
## Copyright (C) 2026, Charles Chiou
##

SHELL := /bin/bash
BUILD_DIR := build
NUM_PROCS := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all clean distclean test

all:
	@if [ -f .gitmodules ] && [ ! -f third_party/cpp-httplib/httplib.h ]; then \
		git submodule update --init --recursive; \
	fi
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && (test -f Makefile || cmake .. -DCMAKE_BUILD_TYPE=Release)
	@$(MAKE) -C $(BUILD_DIR) -j$(NUM_PROCS)

test: all
	@./$(BUILD_DIR)/test_gateway_timeout

clean:
	@if [ -d $(BUILD_DIR) ] && [ -f $(BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(BUILD_DIR) clean; \
	fi

distclean:
	@rm -rf $(BUILD_DIR)
