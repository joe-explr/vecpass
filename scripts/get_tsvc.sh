#!/usr/bin/env bash
# scripts/get_tsvc.sh — fetch TSVC_2 into third_party/ (run on the login node;
# compute nodes may have no internet). Pinned to the commit we analyzed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/TSVC_2"
if [ ! -d "$DEST" ]; then
  mkdir -p "$ROOT/third_party"
  git clone -q https://github.com/UoB-HPC/TSVC_2.git "$DEST"
fi
git -C "$DEST" rev-parse HEAD
