#!/bin/sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
AS="$ROOT/build/svm-as"
OUT="$ROOT/build/factorial.svm"
SVM_RUN="$ROOT/../stack-vm/build/svm-run"
"$AS" "$ROOT/tests/fixtures/factorial.sasm" "$OUT"
got=$("$SVM_RUN" "$OUT" 1)
test "$got" = "i32:120"
echo "ok run-factorial: $got"
