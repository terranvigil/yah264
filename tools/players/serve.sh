#!/usr/bin/env sh
# Copyright (c) 2026, the next264 authors
# SPDX-License-Identifier: BSD-2-Clause
# Serve the players over HTTP. Browsers refuse to load <video> from file:// in
# most setups, so a real server is required. This wraps python3's http.server.
#
# Usage: ./serve.sh [DIR] [PORT]
#   DIR   directory to serve (default: the "site" folder make_vmaf.py wrote,
#         falling back to this players directory)
#   PORT  TCP port (default: 8787)
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
dir="${1:-}"
if [ -z "$dir" ]; then
  if [ -d "$here/site" ]; then dir="$here/site"; else dir="$here"; fi
fi
port="${2:-8787}"

# the players are here; the data + mp4s are usually in the site dir. Symlink the
# two html files into the served dir if they aren't already there.
for f in compare.html inspect.html; do
  if [ ! -e "$dir/$f" ] && [ -e "$here/$f" ]; then
    ln -sf "$here/$f" "$dir/$f"
  fi
done

echo "serving $dir at http://localhost:$port"
echo "  compare:  http://localhost:$port/compare.html"
echo "  inspect:  http://localhost:$port/inspect.html"
echo "Ctrl-C to stop"
exec python3 -m http.server "$port" --directory "$dir"
