#!/usr/bin/env bash
# Build the docs site and open it. Creates the venv on first run.
#
#   scripts/site.sh          build, then open the home page
#   scripts/site.sh --serve  build, then serve on http://localhost:8000
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
venv="$root/local/sitevenv"

if [ ! -x "$venv/bin/python" ]; then
  echo "creating $venv"
  python3 -m venv "$venv"
  "$venv/bin/pip" -q install markdown
fi

"$venv/bin/python" "$root/scripts/build_site.py" "$@"

if [ "${1:-}" != "--serve" ]; then
  open "$root/site/_build/index.html"
fi
