#!/bin/sh
# Stage the Silksong runtime pack for the HarmonyOS device: <dst>/RomFS + <dst>/out + <dst>/shaders
# (the layout napi_ohos.c start() expects from the imported folder)
# Usage: stage_silksong.sh <dest dir>   — sources are read from the Mac build dirs below.
set -e
DST=${1:?usage: stage_silksong.sh <dest dir>}
SS=/Users/wizout/op/fun/Silksong_build
mkdir -p "$DST/RomFS" "$DST/out" "$DST/shaders"
cp -R "$SS/nx_extract_out/RomFS/"* "$DST/RomFS/" 2>/dev/null || cp -R "$SS/nx_extract_v103/RomFS/"* "$DST/RomFS/" 2>/dev/null || { echo "RomFS source not found under $SS"; exit 1; }
cp "$SS/rewritten-nx/v103/"*.patched.bin "$SS/rewritten-nx/v103/"*.fixups.tsv "$SS/rewritten-nx/v103/"*.gates.tsv "$SS/rewritten-nx/v103/"*.cuts.tsv "$SS/rewritten-nx/v103/"*.obs.tsv "$SS/rewritten-nx/v103/"*.sites.tsv "$DST/out/" 2>/dev/null
cp "$SS/rewritten-nx/v103/"build_src.sha256 "$DST/out/" 2>/dev/null
echo "staged: RomFS $(du -sh "$DST/RomFS" | cut -f1), out $(ls "$DST/out" | wc -l) files, shaders $(ls "$DST/shaders" | wc -l) files (SPIR-V step still pending)"
