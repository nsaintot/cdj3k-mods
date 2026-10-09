# package/deck/Makefile - build the two deck-side binaries this package ships.
#
#   ep122_shim.so           the LD_PRELOAD shim: the feature mods (glibc, aarch64)
#   stemd_client_aarch64    the STEMS network sidecar   (musl-static, aarch64)
#
# The sources, their flags and the ABI gates are in mods.mk, which any other
# build that links the mods also includes.
#
# Two toolchains (see the musl guard below):
#   make            uses aarch64-linux-musl-gcc if present  -> fine for the sidecar
#   make CC=gcc     what the Docker stages pass (native aarch64 gcc)
#
# musl cannot build the shim. The release build is ../docker/Dockerfile: an
# Alpine (musl) stage for the sidecar and an Ubuntu 18.04 (glibc 2.27) stage for
# the shim. Host builds are for `make check` and quick iteration.

MUSL_CC  := aarch64-linux-musl-gcc
HAS_MUSL := $(shell command -v $(MUSL_CC) 2>/dev/null)

ifdef HAS_MUSL
  CC := $(MUSL_CC)
else
  CC := aarch64-linux-gnu-gcc  # fallback - use the Docker build for a clean shim
endif

# `-mno-outline-atomics` is a GCC 10+ flag; older GCCs error on it. Probe.
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' \
    | $(CC) -x c -mno-outline-atomics -c -o /dev/null - 2>/dev/null \
    && echo -mno-outline-atomics)

# Version stamp, in the two forms mods/core/version.h documents. Overridable
# from the command line so the Docker build can pass the host's git values in
# (the build context has no .git). No checkout gives "unknown".
GIT_DESCRIBE := $(shell git -C $(CURDIR) describe --tags --always --dirty 2>/dev/null)
GIT_TAG      := $(shell git -C $(CURDIR) describe --tags --abbrev=0 2>/dev/null)
MOD_BUILD    ?= $(or $(GIT_DESCRIBE),unknown)
MOD_VERSION  ?= $(if $(GIT_TAG),$(GIT_TAG)$(if $(filter $(GIT_TAG),$(GIT_DESCRIBE)),,+),unknown)

OUT          := out
EP122_SHIM   := $(OUT)/ep122_shim.so
STEMD_CLIENT := $(OUT)/stemd_client_aarch64

MODS_DIR := .
include $(MODS_DIR)/mods.mk

# -static-libgcc: link the libgcc helpers into the .so so it needs no libgcc_s
#   against the deck's glibc 2.17.
# -mno-outline-atomics: emit inline LL/SC atomics rather than helper calls.
MODS_BUILD_CFLAGS := -O2 -Wall -Wextra -Wno-missing-field-initializers \
                     -Werror=implicit-function-declaration -fPIC -shared -static-libgcc \
                     $(NO_OUTLINE_ATOMICS) \
                     -finput-charset=utf-8 -fexec-charset=utf-8 \
                     -DEP122_MOD_VERSION='"$(MOD_VERSION)"' \
                     -DEP122_MOD_BUILD='"$(MOD_BUILD)"' \
                     $(MODS_CFLAGS)

MODS_ABI_TARGET := $(EP122_SHIM)

# Static tools: fully self-contained binaries.
TOOL_CFLAGS := -O2 -Wall -Wextra -static -I$(MODS_DIR)/shared

.PHONY: all shim agent tools clean purity check abi-check

all: $(OUT) $(EP122_SHIM) $(STEMD_CLIENT)

# Convenience aliases so the Docker stages can request one half.
shim:  $(OUT) $(EP122_SHIM)
agent tools: $(OUT) $(STEMD_CLIENT)

$(OUT):
	mkdir -p $(OUT)

# --- shim ---
# A musl-linked .so records `NEEDED: libc.so`; the deck has no musl, so glibc's
# ld.so refuses it in every process including /bin/sh: apl_start.sh dies with
# status 127 and the deck comes up with no app.
$(EP122_SHIM): $(MODS_OBJS)
	@case '$(CC)' in *musl*) \
	  echo "ERROR: $(CC) links the shim against musl (NEEDED: libc.so)."; \
	  echo "       The deck is glibc 2.17 and its ld.so will refuse it,"; \
	  echo "       taking EP122 and /bin/sh down with it. Build via ../docker."; \
	  exit 1;; esac
	$(CC) $(MODS_BUILD_CFLAGS) $(MODS_LDFLAGS) -o $@ $^ $(MODS_LDLIBS)
	@echo "Built: $@"
	@command -v file >/dev/null && file $@ || true

# --- stem sidecar (static) ---
$(STEMD_CLIENT): $(MODS_STEMD_SRCS) $(MODS_STEMD_HDRS) | $(OUT)
	$(CC) $(TOOL_CFLAGS) -o $@ $(MODS_STEMD_SRCS)
	@echo "Built: $@"
	@command -v file >/dev/null && file $@ || true

# --- host tests ---
HOST_CC ?= cc
# Silence macOS SDK nullability annotations and the deprecated syscall(2);
# neither concerns the code under test.
HOST_QUIET := $(shell printf 'int main(void){return 0;}\n' \
    | $(HOST_CC) -x c -Wno-nullability-completeness -Wno-deprecated-declarations \
      -c -o /dev/null - 2>/dev/null \
    && echo -Wno-nullability-completeness -Wno-deprecated-declarations)
HOST_CFLAGS := -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(HOST_QUIET) \
               -DEP122_MOD_VERSION='"$(MOD_VERSION)"' \
               -DEP122_MOD_BUILD='"$(MOD_BUILD)"' \
               $(MODS_INCLUDES)

TEST_BINS := $(MODS_TEST_SRCS:$(MODS_DIR)/tests/%.c=$(OUT)/tests/%)

$(OUT)/tests/%: $(MODS_DIR)/tests/%.c $(MODS_DIR)/tests/test.h $(MODS_TEST_STUBS) \
                $(MODS_PURE_SRCS) $(MODS_TEST_DEPS) $(MODS_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(MODS_TEST_STUBS) \
	    $(MODS_PURE_SRCS) $(MODS_TEST_DEPS) -lm

check: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do "$$t" || fail=1; done; \
	[ $$fail -eq 0 ] || { echo "check: FAILED"; exit 1; }

purity:    mods-purity
abi-check: $(EP122_SHIM) mods-abi-check

clean:
	rm -rf $(OUT)
