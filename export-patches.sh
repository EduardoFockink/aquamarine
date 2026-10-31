#!/usr/bin/bash
# Regenerates the numbered patches in this directory from a code branch of the fork, then updates the
# checksums in the PKGBUILD. Usage, from this directory (a worktree of the fork):
#   ./export-patches.sh v0.15.1 mgpu-cpu-copy-0.15.1
set -euo pipefail
base=${1:?upstream tag the branch starts from, e.g. v0.15.1}
branch=${2:?code branch, e.g. mgpu-cpu-copy-0.15.1}

rm -f ./[0-9][0-9][0-9][0-9]-*.patch
git format-patch -q --zero-commit --no-signature -o . "$base..$branch"
ls [0-9][0-9][0-9][0-9]-*.patch

echo "Now list these files in source=() in PKGBUILD and run: updpkgsums"
