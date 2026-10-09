# mods.mk - the mod sources, the flags they need, and the gates that guard them.
#
# Included by every build that links the mods, this package's deck build among
# them. The includer sets MODS_DIR to the directory holding this file and OUT to
# its own output root, then includes it:
#
#     MODS_DIR := path/to/cdj3k-mods/package/deck
#     include $(MODS_DIR)/mods.mk
#
# The includer also sets, after the include:
#     MODS_BUILD_CFLAGS   the full compile line for a mod object
#     MODS_ABI_TARGET     the linked .so that mods-abi-check inspects
#
# One source list and one flag set, so a new mod file reaches every build and
# the purity, test and ABI manifests stay in sync.

ifndef MODS_DIR
$(error mods.mk: set MODS_DIR to the directory holding this file)
endif

# --- sources ---
# Found by wildcard, so a new file or feature directory needs no edit here.
# The globs reach two directory levels below mods/ (mods/stem/ui/), as deep
# as the tree goes.
#
# mods/ holds only shipped code. Host harnesses must stay outside it: a harness
# #includes a mod's .c, which would define every symbol in it twice.
MODS_C_SRCS := $(wildcard $(MODS_DIR)/mods/*.c) \
               $(wildcard $(MODS_DIR)/mods/*/*.c) \
               $(wildcard $(MODS_DIR)/mods/*/*/*.c)

MODS_CXX_SRCS := $(wildcard $(MODS_DIR)/mods/*.cc) \
                 $(wildcard $(MODS_DIR)/mods/*/*.cc) \
                 $(wildcard $(MODS_DIR)/mods/*/*/*.cc)

MODS_SRCS := $(MODS_C_SRCS) $(MODS_CXX_SRCS)

# Every .c depends on every mod header. Coarse but cheap: the build takes
# seconds, and a missed dependency would ship a stale object.
MODS_HDRS := $(wildcard $(MODS_DIR)/mods/*.h)    $(wildcard $(MODS_DIR)/mods/*.hh) \
             $(wildcard $(MODS_DIR)/mods/*/*.h)  $(wildcard $(MODS_DIR)/mods/*/*/*.h) \
             $(wildcard $(MODS_DIR)/mods/*/*.hh) $(wildcard $(MODS_DIR)/mods/*/*/*.hh) \
             $(wildcard $(MODS_DIR)/shared/*.h)

# The stem sidecar: the network half of STEMS, a separate binary so a wedged
# HTTP client cannot take down the audio process. It shares
# shared/stem_proto.h with the mods, so the two ship as a versioned pair.
MODS_STEMD_SRCS := $(wildcard $(MODS_DIR)/stemd_client/*.c)
MODS_STEMD_HDRS := $(wildcard $(MODS_DIR)/stemd_client/*.h) \
                   $(wildcard $(MODS_DIR)/shared/*.h)

# --- flags ---
# -I$(MODS_DIR) resolves "mods/...", the path the host's syscall interposers
# and the tests use. -I$(MODS_DIR)/mods makes includes relative to the mod
# tree root, so "kit/mod.h" reads the same from any file in it.
MODS_INCLUDES := -I$(MODS_DIR) -I$(MODS_DIR)/mods -I$(MODS_DIR)/shared

# expf and exp2f got new default versions in glibc 2.27, so an ordinary link on
# 2.27 stamps expf@GLIBC_2.27. The deck's 2.17 ld.so refuses that, and since
# this is an LD_PRELOAD every process apl_start.sh starts fails, /bin/sh
# included. The .symver in compat/glibc217_compat.h rewrites both references to
# the 2.17 symbols; it must be in the TU that references them, hence the
# force-include.
#
# Keep this paired with -lm in MODS_LDLIBS: -lm is what makes the linker
# resolve expf against libm and stamp a version at all.
MODS_COMPAT := -include $(MODS_DIR)/compat/glibc217_compat.h

MODS_CFLAGS := $(MODS_INCLUDES) $(MODS_COMPAT)

# The mods call libm (wave/codec.c's high-band curve) and libpthread (the stem
# worker and analysis threads), which only merged into libc in glibc 2.34.
# Without them the .so still links, with undefined symbols and no DT_NEEDED to
# supply them. That works under lazy binding in EP122 but fails in any
# BIND_NOW binary in the preload path: "symbol lookup error: undefined symbol:
# pthread_detach".
MODS_LDLIBS := -lm -lpthread

# --no-undefined turns a missing library into a link error instead of a deck
# that boots to nothing. Weak undefined symbols stay legal: the db mod's
# optional sqlite3_* imports are weak and resolve from EP122 at runtime.
MODS_LDFLAGS := -Wl,--no-undefined

# The mods export nothing. The object is LD_PRELOADed, so every global symbol
# interposes that name in EP122 and every library it loads. -Wmissing-prototypes
# enforces the rule in the source: a mods/ function that is neither static nor
# header-declared fails the build.
MODS_OBJ_CFLAGS := -fvisibility=hidden -Wmissing-prototypes

# --- C++ ---
# The mods build as C++ in a minimal profile: constructors, destructors,
# references and templates. No exceptions, no RTTI, no STL.
#
# -fno-exceptions       a hook is entered from EP122's own frames; an exception
#                       escaping one would unwind through code this does not own.
# -fno-rtti             the mods walk EP122's RTTI, not their own.
# -fno-threadsafe-statics  keeps __cxa_guard_acquire out, so no libstdc++.
#
# Nothing here uses the STL, so NEEDED is unchanged by a .cc file. mods-abi-check
# fails the build if that stops holding.
#
# Derive the C++ compiler from CC. `?=` does not work here: make always defines
# CXX (origin `default`), so a cross build would compile .cc files with the host
# g++. A CXX from the environment or the command line is left alone.
ifeq ($(filter environment command\ line,$(origin CXX)),)
CXX := $(CC:gcc=g++)
endif

MODS_CXX_PROFILE := -std=gnu++14 -fno-exceptions -fno-rtti -fno-threadsafe-statics

# -Wmissing-declarations is the C++ equivalent of -Wmissing-prototypes.
MODS_OBJ_CXXFLAGS := -fvisibility=hidden -Wmissing-declarations

MODS_OBJS := $(patsubst $(MODS_DIR)/%.c,$(OUT)/%.o,$(MODS_C_SRCS)) \
             $(patsubst $(MODS_DIR)/%.cc,$(OUT)/%.o,$(MODS_CXX_SRCS))

$(OUT)/%.o: $(MODS_DIR)/%.c $(MODS_HDRS) | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) $(MODS_BUILD_CFLAGS) $(MODS_OBJ_CFLAGS) -c -o $@ $<

# -Werror=implicit-function-declaration is C-only; g++ rejects it on the command
# line. C++ has no implicit declarations to guard against.
MODS_CXX_DROP := -Werror=implicit-function-declaration

$(OUT)/%.o: $(MODS_DIR)/%.cc $(MODS_HDRS) | $(OUT)
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out $(MODS_CXX_DROP),$(MODS_BUILD_CFLAGS)) \
	    $(MODS_CXX_PROFILE) $(MODS_OBJ_CXXFLAGS) -c -o $@ $<

# --- purity manifest ---
# Mod sources compiling to an object with no undefined symbol outside
# MODS_PURE_LIBC: no host state, no JUCE, no syscall. These build and test on
# a dev host. `mods-purity` checks each with nm and fails on anything else.
MODS_PURE_SRCS := $(MODS_DIR)/mods/stem/loop.c \
                  $(MODS_DIR)/mods/stem/ui/state.c \
                  $(MODS_DIR)/mods/wave/codec.c

# mem* are emitted by the compiler for aggregate copies. The three libm calls
# are wave/codec.c's high-band curve, built once at init.
MODS_PURE_LIBC := memcpy memmove memset memcmp exp log sqrt

# Sources the tests cover that are not pure: each reads state defined
# elsewhere, and links only because tests/stubs.c stands in for it. Adding one
# here means adding its externals to stubs.c.
# theme/palette.c is here, not in the pure list, because its cache-stats line
# logs: nm finds fprintf/fflush/stderr/time and g_mod_log, and stubs.c
# provides g_mod_log.
MODS_TEST_DEPS := $(MODS_DIR)/mods/theme/roles.c \
                  $(MODS_DIR)/mods/theme/presets.c \
                  $(MODS_DIR)/mods/theme/palette.c

MODS_TEST_SRCS := $(wildcard $(MODS_DIR)/tests/test_*.c)
MODS_TEST_STUBS := $(MODS_DIR)/tests/stubs.c

# --- ABI ceiling ---
# The deck runs glibc 2.17. Three properties decide whether the shim loads:
# the glibc version ceiling, NEEDED naming libc.so.6 rather than musl's
# libc.so, and nothing in NEEDED the deck lacks. Failing any of them takes
# down every process apl_start.sh starts.
MODS_GLIBC_MAX ?= 2.17
MODS_DECK_LIBS := libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
                  ld-linux-aarch64.so.1

NM      ?= $(CC:gcc=nm)
READELF ?= $(CC:gcc=readelf)

# --- gates ---
# Prefixed so an includer's own targets never collide.
.PHONY: mods-purity mods-abi-check

mods-purity:
	@mkdir -p $(OUT)/purity; fail=0; \
	for src in $(MODS_PURE_SRCS); do \
	  obj=$(OUT)/purity/`echo "$$src" | tr / _`.o; \
	  $(CC) $(MODS_BUILD_CFLAGS) $(MODS_OBJ_CFLAGS) -c -o "$$obj" "$$src" \
	    || { fail=1; continue; }; \
	  bad=`$(NM) --undefined-only "$$obj" | awk '{print $$NF}' \
	       | grep -vxF "$$(printf '%s\n' $(MODS_PURE_LIBC))" | tr '\n' ' '`; \
	  if [ -n "$$bad" ]; then echo "IMPURE $$src: $$bad"; fail=1; \
	  else echo "pure   $$src"; fi; \
	done; \
	[ $$fail -eq 0 ] || { echo "purity: FAILED"; exit 1; }; \
	echo "purity: $(words $(MODS_PURE_SRCS)) sources, nothing outside [$(MODS_PURE_LIBC)]"

# MODS_ABI_TARGET is the linked .so to inspect; the includer sets it.
mods-abi-check:
	@fail=0; \
	echo "== ABI check: $(MODS_ABI_TARGET) against glibc $(MODS_GLIBC_MAX) =="; \
	vers=`$(READELF) --dyn-syms -W $(MODS_ABI_TARGET) \
	      | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -uV`; \
	echo "   glibc versions required: `echo $$vers | tr '\n' ' '`"; \
	for v in $$vers; do \
	  n=$${v#GLIBC_}; \
	  if [ "`printf '%s\n%s\n' "$$n" "$(MODS_GLIBC_MAX)" | sort -V | tail -1`" != "$(MODS_GLIBC_MAX)" ]; then \
	    echo "   FAIL: needs $$v, deck has $(MODS_GLIBC_MAX) - ld.so would refuse the preload"; fail=1; \
	    $(READELF) --dyn-syms -W $(MODS_ABI_TARGET) | grep "$$v" | awk '{print "         " $$8}' | sort -u; \
	  fi; \
	done; \
	needed=`$(READELF) -d $(MODS_ABI_TARGET) | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'`; \
	echo "   NEEDED: `echo $$needed | tr '\n' ' '`"; \
	case "$$needed" in *libc.so.6*) ;; \
	  *) echo "   FAIL: no libc.so.6 in NEEDED (musl build? the deck is glibc)"; fail=1;; esac; \
	for l in $$needed; do \
	  case " $(MODS_DECK_LIBS) " in *" $$l "*) ;; \
	    *) echo "   FAIL: NEEDED $$l is not on the deck"; fail=1;; esac; \
	done; \
	[ $$fail -eq 0 ] || { echo "abi-check: FAILED"; exit 1; }; \
	echo "   ok: loadable on glibc $(MODS_GLIBC_MAX)"
