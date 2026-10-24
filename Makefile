# stepwright developer entry points.
#
#   make <task>          run natively on this machine (macOS)
#   make docker-<task>   run inside the pinned Linux dev container
#
# Formatting, clang-tidy, cppcheck, coverage and LeakSanitizer always run in
# the container so every contributor uses the same tool versions.
#
# Variables:
#   COMPILER=native|gcc|clang|clang-libcxx   (container default: gcc)
#   BUILD_TYPE=Release|Debug                  (default: Release)
#   PLATFORM=linux/arm64|linux/amd64          (container only; default: host arch)
#   EXAMPLE=<name>  BENCH_ARGS="..."  CMAKE_ARGS="..."  JOBS=<n>

COMPILER   ?=
BUILD_TYPE ?= Release
PLATFORM   ?=
EXAMPLE    ?=
BENCH_ARGS ?=
CMAKE_ARGS ?=
JOBS       ?=
STEPWRIGHT_STRESS_SCALE ?= 1

CI := bash scripts/ci.sh
export COMPILER BUILD_TYPE EXAMPLE BENCH_ARGS CMAKE_ARGS JOBS
export STEPWRIGHT_STRESS_SCALE

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
	-e STEPWRIGHT_STRESS_SCALE \
	$(IMAGE)

TASKS := build test debug asan tsan ubsan bench run check
CONTAINER_ONLY := format format-check tidy cppcheck coverage leak forbidden
DOCKER_TASKS := $(addprefix docker-,$(TASKS) $(CONTAINER_ONLY))

.DEFAULT_GOAL := help
.PHONY: help clean $(TASKS) $(CONTAINER_ONLY) $(DOCKER_TASKS) \
	docker-image docker-shell docker-clean

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

docker-shell: docker-image ## Interactive shell in the dev container
	@$(DOCKER_RUN) bash

docker-clean: ## Delete the container build volume
	-$(DOCKER) volume rm $(BUILD_VOLUME)

$(DOCKER_TASKS): docker-%: docker-image
	@$(DOCKER_RUN) bash scripts/ci.sh $*
