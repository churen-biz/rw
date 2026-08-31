#!/usr/bin/env bash

set -Eeuo pipefail

vm="${1:-build/milestone-01}"

expect_output() {
  local expected="$1"
  shift
  local actual
  actual="$($vm "$@")"
  [[ "$actual" == "$expected" ]] || {
    printf '期望: %s\n实际: %s\n' "$expected" "$actual" >&2
    return 1
  }
}

expect_failure() {
  if "$vm" "$@" >/dev/null 2>&1; then
    printf '命令本应失败: %s %s\n' "$vm" "$*" >&2
    return 1
  fi
}

expect_output 'sum_to(5) = 15' 5 1000
expect_output 'sum_to(0) = 0' 0 1000
expect_output 'sum_to(-3) = 0' -3 1000
expect_failure 5 5
expect_failure not-a-number 1000
expect_failure 5 -1

printf 'milestone-01: all tests passed\n'
