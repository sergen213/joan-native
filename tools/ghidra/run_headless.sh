#!/usr/bin/env bash
# Rebuild/refresh the canonical Ghidra project work/ghidra/joan.gpr and its exports.
#   tools/ghidra/run_headless.sh import    # fresh import + auto-analysis + names + export (all 4 binaries)
#   tools/ghidra/run_headless.sh reexport  # apply tools/ghidra/names*.tsv to joan.exe, re-export everything
# Env: GHIDRA_HOME (default /opt/ghidra), GHIDRA_HEADLESS_MAXMEM (default 12G).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
gh="${GHIDRA_HOME:-/opt/ghidra}/support/analyzeHeadless"
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-12G}"
proj="$root/work/ghidra"
out="$proj/export"
scripts="$root/tools/ghidra"
mkdir -p "$out"
# names.tsv first, then per-area files (names_*.tsv); a later line for the same address wins.
cat "$scripts/names.tsv" "$scripts"/names_*.tsv > "$proj/names_merged.tsv"
cd "$root"

post=(-scriptPath "$scripts"
      -postScript ApplyNames.java "$proj/names_merged.tsv"
      -postScript ExportRtti.java "$out"
      -postScript ExportAll.java "$out")

case "${1:-import}" in
import)
	rm -rf "$proj/joan.gpr" "$proj/joan.rep"
	for bin in joan.exe JoanofArc.exe VSetting.exe v3d_87.dll; do
		"$gh" "$proj" joan -import "$root/$bin" -overwrite "${post[@]}" \
			-log "$proj/import_${bin}.log" -scriptlog "$proj/script_${bin}.log"
	done
	;;
reexport)
	"$gh" "$proj" joan -process -noanalysis "${post[@]}" \
		-log "$proj/reexport.log" -scriptlog "$proj/script_reexport.log"
	;;
*) echo "usage: $0 [import|reexport]" >&2; exit 2 ;;
esac
