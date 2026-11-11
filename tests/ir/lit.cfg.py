# tests/ir/lit.cfg.py — lit configuration for the vecpass IR tests.
# Run with: llvm-lit -v tests/ir      (after building; see tests/run_all.sh)
import os
import lit.formats

config.name = "vecpass"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ll", ".c"]
config.excludes = ["lit.cfg.py"]

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(root, "build", "lit")
config.environment["PATH"] = os.environ["PATH"]

plugin = os.path.join(root, "build", "libVecPass.so")
config.substitutions.append(("%plugin", plugin))
config.substitutions.append(("%opt", "opt -load-pass-plugin=" + plugin))
# Unoptimized IR with -O3 frontend semantics (TBAA etc.) for AVX2. Tests
# then run the real `opt -passes='default<O3>'` pipeline, in which vecpass
# sits at the VectorizerStart extension point, just like clang -O3 would.
config.substitutions.append(
    ("%cir",
     "clang -O3 -march=x86-64-v3 -Xclang -disable-llvm-passes "
     "-gline-tables-only -fno-discard-value-names -S -emit-llvm"))
