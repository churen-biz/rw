#!/bin/sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MINI="$ROOT/build/mini"
OUT="$ROOT/build/answer-mini.svm"
SVM_RUN="$ROOT/../stack-vm/build/svm-run"
"$MINI" "$ROOT/tests/fixtures/answer.mini" "$OUT"
got=$("$SVM_RUN" "$OUT" 1)
test "$got" = "i32:42"
echo "ok run-mini-answer: $got"
