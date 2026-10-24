# Convenience wrapper around cmake / ctest. Run `make help` for targets.

BUILD_DIR     ?= build/default
BUILD_TYPE    ?= Release
JOBS          ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
CMAKE         ?= cmake
CTEST         ?= ctest
CMAKE_FLAGS   ?=
TARGET        ?=
CTEST_TIMEOUT ?= 120
EXAMPLE       ?= 01_first_machine
BENCH_ARGS    ?= --benchmark_min_time=1s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
VERBOSE       ?= 0

# Debug logging via spdlog (off by default).
#   LOGS=1            -> STEPWRIGHT_ENABLE_LOGGING=ON
#   LOG_LEVEL=<level> -> TRACE | DEBUG | INFO | WARN | ERROR | CRITICAL | OFF
LOGS      ?= 0
LOG_LEVEL ?= DEBUG
LOGGING_FLAGS := $(if $(filter-out 0,$(LOGS)),-DSTEPWRIGHT_ENABLE_LOGGING=ON -DSTEPWRIGHT_LOG_LEVEL=$(LOG_LEVEL),-DSTEPWRIGHT_ENABLE_LOGGING=OFF)

FETCHCONTENT_QUIET ?= ON
CONFIGURE_FLAGS := -DFETCHCONTENT_QUIET=$(FETCHCONTENT_QUIET) $(LOGGING_FLAGS) $(CMAKE_FLAGS)
BUILD_VERBOSE   := $(if $(filter-out 0,$(VERBOSE)),--verbose,)

COVERAGE_ARGS ?=
TOOLS         ?= all

.DEFAULT_GOAL := help
.PHONY: help install-tools configure build test examples run debug release trace clean \
        format format-check tidy cppcheck forbidden-check benchmarks coverage \
        asan tsan ubsan leak sanitizers

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-16s\033[0m %s\n", $$1, $$2}'

install-tools: ## Install the dev toolchain (TOOLS=build|format|tidy|cppcheck|coverage|sanitizers|all)
	@bash scripts/install_toolchain.sh $(TOOLS)

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CONFIGURE_FLAGS)

configure: $(BUILD_DIR)/CMakeCache.txt ## Configure the build directory

build: configure ## Build everything (TARGET=<name> for one target)
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS) $(BUILD_VERBOSE) $(if $(TARGET),--target $(TARGET))

test: build ## Build and run the test suite
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --timeout $(CTEST_TIMEOUT) -j $(JOBS)

examples: build ## Build all examples

run: build ## Run an example (EXAMPLE=<name>)
	./$(BUILD_DIR)/examples/$(EXAMPLE)

debug: ## Debug build
	$(MAKE) build BUILD_TYPE=Debug BUILD_DIR=build/debug

release: ## Release build
	$(MAKE) build BUILD_TYPE=Release

trace: ## Debug build with TRACE-level logging into build/trace
	$(MAKE) build BUILD_TYPE=Debug BUILD_DIR=build/trace LOGS=1 LOG_LEVEL=TRACE

clean: ## Remove all build directories
	rm -rf build

benchmarks: ## Build (Release) and run every benchmark binary
	$(MAKE) build BUILD_TYPE=Release BUILD_DIR=build/bench
	@for b in build/bench/benchmarks/bench_*; do \
		[ -x "$$b" ] || continue; \
		echo "=== $$(basename $$b)"; \
		"$$b" $(BENCH_ARGS) || exit 1; \
	done

format: ## Format sources in place
	@bash scripts/format-code.sh

format-check: ## Check formatting (CI mode)
	@bash scripts/format-code.sh --check

tidy: ## Build tests with clang-tidy enabled
	$(MAKE) build BUILD_TYPE=Debug BUILD_DIR=build/tidy CMAKE_FLAGS="-DSTEPWRIGHT_CLANG_TIDY=ON -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF $(CMAKE_FLAGS)"

cppcheck: configure ## Project-wide cppcheck scan
	@bash scripts/run-cppcheck.sh -b $(BUILD_DIR)

forbidden-check: ## Scan library headers for forbidden patterns
	@bash scripts/check-forbidden-usage.sh

asan: ## Build + test with AddressSanitizer + UBSan
	$(MAKE) build BUILD_DIR=build/asan BUILD_TYPE=Debug CMAKE_FLAGS="-DCODE_INSTRUMENTATION=ASAN -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF $(CMAKE_FLAGS)"
	$(CTEST) --test-dir build/asan --output-on-failure --timeout $(CTEST_TIMEOUT) -j $(JOBS)

tsan: ## Build + test with ThreadSanitizer
	$(MAKE) build BUILD_DIR=build/tsan BUILD_TYPE=Debug CMAKE_FLAGS="-DCODE_INSTRUMENTATION=TSAN -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF $(CMAKE_FLAGS)"
	$(CTEST) --test-dir build/tsan --output-on-failure --timeout $(CTEST_TIMEOUT) -j $(JOBS)

ubsan: ## Build + test with UndefinedBehaviorSanitizer
	$(MAKE) build BUILD_DIR=build/ubsan BUILD_TYPE=Debug CMAKE_FLAGS="-DCODE_INSTRUMENTATION=UBSAN -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF $(CMAKE_FLAGS)"
	$(CTEST) --test-dir build/ubsan --output-on-failure --timeout $(CTEST_TIMEOUT) -j $(JOBS)

leak: ## Build + test with LeakSanitizer (ASan on macOS)
	$(MAKE) build BUILD_DIR=build/leak BUILD_TYPE=Debug CMAKE_FLAGS="-DCODE_INSTRUMENTATION=LEAK -DSTEPWRIGHT_BUILD_BENCHMARKS=OFF $(CMAKE_FLAGS)"
	ASAN_OPTIONS=detect_leaks=1 $(CTEST) --test-dir build/leak --output-on-failure --timeout $(CTEST_TIMEOUT) -j $(JOBS)

sanitizers: asan tsan ## Run ASan+UBSan and TSan suites

coverage: ## HTML coverage report (requires lcov)
	@python3 scripts/coverage.py --jobs $(JOBS) --timeout $(CTEST_TIMEOUT) $(COVERAGE_ARGS)
