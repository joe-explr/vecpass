# scripts/env.sh — put the vecpass toolchain on PATH. Use with: source scripts/env.sh
#
# Why this file exists: the SeaWulf llvm/23.0.0 module ships clang/opt but NOT
# the LLVM C++ headers, so an out-of-tree plugin can't be built against it.
# We install LLVM 23.1.5 (llvmdev + clang + llvm-tools, all the same version)
# with micromamba into $HOME, so clang, opt, and the libraries we link against
# are one consistent build. Mixing versions breaks plugins (ABI + IR format).
#
# Optional: VECPASS_ICX=1 also loads Intel oneAPI 2025.0 (icx) as a reference.

export VECPASS_LLVM_PREFIX="${VECPASS_LLVM_PREFIX:-$HOME/micromamba/envs/llvm}"
if [ ! -x "$VECPASS_LLVM_PREFIX/bin/opt" ]; then
  echo "env.sh: no LLVM at $VECPASS_LLVM_PREFIX (see README: Setup)" >&2
  return 1 2>/dev/null || exit 1
fi
# libexec/llvm holds FileCheck and `not` in the conda package.
export PATH="$VECPASS_LLVM_PREFIX/bin:$VECPASS_LLVM_PREFIX/libexec/llvm:$PATH"
export CC=clang CXX=clang++

# Target ISA for every C compile in tests/bench. Override, e.g.:
#   VECPASS_MARCH=x86-64-v3 (AVX2)   VECPASS_MARCH=sapphirerapids (AVX-512)
export VECPASS_MARCH="${VECPASS_MARCH:-sapphirerapids}"

if [ "${VECPASS_ICX:-0}" = 1 ] && command -v module >/dev/null 2>&1; then
  module load intel/oneAPI/2025.0 >/dev/null 2>&1
  module load tbb/latest umf/latest compiler-rt/latest compiler/latest >/dev/null 2>&1
fi
