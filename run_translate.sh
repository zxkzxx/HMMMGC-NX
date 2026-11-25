#!/usr/bin/env bash
set -e
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_WIN="$(cygpath -m "$ROOT_DIR" 2>/dev/null || echo "$ROOT_DIR")"
source "$ROOT_DIR/env_switch.sh"
export PATH="$PATH:/c/Program Files/Git/cmd:/c/Program Files/dotnet"
cd "$ROOT_DIR/src/hmmmgc"

TRANSLATOR="$ROOT_WIN/deps/wiicompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
REPO_WIN="$ROOT_WIN/src/hmmmgc"
OUT_WIN="$REPO_WIN/build/native/translation"

echo "=== 1. BUAT PROJECT TRANSLATOR ==="
ENTRY="$(python3 tools/write_translator_project.py \
    --dol orig/G4AEE9/sys/main.dol \
    --out build/native/translation | tr -d '\r\n')"
echo "Entry point: $ENTRY"
echo "--- isi project.yml ---"
cat build/native/translation/project.yml

echo "=== 2. TERJEMAHKAN main.dol KE C++ ==="
MSYS2_ARG_CONV_EXCL="*" dotnet "$TRANSLATOR" translate-recursive "$ENTRY" \
    --project "$OUT_WIN/project.yml" \
    --outdir "$OUT_WIN/game/functions" \
    --output-metadata "$OUT_WIN/game/generated/base_translation_output.json"

MSYS2_ARG_CONV_EXCL="*" dotnet "$TRANSLATOR" generate-data-init \
    --project "$OUT_WIN/project.yml"

MSYS2_ARG_CONV_EXCL="*" dotnet "$TRANSLATOR" emit-build-shards \
    --project "$OUT_WIN/project.yml" \
    --base-metadata "$OUT_WIN/game/generated/base_translation_output.json" \
    --base-functions-dir "$OUT_WIN/game/functions" \
    --native-source-dir "$REPO_WIN/runtime/src" \
    --out "$OUT_WIN/game/build_shards"

echo "=== 3. CROP GRID, GROUND MESH & MOD HOOKS ==="
python3 tools/size_crop_grid.py --translation build/native/translation/game
python3 tools/size_ground_mesh.py --translation build/native/translation/game
python3 tools/export_mod_symbols.py
python3 tools/insert_mod_hooks.py --translation build/native/translation/game \
    --mod jamie_marriage --mod player_speed --mod carry_speed \
    --mod far_camera --mod marry_anyone --mod longer_days \
    --mod gentle_stamina --mod faster_tools --mod cheats

echo "=== 4. HASIL TERJEMAHAN ==="
ls -lh build/native/translation/game/data_sections_init* build/G4AEE9_symbols.h
find build/native/translation/game/build_shards -name "*.cpp" | wc -l

echo "=== 5. INTIP guest_memory_posix.cpp & fault_handler_posix.cpp ==="
cat runtime/src/guest_memory_posix.cpp
echo "--- fault_handler_posix.cpp ---"
cat runtime/src/fault_handler_posix.cpp
