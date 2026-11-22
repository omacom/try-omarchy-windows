#!/bin/bash
# iconruns.sh LABEL RUNS [ENV=VALUE...]: Files scroll runs with screenshots
# (navscroll.py), spaced apart, into $OUT/shots-LABEL-N; prints each run's
# fps. Check the captures with iconcheck.py (host, PIL). SESSENV: a script
# that exports the desktop session environment.
. "${SESSENV:-/dev/null}"
label=$1 runs=$2; shift 2
for kv in "$@"; do export "$kv"; done
omarchy-shell -q notifications dismiss "" 2>/dev/null
for n in $(seq "$runs"); do
    d=${OUT:-/tmp}/shots-$label-$n
    rm -rf "$d"; mkdir -p "$d"
    hyprctl eval "hl.dispatch(hl.dsp.focus({ workspace = 7 }))" >/dev/null
    # DEV=1: Files started by DEV_CMD instead (e.g. through a development
    # Mesa wrapper).
    echo "$label-$n: $(NAVSCROLL_SHOTS=$d python3 "$(dirname "$0")/navscroll.py" ${DEV:+${DEV_CMD:-nautilus --new-window /tmp/navtest}} 2>&1 | grep -E '^surface|no ' | cut -c1-90)"
    sleep 8
done
