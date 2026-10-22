# Sourced by build_host.sh and check_patches.sh: which toolkit patches apply on this host, in which order.
#
# patches/upstream/*.patch are open pull requests of the toolkit, imported as they are (dropped once the pinned revision
# contains them); they go on first. Then come the domains of patches/domains.txt for this platform, each one's patches in
# name order. A domain is one subject area and applies to the pinned revision on its own: no domain needs another.

r3_platform() {
  case "$(uname -s)" in Darwin) echo darwin ;; Linux) echo linux ;; *) echo windows ;; esac
}

# r3_domains <patches-dir>: one "domain platforms" line per domain, comments and blank lines dropped
r3_domains() {
  grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$1/domains.txt"
}

# r3_domain_patches <patches-dir> <domain>: its patches, one path per line
r3_domain_patches() {
  local p
  for p in "$1/$2"/*.patch; do [[ -f "$p" ]] && echo "$p"; done
  return 0
}

# r3_patch_list <patches-dir> <platform>: every patch to apply, one path per line
r3_patch_list() {
  local dir="$1" plat="$2" dom plats p
  for p in "$dir"/upstream/*.patch; do [[ -f "$p" ]] && echo "$p"; done
  while read -r dom plats; do
    if [[ -n "$plats" && " ${plats//,/ } " != *" $plat "* ]]; then continue; fi
    r3_domain_patches "$dir" "$dom"
  done < <(r3_domains "$dir")
  return 0
}
