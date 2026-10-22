##
## Makefile
##
## Copyright (C) 2026, Charles Chiou
##

SHELL := /bin/bash
BUILD_DIR := build
NUM_PROCS := $(shell nproc 2>/dev/null || echo 4)
CMAKE := $(shell if [ -x /usr/bin/cmake ]; then echo /usr/bin/cmake; else echo cmake; fi)

# The tests run with HOME pointing at an empty directory under the build tree.
TEST_HOME = $(CURDIR)/$(BUILD_DIR)/test-home
TEST_ENV = HOME=$(TEST_HOME)

# SANITIZE=1 builds and runs the test suite under AddressSanitizer and UBSan in
# a separate build directory (inside build/, which is already ignored).
# SANITIZE_CANARY=1 additionally runs a deliberately broken test that must make
# the sanitized run fail, proving the sanitizers are active.
SANITIZE ?= 0
SANITIZE_CANARY ?= 0
ifneq (,$(findstring -fsanitize,$(CFLAGS) $(CXXFLAGS)))
SANITIZE := 1
endif
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Release
ifeq ($(SANITIZE),1)
BUILD_DIR := build/asan
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Debug -DAIMON_SANITIZE=ON \
	-DAIMON_SANITIZE_CANARY=$(if $(filter 1,$(SANITIZE_CANARY)),ON,OFF)
endif
ifeq ($(SANITIZE_CANARY),1)
ifneq ($(SANITIZE),1)
$(error SANITIZE_CANARY=1 requires SANITIZE=1)
endif
endif

.PHONY: all clean distclean test mobile windows-eda-bridge windows-easyeda-bridge

all:
	@if [ -f .gitmodules ] && [ ! -f third_party/cpp-httplib/httplib.h ]; then \
		git submodule update --init --recursive; \
	fi
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && (test -f Makefile || $(CMAKE) $(CURDIR) $(CMAKE_FLAGS))
ifeq ($(SANITIZE),1)
	@cd $(BUILD_DIR) && $(CMAKE) $(CURDIR) $(CMAKE_FLAGS) > /dev/null
endif
	@$(MAKE) -C $(BUILD_DIR) -j$(NUM_PROCS)

test: all
	@mkdir -p $(TEST_HOME)
	@$(TEST_ENV) ./$(BUILD_DIR)/test_libconfig_parser
	@$(TEST_ENV) ./$(BUILD_DIR)/test_gateway_timeout
	@$(TEST_ENV) ./$(BUILD_DIR)/test_agent_telemetry_db
	@$(TEST_ENV) ./$(BUILD_DIR)/test_mobile_gateway
	@$(TEST_ENV) ./$(BUILD_DIR)/test_web_server_api
	@$(TEST_ENV) ./$(BUILD_DIR)/test_cursor_collector
	@$(TEST_ENV) ./$(BUILD_DIR)/test_price_catalog
	@$(TEST_ENV) ./$(BUILD_DIR)/test_price_catalog_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_usage_store
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_account
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_collector
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_parser_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_claude_surfaces
	@$(TEST_ENV) ./$(BUILD_DIR)/test_insight_engine
	@$(TEST_ENV) ./$(BUILD_DIR)/test_insight_parser_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_insight_surfaces
	@$(TEST_ENV) ./$(BUILD_DIR)/test_eda_bridge
	@$(TEST_ENV) ./$(BUILD_DIR)/test_eda_bridge_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_eda_bridge_daemon
	@$(TEST_ENV) ./$(BUILD_DIR)/test_easyeda_converter_props
	@$(TEST_ENV) ./$(BUILD_DIR)/test_eda_surfaces
ifeq ($(SANITIZE_CANARY),1)
	@$(TEST_ENV) ./$(BUILD_DIR)/test_sanitizer_canary
endif

mobile:
	@if [ -d third_party/antimatter/android ] && [ -x third_party/antimatter/android/gradlew ]; then \
		if [ -d "$$HOME/.jdks/jdk-21.0.4+7" ]; then export JAVA_HOME="$$HOME/.jdks/jdk-21.0.4+7"; fi; \
		cd third_party/antimatter/android && ./gradlew assembleDebug testDebugUnitTest; \
	fi

windows-eda-bridge:
	@mkdir -p build/windows-eda-bridge
	@cd build/windows-eda-bridge && $(CMAKE) $(CURDIR)/tools/eda-bridge \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_SYSTEM_NAME=Windows \
		-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
		-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
		-DAIMON_EDA_BRIDGE_WITH_QT=ON
	@$(MAKE) -C build/windows-eda-bridge -j$(NUM_PROCS)
	@python3 $(CURDIR)/tools/eda-bridge/scripts/pack_windows_installer.py \
		--root-dir $(CURDIR) \
		--build-dir $(CURDIR)/build/windows-eda-bridge \
		--qt-prefix $(CURDIR)/build/qt-prebuilt/6.8.3/mingw_64 \
		--output-dir $(CURDIR)/build/windows

windows-easyeda-bridge: windows-eda-bridge

clean:
	@if [ -d $(BUILD_DIR) ] && [ -f $(BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(BUILD_DIR) clean; \
	fi
	@if [ -d build/windows-eda-bridge ] && [ -f build/windows-eda-bridge/Makefile ]; then \
		$(MAKE) -C build/windows-eda-bridge clean; \
	fi

distclean:
	@rm -rf $(BUILD_DIR) build/windows-eda-bridge build/windows
