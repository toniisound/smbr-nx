#!/usr/bin/env bash
# build_payload.sh -- builds the Switch payload (game.pck + engine .so files)
# straight from the upstream Super Mario Bros. Remastered repository, so the
# port follows upstream instead of a stale Android fork.
#
#   tools/build_payload.sh [upstream_ref] [out_dir]
#
#   upstream_ref  branch, tag or commit of JHDev2006/Super-Mario-Bros.-Remastered-Public
#                 (default: the latest *-stable release tag; "main" for the
#                 development branch, which can carry unreleased breakage)
#   out_dir       where to put the files (default: ./payload/smbr_nx)
#
# Produces in out_dir:
#   game.pck              upstream game data, exported with the Android preset
#   libgodot_android.so   official Godot arm64-v8a engine (Android export template)
#   libc++_shared.so      matching C++ runtime
#
# Requirements: git, curl, unzip, python3, patch. Runs on Linux x86_64.
# MIT license; see LICENSE.

set -euo pipefail

GODOT_VERSION="${GODOT_VERSION:-4.6.3}"
UPSTREAM_URL="https://github.com/JHDev2006/Super-Mario-Bros.-Remastered-Public.git"
UPSTREAM_REF="${1:-}"
OUT_DIR="$(realpath -m "${2:-payload/smbr_nx}")"

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK_DIR:-$TOOLS_DIR/../.payload-work}"
mkdir -p "$WORK" "$OUT_DIR"
WORK="$(realpath "$WORK")"

GODOT_REL="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}-stable"
GODOT_BIN="$WORK/godot/Godot_v${GODOT_VERSION}-stable_linux.x86_64"

log() { printf '\n== %s\n' "$*"; }

# --- 1. Godot editor (headless) -------------------------------------------
if [ ! -x "$GODOT_BIN" ]; then
  log "Downloading Godot ${GODOT_VERSION} editor"
  mkdir -p "$WORK/godot"
  curl -fsSL -o "$WORK/godot/godot.zip" "$GODOT_REL/Godot_v${GODOT_VERSION}-stable_linux.x86_64.zip"
  unzip -oq "$WORK/godot/godot.zip" -d "$WORK/godot"
  rm "$WORK/godot/godot.zip"
fi
"$GODOT_BIN" --headless --version

# --- 2. engine .so files from the official Android export template ---------
# Only android_release.apk is fetched out of the ~1 GB templates archive
# (HTTP range requests), so this is a ~100 MB download.
APK="$WORK/templates/templates/android_release.apk"
if [ ! -f "$APK" ]; then
  log "Fetching android_release.apk from the ${GODOT_VERSION} export templates"
  python3 "$TOOLS_DIR/rangezip.py" \
    "$GODOT_REL/Godot_v${GODOT_VERSION}-stable_export_templates.tpz" \
    "$WORK/templates" templates/android_release.apk
fi
unzip -ojq "$APK" lib/arm64-v8a/libgodot_android.so lib/arm64-v8a/libc++_shared.so -d "$OUT_DIR"

# --- 3. upstream source + Switch patch --------------------------------------
SRC="$WORK/upstream"
if [ ! -d "$SRC/.git" ]; then
  log "Cloning upstream"
  git clone -q "$UPSTREAM_URL" "$SRC"
fi
git -C "$SRC" fetch -q --tags origin
if [ -z "$UPSTREAM_REF" ]; then
  UPSTREAM_REF="$(git -C "$SRC" tag -l '*-stable' | sort -V | tail -1)"
  [ -n "$UPSTREAM_REF" ] || UPSTREAM_REF=main
fi
log "Checking out upstream ${UPSTREAM_REF}"
git -C "$SRC" checkout -q --force "$UPSTREAM_REF" 2>/dev/null || git -C "$SRC" checkout -q --force "origin/$UPSTREAM_REF"
git -C "$SRC" clean -qfdx -e .godot   # keep the import cache between runs
UPSTREAM_REV="$(git -C "$SRC" rev-parse --short HEAD)"

log "Applying Switch patch"
git -C "$SRC" apply --3way "$TOOLS_DIR/smb1r-switch.patch"

# The wrapper does not load GDExtensions: drop godotgif and discord-rpc-gd from
# the Switch build so the exported pack lists no extensions (the scripts above
# already treat both as optional singletons).
rm -rf "$SRC/godotgif" "$SRC/addons/discord-rpc-gd" "$SRC/.godot/extension_list.cfg"

# project settings + Android pck preset, applied by script (not by patch) so
# upstream edits to these files never conflict
python3 - "$SRC" <<'EOF'
import re, sys, pathlib
src = pathlib.Path(sys.argv[1])

pg = src / "project.godot"
s = pg.read_text()
s = s.replace('"res://addons/discord-rpc-gd/plugin.cfg", ', "")
if "import_etc2_astc" not in s:
    s = s.replace("[rendering]\n", "[rendering]\n\ntextures/vram_compression/import_etc2_astc=true\n", 1)
pg.write_text(s)

ep = src / "export_presets.cfg"
s = ep.read_text()
idx = max(int(i) for i in re.findall(r"^\[preset\.(\d+)\]", s, re.M)) + 1
s += f'''
[preset.{idx}]

name="Switch (Android pck)"
platform="Android"
runnable=false
dedicated_server=false
custom_features="switch"
export_filter="all_resources"
include_filter="*.bgm, *.mp3, *.txt"
exclude_filter=""
export_path="game.pck"
encryption_include_filters=""
encryption_exclude_filters=""
seed=0
encrypt_pck=false
encrypt_directory=false
script_export_mode=2

[preset.{idx}.options]

gradle_build/use_gradle_build=false
architectures/armeabi-v7a=false
architectures/arm64-v8a=true
architectures/x86=false
architectures/x86_64=false
package/unique_name="org.smbr.switch"
package/name="SMB1R"
'''
ep.write_text(s)
EOF

# --- 4. import + export ------------------------------------------------------
# two import passes: the first one reports resources referenced before they
# were imported; the second one settles them
log "Importing project (2 passes)"
"$GODOT_BIN" --headless --import --path "$SRC" > "$WORK/import1.log" 2>&1 || true
"$GODOT_BIN" --headless --import --path "$SRC" > "$WORK/import2.log" 2>&1 || true

log "Exporting game.pck"
"$GODOT_BIN" --headless --path "$SRC" --export-pack "Switch (Android pck)" "$OUT_DIR/game.pck" > "$WORK/export.log" 2>&1
if grep -q 'extension_list.cfg' "$OUT_DIR/game.pck"; then
  echo "!! game.pck still lists GDExtensions; the wrapper cannot load them" >&2
  exit 1
fi

# --- 5. smoke test: boot the pack headless and look for script errors -------
log "Smoke test"
SMOKE_HOME="$WORK/smoke-home"
rm -rf "$SMOKE_HOME"; mkdir -p "$SMOKE_HOME"
HOME="$SMOKE_HOME" timeout 120 "$GODOT_BIN" --headless --main-pack "$OUT_DIR/game.pck" --quit-after 600 \
  > "$WORK/smoke.log" 2>&1 || true
if grep -q 'SCRIPT ERROR' "$WORK/smoke.log"; then
  echo "!! script errors while booting the pack (see $WORK/smoke.log):" >&2
  grep 'SCRIPT ERROR' "$WORK/smoke.log" | sort | uniq -c >&2
  exit 1
fi

printf 'upstream %s (%s)\ngodot %s\n' "$UPSTREAM_REF" "$UPSTREAM_REV" "$GODOT_VERSION" > "$OUT_DIR/payload_version.txt"
log "Done: upstream ${UPSTREAM_REF} (${UPSTREAM_REV}), Godot ${GODOT_VERSION}"
ls -la "$OUT_DIR"
