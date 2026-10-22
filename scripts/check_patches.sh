#!/usr/bin/env bash
# Check the toolkit patches against a toolkit revision.
#
#   scripts/check_patches.sh [rev]       rev: a commit or tag of the toolkit clone (default: the pin in build_host.sh)
#
# For each patch, against the unpatched revision:
#   INCLUDED  the revision already has this change (reverse-applies): drop the patch
#   APPLIES   applies after the earlier patches of its domain
#   CONFLICT  neither: the revision changed the same code; rebase the patch, or drop the part upstream now has
# Then the independence of the domains: each applies on its own, and all of them in three different orders give the
# same tree. Every patch header must carry an "Upstream:" line. Exit status 1 when anything is wrong.
# Only needs git and the toolkit clone in build/ps3recomp (git fetch --tags there for a newer revision).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLKIT="$ROOT/build/ps3recomp"
source "$ROOT/scripts/lib_patches.sh"
REV="${1:-$(sed -n 's/^TOOLKIT_REV="\([0-9a-f]*\)".*/\1/p' "$ROOT/scripts/build_host.sh")}"
PATCHES="$ROOT/patches"

git -C "$TOOLKIT" cat-file -e "$REV^{commit}" 2>/dev/null || { echo "$REV is not in $TOOLKIT (git fetch --tags there)" >&2; exit 2; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/r3-check-patches.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
git clone -q -c advice.detachedHead=false --shared "$TOOLKIT" "$WORK/t" 2>/dev/null
cd "$WORK/t"
git config core.autocrlf false
git -c advice.detachedHead=false checkout -q -f "$REV"
REV="$(git rev-parse HEAD)"
bad=0

reset() { git checkout -q -f "$REV"; git clean -fdq; }

echo "toolkit $REV"
echo "== patches (platform $(r3_platform))"
domains=()
while read -r dom plats; do
  domains+=("$dom")
  reset
  while IFS= read -r p; do
    name="${p#"$PATCHES"/}"
    if git apply --check -R "$p" 2>/dev/null && ! git apply --check "$p" 2>/dev/null; then
      echo "  INCLUDED  $name"; bad=1
    elif git apply --check "$p" 2>/dev/null; then
      git apply "$p"; echo "  APPLIES   $name"
    else
      echo "  CONFLICT  $name"; bad=1
    fi
    grep -q '^Upstream:' "$p" || { echo "  NO Upstream: LINE  $name"; bad=1; }
  done < <(r3_domain_patches "$PATCHES" "$dom")
done < <(r3_domains "$PATCHES")

# applies every patch of the given domains, in order, onto the clean revision; prints the tree
tree_of() {
  reset
  local dom p
  for dom in "$@"; do
    while IFS= read -r p; do git apply "$p" 2>/dev/null || { echo "FAIL:$dom"; return 1; }; done < <(r3_domain_patches "$PATCHES" "$dom")
  done
  git add -A
  git write-tree
}

echo "== domains alone"
for dom in "${domains[@]}"; do
  if t="$(tree_of "$dom")"; then echo "  ok   $dom"; else echo "  FAIL $dom (${t#FAIL:} does not apply on its own)"; bad=1; fi
done

echo "== domains together"
fwd="$(tree_of "${domains[@]}")" || true
rev=()
for ((i = ${#domains[@]} - 1; i >= 0; i--)); do rev+=("${domains[i]}"); done
bwd="$(tree_of "${rev[@]}")" || true
mid=("${domains[@]:1}" "${domains[0]}")
rot="$(tree_of "${mid[@]}")" || true
if [[ "$fwd" == FAIL:* || "$bwd" == FAIL:* || "$rot" == FAIL:* ]]; then
  echo "  FAIL: some order does not apply (forward $fwd, reverse $bwd, rotated $rot)"; bad=1
elif [[ "$fwd" == "$bwd" && "$fwd" == "$rot" ]]; then
  echo "  ok   forward, reverse and rotated order give tree ${fwd:0:12}"
else
  echo "  FAIL: the domains are not independent (trees ${fwd:0:12} ${bwd:0:12} ${rot:0:12})"; bad=1
fi

[[ $bad == 0 ]] && echo "all patches fine for $REV" || echo "problems found for $REV"
exit "$bad"
