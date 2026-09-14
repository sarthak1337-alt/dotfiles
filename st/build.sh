#!/usr/bin/env bash
# st/build.sh - build st from st-flexipatch with the rice's patches and settings.
#
#   st/build.sh [--install] [--src DIR]
#
# Clones st-flexipatch (pinned below) into ~/.local/src/st-flexipatch, or reuses
# --src, turns on the patches in st/patches.list, applies st/config.sed to
# upstream's config.def.h, enables the libraries those patches need in
# config.mk, and runs make. --install then runs `sudo make install`.
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
repo="https://github.com/bakkeby/st-flexipatch"
commit="aa56259"   # pinned: bump on purpose, after checking patches.list and config.sed still apply
src="${ST_SRC:-$HOME/.local/src/st-flexipatch}"
install=0

while (( $# )); do
  case "$1" in
    --install) install=1 ;;
    --src)     src="${2:?--src needs a directory}"; shift ;;
    *) echo "usage: st/build.sh [--install] [--src DIR]" >&2; exit 2 ;;
  esac
  shift
done

if [[ ! -d $src/.git ]]; then
  mkdir -p "$(dirname "$src")"
  git clone --quiet "$repo" "$src"
fi
if [[ $(git -C "$src" rev-parse --is-shallow-repository) == true ]]; then
  git -C "$src" fetch --quiet --unshallow origin
fi
git -C "$src" checkout --quiet --force "$commit"

cd "$src"

# patches.h: upstream defaults with our list switched on.
enabled=$(sed 's/#.*//' "$here/patches.list" | awk 'NF { print $1 }')
cp patches.def.h patches.h
for p in $enabled; do
  grep -q "^#define ${p}_PATCH " patches.h || { echo "st/build.sh: no patch called ${p}_PATCH" >&2; exit 1; }
  sed -i "s/^#define ${p}_PATCH 0$/#define ${p}_PATCH 1/" patches.h
done

# config.h: upstream defaults plus our values and shortcuts.
sed -f "$here/config.sed" config.def.h > config.h

# config.mk: link the libraries the enabled patches need.
has() { grep -qx "$1" <<<"$enabled"; }
has ALPHA     && sed -i -E 's/^#(XRENDER = )/\1/' config.mk
has LIGATURES && sed -i -E 's/^#(LIGATURES_(C|H|INC|LIBS) = )/\1/' config.mk
has SIXEL     && sed -i -E 's/^#(SIXEL_(C|LIBS) = )/\1/' config.mk

make clean >/dev/null
make -j"$(nproc)"

if (( install )); then
  sudo make install
fi
echo "st built in $src"
