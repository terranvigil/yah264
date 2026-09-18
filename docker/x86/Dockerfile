# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The linux/amd64 validation image (docs/x86-plan.md, W0d).
#
# The owner has no x86 machine, so every x86 correctness claim this project
# makes before it rents one is made from inside this image, run under an
# emulator on Apple silicon. It therefore holds the WHOLE battery's
# dependencies, not just a compiler: meson and ninja to build, gcc and clang to
# build it twice, ffmpeg as the conformance decode oracle AND as the VMAF
# engine, nasm because the x264 arms of the comparison harness want it, perf
# for the instruction-ratio twin, and git/curl/cmake for the fetch scripts that
# build the extra decoders.
#
# There is no libvmaf-dev here because Ubuntu 24.04 does not package one, in
# any component. That is not a gap: every quality harness in this tree reaches
# VMAF through `ffmpeg -lavfi libvmaf`, and the distribution ffmpeg is built
# with it. A cloud image that wants the library itself builds it from source,
# the way scripts/cloud/bootstrap.sh builds the other tools.
#
# The image carries NO source and NO corpus. scripts/x86-docker.sh bind-mounts
# the worktree read-write and the corpus read-only: the corpus is ~25 GB of Y4M
# and copying it into a layer would be both slow and wrong, since it is not
# ours to redistribute.
FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        cmake \
        curl \
        ffmpeg \
        git \
        linux-tools-common \
        linux-tools-generic \
        meson \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-numpy \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# The scripts run as the invoking uid/gid so that nothing they write into the
# bind-mounted worktree comes back owned by root. That uid has no passwd entry
# and therefore no home, which meson and pip both want, so give them one that
# is writable by anybody.
ENV HOME=/tmp
ENV XDG_CACHE_HOME=/tmp/.cache

WORKDIR /work
CMD ["/bin/bash"]
