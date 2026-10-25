# shellcheck shell=bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# devknobs.sh -- sourced, not run. `y264_require_dev_knobs <binary> [text...]`
# exits 2 when the texts (arm specs, env strings) or the environment set a
# Y264_* knob that <binary> ignores because it was built without
# -Ddev_knobs=true; such a run would measure the default under the arm's name.
# Harness variables and the CLI-carried knobs never refuse. scripts/devknobs.py
# holds the knob lists.

y264_require_dev_knobs() {
    python3 "$(dirname "${BASH_SOURCE[0]}")/devknobs.py" --as "$(basename "$0")" "$@" || exit 2
}
