#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""alias.py - the yah264 name for the y264 binary.

    alias.py build SRC DST   hard-link (or copy) SRC to DST in the build dir
    alias.py install BINDIR  point BINDIR/yah264 at y264 under the install root
"""
import os
import shutil
import sys


def build(src, dst):
    # a hard link, not a symlink: once y264 is gone (an older checkout in the
    # same build dir) a dangling symlink makes the linker refuse to write
    # through it, while a hard link is just a file it can replace
    if os.path.lexists(dst):
        os.unlink(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def install(bindir):
    if os.environ.get("MESON_INSTALL_DRY_RUN"):
        return
    if os.path.isabs(bindir):
        # an absolute bindir sits outside the prefix, so only DESTDIR moves it
        destdir = os.environ.get("DESTDIR", "")
        if destdir:
            bindir = os.path.join(destdir, os.path.relpath(bindir, os.sep))
    else:
        bindir = os.path.join(os.environ["MESON_INSTALL_DESTDIR_PREFIX"], bindir)
    dst = os.path.join(bindir, "yah264")
    if os.path.isdir(dst) and not os.path.islink(dst):
        sys.exit(f"alias.py: {dst} is a directory; not replacing it with a link")
    # an earlier install left a real yah264 binary here; replace it (or an old
    # link) in one rename so there is never a moment with no yah264
    tmp = dst + ".tmp-alias"
    if os.path.lexists(tmp):
        os.unlink(tmp)
    os.symlink("y264", tmp)
    os.replace(tmp, dst)
    print(f"Installing symlink pointing to y264 to {dst}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "build":
        build(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == "install":
        install(sys.argv[2])
    else:
        sys.exit(__doc__)
