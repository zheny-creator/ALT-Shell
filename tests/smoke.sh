#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/work" "$TMP/other" "$TMP/config/alt-shell"
cat > "$TMP/config/alt-shell/config" <<'CONF'
container.enabled=false
container.network=false
container.workspace_writable=true
container.readonly_paths=/usr,/etc,/bin,/sbin,/lib,/lib64
container.extra_readonly=
container.extra_writable=
container.allowed_commands=
container.pass_env=TERM,LANG,LC_ALL,USER,LOGNAME,EDITOR
CONF

cd "$TMP/work"
OUT=$(XDG_CONFIG_HOME="$TMP/config" XDG_DATA_HOME="$TMP/data" ALT_TEST_TMP="$TMP" "$ROOT/alt" <<'INPUT'
export ALT_CAPSULE_TEST=before
cd "$ALT_TEST_TMP/work"
capsule save work ALT_CAPSULE_TEST
export ALT_CAPSULE_TEST=after
cd "$ALT_TEST_TMP/other"
capsule diff work
capsule load work
printf 'loaded=%s\n' "$ALT_CAPSULE_TEST"
printf 'pipeline-ok\n' | grep pipeline-ok
false; echo status=$?
sh -c 'echo captured-error >&2' 2> "$ALT_TEST_TMP/work/stderr.txt"
exit
INPUT
)

printf '%s\n' "$OUT" | grep -q "Saved capsule 'work'"
printf '%s\n' "$OUT" | grep -q 'ALT_CAPSULE_TEST: current=after | saved=before'
printf '%s\n' "$OUT" | grep -q "Loaded capsule 'work'"
printf '%s\n' "$OUT" | grep -q 'loaded=before'
printf '%s\n' "$OUT" | grep -q 'pipeline-ok'
printf '%s\n' "$OUT" | grep -q 'status=1'
grep -q 'captured-error' "$TMP/work/stderr.txt"
printf '%s\n' 'ALT-Shell smoke tests passed.'
