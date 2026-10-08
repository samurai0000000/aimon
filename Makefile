##
## Makefile
##
## Copyright (C) 2026, Charles Chiou
##

SHELL := /bin/bash
BUILD_DIR := build
NUM_PROCS := $(shell nproc 2>/dev/null || echo 4)

# The tests run with HOME pointing at an empty directory under the build tree.
# Some of them (circuit breaker, process monitor) write incident reports and
# move database files in the configuration directory; with a real HOME they
# quarantine a running daemon's databases.
TEST_HOME = $(CURDIR)/$(BUILD_DIR)/test-home
TEST_ENV = HOME=$(TEST_HOME)

# SANITIZE=1 builds and runs the test suite under AddressSanitizer and UBSan in
# a separate build directory (inside build/, which is already ignored).
# SANITIZE_CANARY=1 additionally runs a deliberately broken test that must make
# the sanitized run fail, proving the sanitizers are active.
SANITIZE ?= 0
SANITIZE_CANARY ?= 0
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release
ifeq ($(SANITIZE),1)
BUILD_DIR := build/asan
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Debug -DAIMON_SANITIZE=ON \
	-DAIMON_SANITIZE_CANARY=$(if $(filter 1,$(SANITIZE_CANARY)),ON,OFF)
# TestProcessMonitorLifecycle deliberately raises SIGSEGV in a forked child to
# exercise crash detection, so ASan's SEGV handler is disabled for that one
# binary only; every other test keeps full AddressSanitizer reporting.
ASAN_PM_ENV := ASAN_OPTIONS=handle_segv=0
else
ASAN_PM_ENV :=
endif
ifeq ($(SANITIZE_CANARY),1)
ifneq ($(SANITIZE),1)
$(error SANITIZE_CANARY=1 requires SANITIZE=1)
endif
endif

.PHONY: all clean distclean test

all:
	@if [ -f .gitmodules ] && [ ! -f third_party/cpp-httplib/httplib.h ]; then \
		git submodule update --init --recursive; \
	fi
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && (test -f Makefile || cmake $(CURDIR) $(CMAKE_FLAGS))
ifeq ($(SANITIZE),1)
	@cd $(BUILD_DIR) && cmake $(CURDIR) $(CMAKE_FLAGS) > /dev/null
endif
	@$(MAKE) -C $(BUILD_DIR) -j$(NUM_PROCS)

test: all
	@mkdir -p $(TEST_HOME)
	@$(TEST_ENV) ./$(BUILD_DIR)/test_libconfig_parser
	@$(ASAN_PM_ENV) $(TEST_ENV) ./$(BUILD_DIR)/test_process_monitor_lifecycle
	@$(TEST_ENV) ./$(BUILD_DIR)/test_circuit_breaker_quarantine
	@$(TEST_ENV) ./$(BUILD_DIR)/test_service_supervisor
	@$(TEST_ENV) ./$(BUILD_DIR)/test_gemini_triage
	@$(TEST_ENV) ./$(BUILD_DIR)/test_gateway_timeout
	@$(TEST_ENV) ./$(BUILD_DIR)/test_agent_telemetry_db
	@$(TEST_ENV) ./$(BUILD_DIR)/test_mobile_gateway
	@$(TEST_ENV) ./$(BUILD_DIR)/test_web_server_api
	@$(TEST_ENV) ./$(BUILD_DIR)/test_supervisor_api_and_mcp
	@$(TEST_ENV) ./$(BUILD_DIR)/test_cursor_collector
	@$(TEST_ENV) ./$(BUILD_DIR)/test_fleet_integration
	@$(TEST_ENV) ./$(BUILD_DIR)/test_price_catalog
	@$(TEST_ENV) ./$(BUILD_DIR)/test_price_catalog_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_usage_store
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_account
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_collector
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_parser_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_surfaces
ifeq ($(SANITIZE_CANARY),1)
	@$(TEST_ENV) ./$(BUILD_DIR)/test_sanitizer_canary
endif

mobile:
	@if [ -d mobile/android ] && command -v ./mobile/android/gradlew >/dev/null 2>&1; then \
		cd mobile/android && ./gradlew assembleDebug; \
	fi




clean:
	@if [ -d $(BUILD_DIR) ] && [ -f $(BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(BUILD_DIR) clean; \
	fi

distclean:
	@rm -rf $(BUILD_DIR)
