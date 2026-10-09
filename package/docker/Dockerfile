# syntax=docker/dockerfile:1
#
# Standalone build of the two deck-side binaries this package installs.
# Runs natively on arm64 (Apple Silicon / OrbStack); the CDJ-3000 is aarch64.
#
# Produces, in the `artifacts` stage (a scratch image you extract with
# --output):
#   ep122_shim.so           LD_PRELOAD shim + mods, glibc 2.17-compatible
#   stemd_client_aarch64    STEMS sidecar, musl-static
#
# Two stages, because the two binaries need different libcs:
#
#   agent-builder  (alpine, musl gcc)   -> stemd_client_aarch64
#   shim-builder   (ubuntu 18.04, glibc)-> ep122_shim.so
#
# The shim is LD_PRELOADed into EP122 (the stock aarch64 app, dynamically
# linked against glibc 2.17) and into every process apl_start.sh launches. A
# musl-linked .so lists `libc.so` in NEEDED; glibc's ld.so looks for `libc.so.6`
# and fails with "cannot open shared object file", taking /bin/sh -- and so
# apl_start.sh, and so EP122 -- down with it. Ubuntu 18.04 ships glibc 2.27,
# old enough to stay compatible with the deck's 2.17. Same deck/Makefile drives
# both; only the toolchain differs.

# ---- agent: musl-static aarch64 ------------------------------------------------
FROM arm64v8/alpine:3.19 AS agent-builder
RUN apk add --no-cache build-base make file
COPY deck/ /tmp/deck/
# Alpine arm64's plain `gcc` is the musl compiler we want for a fully
# static aarch64 binary with no glibc dependency.
RUN make -C /tmp/deck CC=gcc agent && \
    file /tmp/deck/out/stemd_client_aarch64

# ---- shim: glibc aarch64 -------------------------------------------------------
FROM ubuntu:18.04 AS shim-builder
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends gcc g++ libc6-dev make file binutils && \
    rm -rf /var/lib/apt/lists/*
COPY deck/ /tmp/deck/
# Stamped in from the host (the build context carries no .git). Declared after
# COPY so a source change re-runs only the shim compile.
ARG MOD_VERSION=unknown
ARG MOD_BUILD=unknown
RUN make -C /tmp/deck CC=gcc MOD_VERSION="$MOD_VERSION" MOD_BUILD="$MOD_BUILD" shim && \
    file /tmp/deck/out/ep122_shim.so
# The ABI gate must be able to fail the build; do not mask it with `|| true`.
# abi-check asserts the glibc ceiling, the libc flavour and that every NEEDED
# library exists on the deck. This is the last place with a real toolchain
# before the object reaches hardware.
RUN make -C /tmp/deck CC=gcc abi-check

# ---- collected artifacts -------------------------------------------------------
FROM scratch AS artifacts
COPY --from=agent-builder /tmp/deck/out/stemd_client_aarch64 /stemd_client_aarch64
COPY --from=shim-builder  /tmp/deck/out/ep122_shim.so        /ep122_shim.so
