#!/usr/bin/env bash
# Fetch the third-party libraries the firmware builds against.
#
# They are NOT committed: the full reference material is ~1.2 GB of upstream
# clones and vendor PDFs. This script recreates the minimum needed to build, at
# the revisions this workspace was developed and verified against, and applies
# the recorded M5GFX patch that gives the SDL simulator a 466x466 window.
#
# Usage:
#   scripts/fetch-references.sh          # libraries needed to build (~60 MB)
#   scripts/fetch-references.sh --all    # plus optional reference projects
#
# Idempotent: existing clones are checked out to the pinned revision, and the
# patch is only applied when it is not already in the tree.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${root}/references/projects"
patch_file="${root}/patches/M5GFX-sdl-stopwatch-466.patch"
want_all=0
[[ "${1:-}" == "--all" ]] && want_all=1

# name|url|pinned ref  — pinned so the simulator and the device compile the same code.
# ArduinoJson is not here: it is pinned in linkfw/platformio.ini and fetched by PlatformIO.
required=(
  "M5Unified|https://github.com/m5stack/M5Unified.git|0.2.22"
  "M5GFX|https://github.com/m5stack/M5GFX.git|0.2.29"
)
optional=(
  "M5StopWatch-UserDemo|https://github.com/m5stack/M5StopWatch-UserDemo.git|V0.5"
  "m5stack-stopwatch-simulator|https://github.com/ochyai/m5stack-stopwatch-simulator.git|main"
  "M5PM1|https://github.com/m5stack/M5PM1.git|1.0.6"
  "M5IOE1|https://github.com/m5stack/M5IOE1.git|1.0.8"
)

fetch() {
  local name url ref path
  IFS='|' read -r name url ref <<<"$1"
  path="${dest}/${name}"
  if [[ -d "${path}/.git" ]]; then
    # Offline is fine when the pinned revision is already in the clone: warn, do not abort.
    if ! git -C "${path}" fetch --tags --quiet origin 2>/dev/null; then
      echo "==> ${name}: cannot reach ${url} (offline?), using the local clone"
    else
      echo "==> ${name}: already present, checking out ${ref}"
    fi
    if ! git -C "${path}" -c advice.detachedHead=false checkout --quiet "${ref}" 2>/dev/null; then
      echo "    ERROR: ${ref} is not in the local clone and cannot be fetched" >&2
      exit 1
    fi
  else
    echo "==> ${name}: cloning ${url}"
    git clone --quiet "${url}" "${path}"
    git -C "${path}" -c advice.detachedHead=false checkout --quiet "${ref}"
  fi
  printf '    %s at %s\n' "${name}" "$(git -C "${path}" rev-parse --short HEAD)"
}

mkdir -p "${dest}"
for entry in "${required[@]}"; do fetch "${entry}"; done
if (( want_all )); then
  for entry in "${optional[@]}"; do fetch "${entry}"; done
fi

# M5GFX knows board_M5StopWatch but has no 466x466 SDL window size for it, so the
# simulator would fall back to 320x240. See AGENTS.md.
echo "==> M5GFX: SDL 466x466 window patch"
if git -C "${dest}/M5GFX" apply --check --reverse "${patch_file}" 2>/dev/null; then
  echo "    already applied"
elif git -C "${dest}/M5GFX" apply "${patch_file}"; then
  echo "    applied"
else
  echo "    FAILED to apply ${patch_file} — check the M5GFX revision" >&2
  exit 1
fi

cat <<'EOF'

Done. Build the firmware with:
    cd linkfw && export HOMEBREW_PREFIX=/opt/homebrew
    pio run -e native        # desktop simulator (needs SDL2: brew install sdl2)
    pio run -e stopwatch     # device build

Vendor datasheets and schematics are not fetched by this script; the download
URLs are listed in references/README.md.
EOF
