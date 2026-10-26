#!/bin/bash
set -eu
cd "$(dirname "$0")"

target_values=(raddbg raddbg_non_graphical radlink radbin torture)
cc_values=(clang gcc)
if [[ "$(uname -s)" == "Darwin" ]]; then
  # macOS: clang only (build.sh always compiles with clang there; `gcc` is an alias of it)
  cc_values=(clang)
fi
mode_values=(debug release)
run_torture=1
torture_args=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --)
      shift
      torture_args=("$@")
      break
      ;;
    clang)
      cc_values=("$1")
      shift
      ;;
    gcc)
      if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "gcc is not supported on macOS; use clang"
        exit 1
      fi
      cc_values=("$1")
      shift
      ;;
    debug|release)
      mode_values=("$1")
      shift
      ;;
    no_torture)
      run_torture=0
      shift
      ;;
    *)
      echo "usage: $0 [clang|gcc] [debug|release] [no_torture] [-- torture-args]"
      exit 1
      ;;
  esac
done

if [[ "${#torture_args[@]}" == "0" ]]; then
  torture_args=("*")
fi

for m in "${mode_values[@]}"; do
  for c in "${cc_values[@]}"; do
    # nuke artifacts from last run
    rm -rf build/torture_artifacts
    rm -f build/metagen
    for t in "${target_values[@]}"; do
      rm -f "build/$t"
    done

    # build targets
    ./build.sh meta "$c" "$m" "${target_values[@]}"

    # macOS/arm64: the unwinder's prologue / epilogue analysis checks (a standalone program; exits with the failed count)
    if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
      clang -I src -D_DARWIN_C_SOURCE -g -Wno-unused-function -Wno-unused-variable -Wno-writable-strings -Wno-initializer-overrides -o build/unwind_arm64_test src/mac/tests/unwind_arm64_test.c -lm -ldl
      build/unwind_arm64_test
    fi

    # run torture from build folder
    if [[ "$run_torture" == "1" ]]; then
      torture_compiler_args=()
      has_clang_arg=0
      has_gcc_arg=0
      for arg in "${torture_args[@]}"; do
        case "$arg" in
          -clang:*|--clang:*) has_clang_arg=1 ;;
          -gcc:*|--gcc:*) has_gcc_arg=1 ;;
        esac
      done
      if [[ "$c" == clang && "$has_clang_arg" == "0" ]]; then
        torture_compiler_args=("-clang:$(command -v "${CC:-clang}")")
      elif [[ "$c" == gcc && "$has_gcc_arg" == "0" ]]; then
        torture_compiler_args=("-gcc:$(command -v "${CC:-gcc}")")
      fi
      (cd build && ./torture "${torture_compiler_args[@]}" "${torture_args[@]}")
    fi

  done
done
