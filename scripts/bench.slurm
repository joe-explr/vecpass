#!/usr/bin/env bash
# scripts/bench.slurm — run the full vecpass benchmark on an Intel Xeon
# compute node. Submit from the repo root:   sbatch scripts/bench.slurm
#
# Partition hbm-short-96core = Intel Xeon CPU Max 9468 (Sapphire Rapids,
# AVX-512), 2 sockets x 48 cores, chosen in Phase 0 (see docs/DESIGN.md).
# --exclusive: no other job shares the node (no noisy neighbours).
# The benchmark itself is single-threaded and pinned to one core by
# run_bench.sh (numactl --physcpubind, or taskset if numactl is missing).
#SBATCH --job-name=vecpass-bench
#SBATCH --partition=hbm-short-96core
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --exclusive
#SBATCH --time=01:30:00
#SBATCH --output=results/slurm-%j.out

set -euo pipefail
cd "${SLURM_SUBMIT_DIR:-$(dirname "$0")/..}"

# Make sure the `module` command exists in the batch shell.
if ! command -v module >/dev/null 2>&1; then
  for f in /etc/profile.d/modules.sh /cm/local/apps/environment-modules/current/init/bash; do
    [ -f "$f" ] && { set +u; source "$f"; set -u; break; }
  done
fi
# perf for `perf stat` (optional; run_bench.sh skips it if absent).
set +u; module load perf/5.14.0 2>/dev/null || module load perf 2>/dev/null || true; set -u
# Intel oneAPI icx is loaded by scripts/env.sh when VECPASS_ICX=1 (run_bench does).

echo "node: $(hostname)  job: ${SLURM_JOB_ID:-?}"
export VECPASS_TAG="slurm-${SLURM_JOB_ID:-manual}"
scripts/run_bench.sh
