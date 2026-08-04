# TAPP SDK Makefile
# Convenience wrapper for building TAPP applications

.PHONY: help example clean all

# Default target
help:
	@echo "TAPP SDK Build System"
	@echo ""
	@echo "Targets:"
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
	@echo "✓ Clean complete"

# Build all examples
all: check
	@echo "Building all examples..."
	@for example in examples/*.c; do \
		echo "Building $$example..."; \
		./tapp-build $$example; \
	done
	@echo "✓ All examples built"
