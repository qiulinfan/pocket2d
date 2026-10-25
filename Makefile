# Top-level convenience wrapper for CMake presets.
# Do not copy generated build/*/Makefile into the repo.

CMAKE ?= cmake
MKDOCS ?= mkdocs
RSYNC ?= rsync
JOBS ?= 4

DEBUG_PRESET ?= unix-makefiles-debug
RELEASE_PRESET ?= unix-makefiles-release
SITE_BUILD_DIR := .site

DEBUG_BUILD_DIR := build/$(DEBUG_PRESET)
RUNTIME_BIN := $(DEBUG_BUILD_DIR)/game
EDITOR_BIN := $(DEBUG_BUILD_DIR)/pocket

.DEFAULT_GOAL := build

.PHONY: help configure build configure-release build-release
.PHONY: run run-editor compile-commands
.PHONY: test
.PHONY: docs-build docs-serve site-build docs
.PHONY: clean clean-all

configure:
	$(CMAKE) --preset $(DEBUG_PRESET)

build: configure
	$(CMAKE) --build --preset $(DEBUG_PRESET) -j$(JOBS)

configure-release:
	$(CMAKE) --preset $(RELEASE_PRESET)

build-release: configure-release
	$(CMAKE) --build --preset $(RELEASE_PRESET) -j$(JOBS)

# Executables stay in the build tree; run them from the repository root, where
# they find the engine's .engine resources.
run: build
	./$(RUNTIME_BIN)

run-editor: build
	./$(EDITOR_BIN)

test: build
	ctest --test-dir $(DEBUG_BUILD_DIR) --output-on-failure

compile-commands: configure
	cp $(DEBUG_BUILD_DIR)/compile_commands.json ./compile_commands.json

docs-build:
	$(MKDOCS) build --clean

docs-serve:
	$(MKDOCS) serve

site-build:
	rm -rf $(SITE_BUILD_DIR)
	mkdir -p $(SITE_BUILD_DIR)
	$(RSYNC) -av --delete --exclude 'lua-api/' --exclude '*.md' docs/ $(SITE_BUILD_DIR)/
	$(MKDOCS) build --clean

# Pages deploys from the published main branch, not from this checkout.
docs: site-build
	gh workflow run deploy-pages.yml --ref main

clean:
	-$(CMAKE) --build --preset $(DEBUG_PRESET) --target clean
	-$(CMAKE) --build --preset $(RELEASE_PRESET) --target clean

clean-all:
	rm -rf build/$(DEBUG_PRESET) build/$(RELEASE_PRESET)
