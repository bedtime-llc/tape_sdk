# TAPP SDK Makefile
# Convenience wrapper for building TAPP applications

.PHONY: help init example clean all

# Name for `make init`. Only ever used as the folder name, the .c filename and the app's
# display .name — never as a C identifier, so a name like my-app is fine.
NAME ?= my_tapp

# Targets that must not be shadowed by the $(eval) below.
RESERVED := help init check example build clean all

# `make init my_app` — Make parses `my_app` as a second GOAL and would fail with "No rule to make
# target". Capture it as the name and give it a do-nothing rule so Make is satisfied.
# `make init NAME=my_app` still works: an assignment is not a goal, so MAKECMDGOALS is just "init"
# and none of this fires.
ifeq (init,$(firstword $(MAKECMDGOALS)))
  INIT_ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))
  ifneq ($(INIT_ARGS),)
    NAME := $(firstword $(INIT_ARGS))
    INIT_FROM_GOAL := 1
    # Only neutralise words that are NOT real targets — otherwise `make init clean` would replace
    # the real clean rule with a no-op for this invocation. Reserved names are rejected in the
    # recipe with a proper message instead.
    NEUTRALISE := $(filter-out $(RESERVED),$(INIT_ARGS))
    ifneq ($(NEUTRALISE),)
      # .PHONY as well: the scaffolded directory now exists, so without it Make reports
      # "'my_app' is up to date" straight after creating it, which reads like an error.
      $(eval .PHONY: $(NEUTRALISE))
      $(eval $(NEUTRALISE):;@:)
    endif
  endif
endif

# Default target
help:
	@echo "TAPP SDK Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make init my_app  - Scaffold a new tapp in my_app/"
	@echo "  make example      - Build example application"
	@echo "  make clean        - Remove built .tapp files"
	@echo "  make check        - Check dependencies"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Custom build:"
	@echo "  make APP=my_app.c - Build custom app"
	@echo ""
	@echo "Or use directly:"
	@echo "  ./tapp-build my_app.c"
	@echo ""
	@echo "Toolchain: clang + ld.lld"

# Scaffold a new tapp:  make init NAME=my_app
#
# Creates a DIRECTORY rather than a bare .c so that assets/ and inc/ can be added later without
# moving anything — tapp-build takes a directory and names the app after it.
init:
	@case '$(NAME)' in \
	  ''|*[!A-Za-z0-9_-]*) \
	    echo "❌ name must be non-empty and only [A-Za-z0-9_-]: '$(NAME)'"; \
	    echo "   e.g. make init my_app"; \
	    exit 1;; \
	esac
	@# Only for the positional form: `make init clean` reads as two goals and is ambiguous. The
	@# NAME= form has no such ambiguity, so this list is empty there and the escape hatch is real.
	@for r in $(if $(INIT_FROM_GOAL),$(RESERVED),); do \
	    if [ "$$r" = '$(NAME)' ]; then \
	        echo "❌ '$(NAME)' is a make target — pick another name,"; \
	        echo "   or force it with: make init NAME=$(NAME)"; \
	        exit 1; \
	    fi; \
	done
	@# Never clobber: scaffolding is a one-shot and the directory may hold real work.
	@if [ -e '$(NAME)' ]; then \
	    echo "❌ '$(NAME)' already exists — refusing to overwrite"; \
	    ls -ld '$(NAME)'; \
	    exit 1; \
	fi
	@mkdir -p '$(NAME)'
	@sed 's|@NAME@|$(NAME)|g' tools/tapp_skeleton.c.in > '$(NAME)/$(NAME).c'
	@echo "✓ created $(NAME)/$(NAME).c"
	@echo ""
	@echo "  ./tapp-build $(NAME)      # -> $(NAME).tapp"
	@echo "  then copy $(NAME).tapp into the 'apps' folder on the SD card (disk mode)"

# Check dependencies
check:
	@echo "Checking dependencies..."
	@# Probe the whole toolchain, not just clang. Homebrew's llvm kegs are not symlinked into PATH,
	@# so on stock macOS `command -v clang` finds Apple's clang (which has no ld.lld next to it) and
	@# a clang-only check reports success in exactly the setup where the build then dies.
	@tapp_probe() { \
	    for d in "" /opt/homebrew/opt/llvm@18/bin/ /opt/homebrew/opt/llvm/bin/ \
	             /usr/local/opt/llvm@18/bin/ /usr/local/opt/llvm/bin/; do \
	        if command -v "$${d}clang" >/dev/null 2>&1 && command -v "$${d}ld.lld" >/dev/null 2>&1; then \
	            echo "$${d}"; return 0; \
	        fi; \
	    done; \
	    return 1; \
	}; \
	dir=$$(tapp_probe) || { \
	    echo "❌ need clang AND ld.lld together"; \
	    command -v clang >/dev/null 2>&1 && echo "   (found clang, but no ld.lld beside it or on PATH)"; \
	    echo "   Install: brew install llvm (macOS — keg-only, tapp-build finds it automatically)"; \
	    echo "   Install: sudo apt-get install clang lld llvm (Linux)"; \
	    exit 1; \
	}; \
	echo "✓ clang + ld.lld found: $$($${dir}clang --version | head -1)"
	@# v7 is `magick`, v6 is `convert`/`identify`. Probing only for `magick` reported "not found"
	@# on every stock Debian/Ubuntu — which then failed in the asset step anyway, having said the
	@# dependency was missing when it was present.
	@if command -v magick >/dev/null 2>&1; then \
		echo "✓ ImageMagick found: $$(magick --version | head -1)"; \
	elif command -v convert >/dev/null 2>&1 && command -v identify >/dev/null 2>&1; then \
		echo "✓ ImageMagick found: $$(convert --version | head -1)"; \
	else \
		echo "⚠️  ImageMagick not found (optional, needed only for assets)"; \
		echo "   Install: brew install imagemagick (macOS)"; \
		echo "   Install: sudo apt-get install imagemagick (Linux)"; \
	fi
	@echo ""
	@echo "✓ All required dependencies installed"

# Build example app
example: check
	@echo "Building example app..."
	@./tapp-build examples/simple_app.c

# Build custom app
ifdef APP
build: check
	@echo "Building $(APP)..."
	@./tapp-build $(APP)
endif

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -f *.tapp
	@rm -f examples/*.tapp
	@# Scaffolded apps build into their own folder. Only ever removes .tapp outputs, never sources.
	@rm -f */*.tapp
	@echo "✓ Clean complete"

# Build all examples
all: check
	@echo "Building all examples..."
	@for example in examples/*.c; do \
		echo "Building $$example..."; \
		./tapp-build $$example; \
	done
	@echo "✓ All examples built"
