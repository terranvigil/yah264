# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The linux/arm64 image for the SVE tier (docs/simd.md
# tier"). Apple silicon has no SVE, so the tier is executed under QEMU user
# mode, which emulates SVE and SVE2 at any vector length: scripts/sve-docker.sh
# runs the same aarch64 binary at 128 and 256 bits. The container itself is
# native arm64, so the NEON leg runs natively and the compiler is not emulated.
#
# gcc 13 and clang 18 both carry the SVE ACLE (arm_sve.h). ffmpeg is the
# conformance decode oracle, as in the x86 image.
FROM --platform=linux/arm64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        ffmpeg \
        git \
        meson \
        ninja-build \
        pkg-config \
        python3 \
        python3-numpy \
        qemu-user \
        xxd \
    && rm -rf /var/lib/apt/lists/*

ENV HOME=/tmp
ENV XDG_CACHE_HOME=/tmp/.cache

WORKDIR /work
CMD ["/bin/bash"]
