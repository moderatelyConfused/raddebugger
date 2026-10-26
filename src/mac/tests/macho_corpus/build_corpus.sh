#!/bin/zsh
# Builds the Mach-O test corpus for the converter conformance checker
# (check_corpus.py). Nothing is written into the repository: everything lands
# under the build directory (default: $TMPDIR/raddbg_macho_corpus, or -o).
#
#   build_corpus.sh [-o <build dir>] [--no-zig] [--no-rust] [--no-swift] [--only <name-glob>]
#
# Layout, per corpus entry <name>:
#   <build>/<name>/obj/            object files (the debug map's N_OSO entries point here)
#   <build>/<name>/dsym/<exe>      + <exe>.dSYM next to it (dsymutil run by this script)
#   <build>/<name>/debugmap/<exe>  the same executable without a dSYM (radbin runs dsymutil itself)
#   <build>/symtab-*/stripped/<exe> a `strip -S` copy (no debug map, no DWARF: symbol table only)
# and <build>/manifest.tsv, one row per (entry, variant):
#   name  lang  cfg  kind  variant  exe  dsym_dwarf  arch  notes
# where dsym_dwarf is the DWARF file inside the bundle, or `-` for debug-map rows.
set -eu
setopt null_glob

here="${0:A:h}"
src="$here/src"
build="${MACHO_CORPUS_DIR:-${TMPDIR:-/tmp}/raddbg_macho_corpus}"
build="${build%/}"
with_zig=1
with_rust=1
with_swift=1
only="*"
while (( $# > 0 )); do
  case "$1" in
    -o) build="$2"; shift 2;;
    --no-zig) with_zig=0; shift;;
    --no-rust) with_rust=0; shift;;
    --no-swift) with_swift=0; shift;;
    --only) only="$2"; shift 2;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done

zig="${ZIG:-$HOME/.zvm/bin/zig}"
generator="${GENERATOR_DIR:-$HOME/Desktop/Etc/October-2026/generator}"
rustc="${RUSTC:-$(command -v rustc || true)}"
swiftc="${SWIFTC:-$(command -v swiftc || true)}"
cc="${CC:-clang}"
cxx="${CXX:-clang++}"

mkdir -p "$build"
# compile from the build directory: the sources are then outside
# DW_AT_comp_dir and clang records absolute file names in the line tables
# (as any out-of-tree build does); cprog-O2rel covers relative names
cd "$build"
manifest="$build/manifest.tsv"
: > "$manifest"
notes_file="$build/NOTES.txt"
: > "$notes_file"

tsv_row() { print -r -- "${(pj:\t:)@}" >> "$manifest"; }
note() { print -r -- "$*" | tee -a "$notes_file" >&2; }
want() { [[ "$1" == ${~only} ]]; }

tsv_row name lang cfg kind variant exe dsym_dwarf arch notes

# manifest_rows <name> <lang> <cfg> <kind> <exe basename> <arch> <notes>
# expects <build>/<name>/obj/<exe> to exist; produces the dsym/ and debugmap/ variants
manifest_rows() {
  local name="$1" lang="$2" cfg="$3" kind="$4" exe="$5" arch="$6" notes="$7"
  local dir="$build/$name"
  rm -rf "$dir/dsym" "$dir/debugmap"
  mkdir -p "$dir/dsym" "$dir/debugmap"
  cp "$dir/obj/$exe" "$dir/dsym/$exe"
  cp "$dir/obj/$exe" "$dir/debugmap/$exe"
  if dsymutil "$dir/dsym/$exe" -o "$dir/dsym/$exe.dSYM" 2>"$dir/dsymutil.log"; then
    tsv_row "$name" "$lang" "$cfg" "$kind" dsym "$dir/dsym/$exe" "$dir/dsym/$exe.dSYM/Contents/Resources/DWARF/$exe" "$arch" "$notes"
  else
    note "[$name] dsymutil failed (see $dir/dsymutil.log)"
    tsv_row "$name" "$lang" "$cfg" "$kind" dsym "$dir/dsym/$exe" - "$arch" "dsymutil failed; $notes"
  fi
  tsv_row "$name" "$lang" "$cfg" "$kind" debugmap "$dir/debugmap/$exe" - "$arch" "$notes"
}

# compile_objs <name> <compiler> <flags array name> <sources...>  -> sets $reply to the object paths
compile_objs() {
  local name="$1" compiler="$2" flags_name="$3"; shift 3
  local dir="$build/$name/obj"
  mkdir -p "$dir"
  reply=()
  local s o
  for s in "$@"; do
    o="$dir/${s:t:r}.o"
    "$compiler" "${(P@)flags_name}" -c -o "$o" "$s"
    reply+=("$o")
  done
}

# build_exe <name> <lang> <cfg> <compiler> <cflags array name> <ldflags array name> <sources...>
build_exe() {
  local name="$1" lang="$2" cfg="$3" compiler="$4" cflags_name="$5" ldflags_name="$6"; shift 6
  want "$name" || return 0
  echo "[$name]"
  rm -rf "$build/$name"
  compile_objs "$name" "$compiler" "$cflags_name" "$@"
  local objs=("${reply[@]}")
  local exe="${name%%-*}"
  "$compiler" "${(P@)ldflags_name}" -o "$build/$name/obj/$exe" "${objs[@]}"
  manifest_rows "$name" "$lang" "$cfg" exe "$exe" arm64 ""
}

typeset -a f_O0 f_O2 f_O3nofp f_lto l_none l_lto
f_O0=(-g -O0)
f_O2=(-g -O2)
f_O3nofp=(-g -O3 -fomit-frame-pointer)
f_lto=(-g -O2 -flto)
l_none=()
# with LTO the linker's output object is a temporary, which the debug map
# would point at; -object_path_lto keeps it so that dsymutil can find it
l_lto=(-flto)

#-- C ---------------------------------------------------------------------------
cprog_srcs=("$src/cprog.c" "$src/cprog_util.c")
for cfg in O0 O2 O3nofp lto; do
  name="cprog-$cfg"
  if [[ "$cfg" == lto ]]; then
    l_lto=(-flto "-Wl,-object_path_lto,$build/$name/obj/cprog.lto.o")
    build_exe "$name" c "$cfg" "$cc" f_lto l_lto "${cprog_srcs[@]}"
  else
    build_exe "$name" c "$cfg" "$cc" "f_$cfg" l_none "${cprog_srcs[@]}"
  fi
done

# older DWARF versions: v3 (DW_AT_high_pc is an address, .debug_ranges), v4 (.debug_ranges, offset high_pc)
typeset -a f_O2dw3 f_O2dw4
f_O2dw3=(-g -O2 -gdwarf-3)
f_O2dw4=(-g -O2 -gdwarf-4)
build_exe cprog-O2dw3 c O2dw3 "$cc" f_O2dw3 l_none "${cprog_srcs[@]}"
build_exe cprog-O2dw4 c O2dw4 "$cc" f_O2dw4 l_none "${cprog_srcs[@]}"
# v2: DW_FORM_ref_addr is address-sized (dsymutil's ODR uniquing emits them), .debug_ranges
typeset -a f_O2dw2
f_O2dw2=(-g -O2 -gdwarf-2)
build_exe cprog-O2dw2 c O2dw2 "$cc" f_O2dw2 l_none "${cprog_srcs[@]}"

# inline sites without DW_AT_call_file / DW_AT_call_line: compile to assembly and
# rename those two attributes in the abbreviations (same forms, so the DIE data
# still parses); the converter must still fill the enclosing tables there
if want cprog-O2nocallsite; then
  name=cprog-O2nocallsite
  echo "[$name]"
  rm -rf "$build/$name"; mkdir -p "$build/$name/obj"
  nocallsite_ok=1
  for s in "${cprog_srcs[@]}"; do
    asm="$build/$name/obj/${s:t:r}.s"
    "$cc" -g -O2 -S -o "$asm" "$s"
    if ! grep -q 'DW_AT_call_line' "$asm"; then nocallsite_ok=0; fi
    # DW_AT_call_file (0x58) -> DW_AT_elemental (0x66), DW_AT_call_line (0x59) -> DW_AT_pure (0x67)
    sed -i '' -e 's/^\([[:space:]]*\.byte[[:space:]]*\)88\([[:space:]]*;[[:space:]]*DW_AT_call_file\)/\1102\2/' \
              -e 's/^\([[:space:]]*\.byte[[:space:]]*\)89\([[:space:]]*;[[:space:]]*DW_AT_call_line\)/\1103\2/' "$asm"
    "$cc" -c -o "$build/$name/obj/${s:t:r}.o" "$asm"
  done
  if (( nocallsite_ok )); then
    "$cc" -o "$build/$name/obj/cprog" "$build/$name"/obj/*.o
    manifest_rows "$name" c O2nocallsite exe cprog arm64 "DW_AT_call_file/line removed from inlined_subroutine abbreviations"
  else
    note "[$name] the assembly carries no DW_AT_call_line comments to rewrite; skipped"
  fi
fi

# relative source paths (DWARF file names relative to DW_AT_comp_dir)
if want cprog-O2rel; then
  name=cprog-O2rel
  echo "[$name]"
  rm -rf "$build/$name"; mkdir -p "$build/$name/obj"
  ( cd "$src" && "$cc" -g -O2 -c -o "$build/$name/obj/cprog.o" cprog.c && "$cc" -g -O2 -c -o "$build/$name/obj/cprog_util.o" cprog_util.c )
  "$cc" -o "$build/$name/obj/cprog" "$build/$name/obj/cprog.o" "$build/$name/obj/cprog_util.o"
  manifest_rows "$name" c O2rel exe cprog arm64 "relative source paths"
fi

#-- C++ -------------------------------------------------------------------------
typeset -a fxx_O0 fxx_O2 fxx_lto lxx_lto
fxx_O0=(-std=c++17 -g -O0)
fxx_O2=(-std=c++17 -g -O2)
fxx_lto=(-std=c++17 -g -O2 -flto)
for cfg in O0 O2 lto; do
  name="cxxprog-$cfg"
  if [[ "$cfg" == lto ]]; then
    lxx_lto=(-flto "-Wl,-object_path_lto,$build/$name/obj/cxxprog.lto.o")
    build_exe "$name" c++ "$cfg" "$cxx" fxx_lto lxx_lto "$src/cxxprog.cpp"
  else
    build_exe "$name" c++ "$cfg" "$cxx" "fxx_$cfg" l_none "$src/cxxprog.cpp"
  fi
done

#-- dylib, executables linking / dlopen()ing it ----------------------------------
for cfg in O0 O2; do
  name="dylib-$cfg"
  if want "$name"; then
    echo "[$name]"
    rm -rf "$build/$name"
    compile_objs "$name" "$cc" "f_$cfg" "$src/libshape.c" "$src/dylib_user.c" "$src/dylib_dlopen.c"
    obj="$build/$name/obj"
    "$cc" -dynamiclib -install_name "@rpath/libshape.dylib" -o "$obj/libshape.dylib" "$obj/libshape.o"
    "$cc" -o "$obj/dylib_user" "$obj/dylib_user.o" -L"$obj" -lshape -Wl,-rpath,@executable_path
    "$cc" -o "$obj/dylib_dlopen" "$obj/dylib_dlopen.o"
    # three entries share one obj/ directory: libshape (dylib), dylib_user, dylib_dlopen
    for exe in libshape.dylib dylib_user dylib_dlopen; do
      kind=exe; [[ "$exe" == *.dylib ]] && kind=dylib
      sub="$name/$exe"
      mkdir -p "$build/$sub/obj"
      ln -f "$obj/$exe" "$build/$sub/obj/$exe"
      manifest_rows "$sub" c "$cfg" "$kind" "$exe" arm64 ""
      # the dlopen()ing executable finds the library next to itself at run time
      [[ "$exe" == dylib_dlopen ]] && cp "$obj/libshape.dylib" "$build/$sub/dsym/" && cp "$obj/libshape.dylib" "$build/$sub/debugmap/"
    done
  fi
done

#-- symbol table only ------------------------------------------------------------
# `strip -S` removes the debug map (there is no DWARF in the image anyway): radbin
# converts LC_SYMTAB alone; the checker compares the procedures with `nm -n`
# (manifest variant `stripped`, no reference dSYM)
stripped_row() {
  local name="$1" lang="$2" cfg="$3" kind="$4" src_exe="$5" notes="$6"
  want "$name" || return 0
  [[ -f "$src_exe" ]] || { note "[$name] skipped: $src_exe missing"; return 0; }
  echo "[$name]"
  local exe="${src_exe:t}"
  rm -rf "$build/$name"; mkdir -p "$build/$name/stripped"
  cp "$src_exe" "$build/$name/stripped/$exe"
  strip -S "$build/$name/stripped/$exe"
  tsv_row "$name" "$lang" "$cfg" "$kind" stripped "$build/$name/stripped/$exe" - arm64 "$notes"
}
stripped_row symtab-cprog c O2 exe "$build/cprog-O2/obj/cprog" "strip -S of cprog-O2: symbol table only"
stripped_row symtab-libshape c O2 dylib "$build/dylib-O2/obj/libshape.dylib" "strip -S of dylib-O2's libshape.dylib: symbol table only"

#-- static library ----------------------------------------------------------------
for cfg in O0 O2; do
  name="staticlib-$cfg"
  if want "$name"; then
    echo "[$name]"
    rm -rf "$build/$name"
    compile_objs "$name" "$cc" "f_$cfg" "$src/libstat.c" "$src/static_user.c"
    obj="$build/$name/obj"
    rm -f "$obj/libstat.a"
    libtool -static -o "$obj/libstat.a" "$obj/libstat.o"
    rm -f "$obj/libstat.o"   # the debug map must resolve through libstat.a(libstat.o)
    "$cc" -o "$obj/static_user" "$obj/static_user.o" "$obj/libstat.a"
    manifest_rows "$name" c "$cfg" exe static_user arm64 "N_OSO into archive"
  fi
done

#-- threads / TLS, large frames, recursion, signals --------------------------------
for prog in threads bigframe recursion signals; do
  for cfg in O0 O2; do
    build_exe "$prog-$cfg" c "$cfg" "$cc" "f_$cfg" l_none "$src/$prog.c"
  done
done

#-- universal binary -------------------------------------------------------------
if want universal-O2; then
  name=universal-O2
  echo "[$name]"
  rm -rf "$build/$name"
  obj="$build/$name/obj"
  mkdir -p "$obj/arm64" "$obj/x86_64"
  x86_ok=1
  for s in "${cprog_srcs[@]}"; do
    "$cc" -arch x86_64 -g -O2 -c -o "$obj/x86_64/${s:t:r}.o" "$s" 2>"$build/$name/x86_64.log" || { x86_ok=0; break; }
  done
  if (( x86_ok )) && "$cc" -arch x86_64 -o "$obj/x86_64/cprog" "$obj"/x86_64/*.o 2>>"$build/$name/x86_64.log"; then
    for s in "${cprog_srcs[@]}"; do "$cc" -arch arm64 -g -O2 -c -o "$obj/arm64/${s:t:r}.o" "$s"; done
    "$cc" -arch arm64 -o "$obj/arm64/cprog" "$obj"/arm64/*.o
    lipo -create -output "$obj/cprog" "$obj/arm64/cprog" "$obj/x86_64/cprog"
    manifest_rows "$name" c O2 exe cprog fat "arm64+x86_64 via lipo"
  else
    note "[$name] no x86_64 slice: \`clang -arch x86_64\` failed (see $build/$name/x86_64.log); universal binary skipped"
  fi
fi

#-- Zig generator ----------------------------------------------------------------
if (( with_zig )); then
  if [[ ! -x "$zig" || ! -f "$generator/build.zig" ]]; then
    note "[zig] skipped: zig at $zig or $generator/build.zig missing"
  else
    for mode in Debug ReleaseSafe ReleaseFast; do
      name="zig-generator-$mode"
      want "$name" || continue
      echo "[$name]"
      rm -rf "$build/$name"
      mkdir -p "$build/$name/obj"
      # --cache-dir keeps the local cache (and the .o files the debug map
      # points at) in the build directory; the project itself is not touched
      "$zig" build "-Doptimize=$mode" --prefix "$build/$name/prefix" --build-file "$generator/build.zig" --cache-dir "$build/zig-cache"
      ln -f "$build/$name/prefix/bin/generator" "$build/$name/obj/generator"
      manifest_rows "$name" zig "$mode" exe generator arm64 "debug map into zig cache"
    done
  fi
fi

#-- Rust ----------------------------------------------------------------------------
if (( with_rust )); then
  if [[ -z "$rustc" ]]; then
    note "[rust] skipped: rustc not found"
  else
    for cfg in debug release; do
      name="rustprog-$cfg"
      want "$name" || continue
      echo "[$name]"
      rm -rf "$build/$name"; mkdir -p "$build/$name/obj"
      opt=0; [[ "$cfg" == release ]] && opt=3
      # unpacked: the object files stay next to the executable (debug map), no dSYM is made by rustc
      "$rustc" -g -C "opt-level=$opt" -C split-debuginfo=unpacked -o "$build/$name/obj/rustprog" "$src/rustprog.rs"
      manifest_rows "$name" rust "$cfg" exe rustprog arm64 "split-debuginfo=unpacked"
    done
  fi
fi

#-- Swift -----------------------------------------------------------------------------
if (( with_swift )); then
  if [[ -z "$swiftc" ]]; then
    note "[swift] skipped: swiftc not found"
  else
    for cfg in Onone O; do
      name="swiftprog-$cfg"
      want "$name" || continue
      echo "[$name]"
      rm -rf "$build/$name"; mkdir -p "$build/$name/obj"
      obj="$build/$name/obj"
      "$swiftc" -g "-$cfg" -module-name swiftprog -emit-module-path "$obj/swiftprog.swiftmodule" -c -o "$obj/swiftprog.o" "$src/swiftprog.swift"
      "$swiftc" -o "$obj/swiftprog" "$obj/swiftprog.o"
      manifest_rows "$name" swift "$cfg" exe swiftprog arm64 ""
    done
  fi
fi

echo
echo "corpus: $build"
echo "manifest: $manifest ($(( $(wc -l < "$manifest") - 1 )) rows)"
[[ -s "$notes_file" ]] && { echo "notes:"; cat "$notes_file"; }
exit 0
