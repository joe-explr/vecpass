#!/usr/bin/env bash
# analysis/asm_vector_width.sh <obj>... — per kernel function, count packed
# floating-point arithmetic instructions by register width:
#   p128 = packed on xmm (4 floats), p256 = ymm (8), p512 = zmm (16),
#   scalar = *ss/*sd (1 element).
# "Packed" = mnemonic ends in ps/pd (vmulps, vfmadd213ps, vaddps, ...), not
# moves/shuffles, so it measures how wide the actual math is.
source "$(dirname "$0")/../scripts/env.sh"
for obj in "$@"; do
  echo "== $obj"
  llvm-objdump -d --no-show-raw-insn "$obj" | awk '
    /^[0-9a-f]+ <.*>:$/ { fn=$2; gsub(/[<>:]/,"",fn); next }
    fn !~ /^k_/ { next }
    { m=$2 }
    m ~ /^v?(add|sub|mul|div|fmadd|fmsub|fnmadd|fnmsub)[0-9]*(ps|pd)$/ {
        if ($0 ~ /%zmm/) z[fn]++; else if ($0 ~ /%ymm/) y[fn]++; else x[fn]++; seen[fn]=1 }
    m ~ /^v?(add|sub|mul|div|fmadd|fmsub|fnmadd|fnmsub)[0-9]*(ss|sd)$/ { s[fn]++; seen[fn]=1 }
    END { for (f in seen) printf "%-10s scalar=%-3d p128=%-3d p256=%-3d p512=%-3d\n", f, s[f], x[f], y[f], z[f] }' | sort
done
