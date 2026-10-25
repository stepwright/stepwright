# stepwright developer entry points.
#
#   make <task>          run natively on this machine (macOS)
#   make docker-<task>   run inside the pinned Linux dev container
#
# Formatting, clang-tidy, cppcheck, coverage and LeakSanitizer always run in
# the container so every contributor uses the same tool versions.
#
# Variables:
#   COMPILER=native|gcc|clang|clang-libcxx|gcc13|clang18   (container default: gcc)
#   BUILD_TYPE=Release|Debug                  (default: Release)
#   PLATFORM=linux/arm64|linux/amd64          (container only; default: host arch)
#   EXAMPLE=<name>  BENCH_ARGS="..."  CMAKE_ARGS="..."  JOBS=<n>
#   BENCH_OUT=<dir>  (bench) also write JSON results there, relative to the repo
#                    root; scripts/bench_report.py turns them into tables
#   BENCH_LABEL="..." (bench) platform label stored with the results

COMPILER   ?=
BUILD_TYPE ?= Release
PLATFORM   ?=
EXAMPLE    ?=
BENCH_ARGS ?=
BENCH_OUT  ?=
BENCH_LABEL ?=
CMAKE_ARGS ?=
JOBS       ?=
STEPWRIGHT_STRESS_SCALE ?= 1

CI := bash scripts/ci.sh
# Recorded with benchmark results (the container cannot see the host's git or CPU).
GIT_COMMIT := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
HOST_CPU   := $(shell sysctl -n machdep.cpu.brand_string 2>/dev/null || sed -n 's/^model name.*: //p' /proc/cpuinfo 2>/dev/null | head -1)
HOST_LOAD  := $(shell uptime | sed -E 's/.*load averages?: //')

export COMPILER BUILD_TYPE EXAMPLE BENCH_ARGS BENCH_OUT BENCH_LABEL CMAKE_ARGS JOBS
export STEPWRIGHT_STRESS_SCALE GIT_COMMIT

# ---------------------------------------------------------------------------
# Docker plumbing
# ---------------------------------------------------------------------------
DOCKER        ?= docker
IMAGE_HASH    := $(shell (shasum -a 256 docker/Dockerfile 2>/dev/null || sha256sum docker/Dockerfile) | cut -c1-12)
PLATFORM_TAG  := $(if $(PLATFORM),$(subst /,-,$(PLATFORM)),host)
IMAGE         := stepwright-dev:$(IMAGE_HASH)-$(PLATFORM_TAG)
BUILD_VOLUME  := stepwright-build-$(PLATFORM_TAG)
TTY_FLAG      := $(shell [ -t 0 ] && echo -it)
USER_FLAG     := $(if $(filter Linux,$(shell uname -s)),--user $(shell id -u):$(shell id -g),)

DOCKER_RUN = $(DOCKER) run --rm $(TTY_FLAG) $(USER_FLAG) \
	$(if $(PLATFORM),--platform $(PLATFORM)) \
	--cap-add=SYS_PTRACE \
	-v "$(CURDIR)":/work/src \
	-v $(BUILD_VOLUME):/work/build \
	-e BUILD_ROOT=/work/build -e HOME=/tmp \
	-e COMPILER -e BUILD_TYPE -e EXAMPLE -e BENCH_ARGS -e CMAKE_ARGS -e JOBS \
	-e BENCH_OUT -e BENCH_LABEL -e GIT_COMMIT -e BENCH_HOST_CPU="$(HOST_CPU) (container)" \
	-e BENCH_HOST_LOAD="$(HOST_LOAD)" \
	-e STEPWRIGHT_STRESS_SCALE \
	-e STEPWRIGHT_TRACKED_FILES=/work/src/build/stepwright-tracked-files.list \
	$(IMAGE)

TASKS := build test debug asan tsan ubsan bench run check consumer
CONTAINER_ONLY := format format-check tidy cppcheck coverage leak forbidden docs
DOCKER_TASKS := $(addprefix docker-,$(TASKS) $(CONTAINER_ONLY))

.DEFAULT_GOAL := help
.PHONY: help clean $(TASKS) $(CONTAINER_ONLY) $(DOCKER_TASKS) \
	docker-image docker-shell docker-clean docker-tracked-files

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'
	@echo
	@echo "Native tasks:     $(TASKS)"
	@echo "Container tasks:  $(addprefix docker-,$(TASKS) $(CONTAINER_ONLY))"

# ---------------------------------------------------------------------------
# Native (macOS)
# ---------------------------------------------------------------------------
build:   ; @$(CI) build    ## (native) configure + build
test:    ; @$(CI) test     ## (native) build + run tests
debug:   ; @$(CI) debug    ## (native) Debug build + tests
asan:    ; @$(CI) asan     ## (native) ASan + UBSan build + tests
tsan:    ; @$(CI) tsan     ## (native) ThreadSanitizer build + tests
ubsan:   ; @$(CI) ubsan    ## (native) UBSan build + tests
bench:   ; @$(CI) bench    ## (native) Release benchmarks
run:     ; @$(CI) run      ## (native) run EXAMPLE=<name>
check:   ; @$(CI) check    ## (native) forbidden + release + debug + asan + tsan
consumer: ; @$(CI) consumer ## (native) relocated install + FetchContent + subdirectory

# Tool-version-sensitive tasks are redirected to the container.
$(CONTAINER_ONLY): %: docker-%
	@true

clean: ## Remove native build trees (build/)
	rm -rf build

# ---------------------------------------------------------------------------
# Linux container
# ---------------------------------------------------------------------------
docker-image: ## Build the dev image if it does not exist yet
	@$(DOCKER) image inspect $(IMAGE) >/dev/null 2>&1 || \
		$(DOCKER) build $(if $(PLATFORM),--platform $(PLATFORM)) -t $(IMAGE) -f docker/Dockerfile docker

docker-shell: docker-image docker-tracked-files ## Interactive shell in the dev container
	@$(DOCKER_RUN) bash

# Keep a linked worktree's parent Git directory outside the container boundary.
# Only tasks which scan tracked files need this manifest; ordinary archive builds
# and benchmarks do not require repository metadata.
docker-tracked-files:
	@mkdir -p build
	@git ls-files -z > build/stepwright-tracked-files.list

docker-forbidden docker-check: docker-tracked-files

docker-clean: ## Delete the container build volume
	-$(DOCKER) volume rm $(BUILD_VOLUME)

$(DOCKER_TASKS): docker-%: docker-image
	@$(DOCKER_RUN) bash scripts/ci.sh $*
