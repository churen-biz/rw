#!/bin/sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
AS="$ROOT/build/svm-as"
OUT="$ROOT/build/answer.svm"
SVM_RUN="$ROOT/../stack-vm/build/svm-run"
"$AS" "$ROOT/tests/fixtures/answer.sasm" "$OUT"
got=$("$SVM_RUN" "$OUT")
test "$got" = "i32:42"
echo "ok run-answer: $got"
