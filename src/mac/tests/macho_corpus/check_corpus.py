#!/usr/bin/env python3 -I
"""Converter conformance checker for the Mach-O corpus (see build_corpus.sh).

For every row of <build>/manifest.tsv the executable (or dylib) is converted
with `radbin --rdi <exe> --out:<rdi>`, and `radbin --dump <rdi>` is validated
against `dwarfdump --debug-line / --debug-info / --uuid` of the dSYM (the one
built next to the executable, or one this script generates with dsymutil for
debug-map rows) and `otool -l` of the image.

The DIE tree (concrete DW_TAG_subprogram, DW_TAG_lexical_block,
DW_TAG_inlined_subroutine) is matched structurally onto the RDI scope tree
(root scopes of procedures, their children, ...) by range set, so nested sites
with identical ranges and names are told apart.

Checks (one column each in the results table):
  convert        radbin exits 0 and writes the .rdi (cell shows wall time, size)
  guid           top-level guid == the image's LC_UUID (== the dSYM's)
  voff_max       voff_max covers __TEXT,__text and every line / scope range;
                 a binary section for __TEXT,__text with the image's range
  units          one RDI unit per DW_TAG_compile_unit, same names, in order
  line_rows      every is_stmt row (line != 0) of every DWARF line program is
                 covered by an entry with that line and file in the expected
                 table: the unit's, or, inside a DW_TAG_inlined_subroutine,
                 the innermost site's
  inline_lines   at addresses inside an inline site, each enclosing table
                 (the parent site's, finally the unit's) carries the call
                 site's DW_AT_call_line / DW_AT_call_file; where a row runs
                 past the end of the site it began in, the tables still
                 containing the code carry the call line of the site below
                 them; a site without a usable call site still leaves no hole;
                 the first entry of a site's table carries the line of the
                 DWARF row at the site's start (also when it is not a statement)
  line_ranges    tables are sorted, entries are non-empty, lie inside the
                 image's text and inside some DWARF sequence of their unit
  procedures     every concrete DW_TAG_subprogram has a procedure with its
                 name and exactly its ranges (as voffs), and vice versa; no
                 procedure without ranges (abstract instances)
  scopes         every DW_TAG_lexical_block has a scope with its ranges at the
                 same place in the tree, under the right procedure
  inline_sites   every DW_TAG_inlined_subroutine has a scope marked as an
                 inline site with the inlinee's name and the DIE's ranges,
                 nested under the scope of its parent site / block / procedure
  inline_types   every inline site's type is a function type (not the return
                 type)
  src_line_maps  every (line -> address) of every source line map is the
                 start of an entry with that line in a table of that file,
                 and every table entry appears in its file's map
  globals        every file-scope DW_TAG_variable with a DW_OP_addr location
                 is a global variable with the matching voff
  tls            every DW_TAG_variable with a DW_OP_form_tls_address location
                 is a thread variable whose bytecode mentions its address

Run with `python3 -I check_corpus.py [--build-dir D] [--radbin PATH]
[--only GLOB] [--variant dsym|debugmap] [--results RESULTS.md] [--work DIR] [-v]`.
"""
import argparse
import bisect
import collections
import datetime
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
MAX_REASONS = 6
MAX_COLLECT = 60

RDI_DUMP_OMIT = "data_sections,user_defined_types,name_maps,strings,constants,namespaces,local_variables,global_variables_vmap,unit_vmap,scope_vmap"


def run(cmd, timeout=900, cwd=None):
    t0 = time.time()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, cwd=cwd)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"), time.time() - t0


def norm_path(p):
    if not p:
        return ""
    return os.path.normpath(p).lower()


def fmt_ranges(ranges):
    return ", ".join("[%#x, %#x)" % r for r in sorted(ranges)) or "(no ranges)"


################################################################################
# Mach-O image facts (otool / dwarfdump --uuid)

class Image:
    def __init__(self):
        self.base = None          # __TEXT vmaddr
        self.sections = []        # (segname, sectname, addr, size)
        self.text = None          # (addr, size) of __TEXT,__text
        self.uuid = None
        self.is_fat = False


def image_info(exe, arch):
    img = Image()
    rc, out, _, _ = run(["lipo", "-archs", exe])
    img.is_fat = len(out.split()) > 1
    arch_args = ["-arch", arch] if img.is_fat else []
    rc, out, err, _ = run(["otool"] + arch_args + ["-l", exe])
    if rc != 0:
        raise RuntimeError("otool failed: " + err.strip())
    seg = None
    sect = None
    in_section = False
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("cmd LC_SEGMENT_64"):
            seg = {}
            in_section = False
        elif s.startswith("segname ") and seg is not None and not in_section:
            seg["name"] = s.split()[1]
        elif s.startswith("vmaddr ") and seg is not None and not in_section and "vmaddr" not in seg:
            seg["vmaddr"] = int(s.split()[1], 16)
            if seg.get("name") == "__TEXT":
                img.base = seg["vmaddr"]
        elif s == "Section":
            in_section = True
            sect = {}
        elif in_section and s.startswith("sectname "):
            sect["sect"] = s.split()[1]
        elif in_section and s.startswith("segname "):
            sect["seg"] = s.split()[1]
        elif in_section and s.startswith("addr "):
            sect["addr"] = int(s.split()[1], 16)
        elif in_section and s.startswith("size "):
            sect["size"] = int(s.split()[1], 16)
            img.sections.append((sect.get("seg"), sect.get("sect"), sect["addr"], sect["size"]))
            if sect.get("seg") == "__TEXT" and sect.get("sect") == "__text":
                img.text = (sect["addr"], sect["size"])
            in_section = False
    rc, out, _, _ = run(["dwarfdump"] + arch_args + ["--uuid", exe])
    m = re.search(r"UUID: ([0-9A-Fa-f-]+)", out)
    if m:
        img.uuid = m.group(1).upper()
    if img.base is None:
        raise RuntimeError("no __TEXT segment in " + exe)
    return img


################################################################################
# dwarfdump --debug-line

class LineCU:
    def __init__(self, off):
        self.off = off
        self.version = 0
        self.dirs = {}
        self.files = {}      # idx -> [name, dir_idx]
        self.rows = []       # (addr, line, col, file, flags)


RE_LINE_CU = re.compile(r"^debug_line\[0x([0-9a-f]+)\]")
RE_ROW = re.compile(r"^0x([0-9a-f]{16})\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)(?:\s+(\d+))?\s*(.*)$")


def parse_debug_line(text):
    cus = {}
    cu = None
    file_idx = None
    for line in text.splitlines():
        m = RE_LINE_CU.match(line)
        if m:
            cu = LineCU(int(m.group(1), 16))
            cus[cu.off] = cu
            file_idx = None
            continue
        if cu is None:
            continue
        s = line.strip()
        if s.startswith("version:"):
            cu.version = int(s.split()[1])
        elif s.startswith("include_directories["):
            m = re.match(r"include_directories\[\s*(\d+)\] = \"(.*)\"", s)
            if m:
                cu.dirs[int(m.group(1))] = m.group(2)
        elif s.startswith("file_names["):
            m = re.match(r"file_names\[\s*(\d+)\]:", s)
            file_idx = int(m.group(1)) if m else None
        elif file_idx is not None and s.startswith("name:"):
            m = re.match(r"name: \"(.*)\"", s)
            cu.files[file_idx] = [m.group(1) if m else "", 0]
        elif file_idx is not None and s.startswith("dir_index:"):
            cu.files[file_idx][1] = int(s.split()[1])
        else:
            m = RE_ROW.match(line)
            if m:
                flags = set(m.group(8).split())
                cu.rows.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4)), flags))
    return cus


def line_cu_file_path(cu, file_idx, comp_dir):
    f = cu.files.get(file_idx)
    if f is None:
        return None
    name, dir_idx = f
    if name.startswith("/"):
        return norm_path(name)
    d = cu.dirs.get(dir_idx, "")
    if d and not d.startswith("/"):
        d = os.path.join(comp_dir, d)
    if not d:
        d = comp_dir
    return norm_path(os.path.join(d, name))


################################################################################
# dwarfdump --debug-info

class DIE:
    __slots__ = ("off", "tag", "depth", "attrs", "parent", "children", "cu")

    def __init__(self, off, tag, depth):
        self.off = off
        self.tag = tag
        self.depth = depth
        self.attrs = collections.OrderedDict()
        self.parent = None
        self.children = []
        self.cu = None

    def attr(self, name, default=None):
        v = self.attrs.get(name)
        return v if v is not None else default

    def has(self, name):
        return name in self.attrs


RE_DIE = re.compile(r"^0x([0-9a-f]+):( +)(DW_TAG_\w+|NULL)\s*$")
RE_ATTR = re.compile(r"^\s+(DW_AT_\w+)\t(.*)$")


def parse_debug_info(text):
    roots = []
    by_off = {}
    stack = []
    cur = None
    cur_attr = None
    for line in text.splitlines():
        m = RE_DIE.match(line)
        if m:
            off = int(m.group(1), 16)
            depth = (len(m.group(2)) - 1) // 2
            tag = m.group(3)
            cur_attr = None
            if tag == "NULL":
                cur = None
                continue
            die = DIE(off, tag, depth)
            by_off[off] = die
            while stack and stack[-1].depth >= depth:
                stack.pop()
            if stack:
                die.parent = stack[-1]
                stack[-1].children.append(die)
                die.cu = stack[0]
            else:
                roots.append(die)
                die.cu = die
            stack.append(die)
            cur = die
            continue
        if cur is None:
            continue
        m = RE_ATTR.match(line)
        if m:
            cur_attr = m.group(1)
            cur.attrs[cur_attr] = m.group(2).strip()
        elif cur_attr is not None and line.strip() and not line.startswith("0x"):
            cur.attrs[cur_attr] += "\n" + line.strip()
    return roots, by_off


RE_PAREN_STR = re.compile(r'^\("(.*)"\)$', re.S)
RE_PAREN_HEX = re.compile(r"^\((0x[0-9a-f]+)\)$")
RE_PAREN_INT = re.compile(r"^\((\d+)\)$")
RE_REF = re.compile(r'^\((0x[0-9a-f]+)(?: "(.*)")?\)$', re.S)
RE_RANGE = re.compile(r"\[0x([0-9a-f]+), 0x([0-9a-f]+)\)")


def attr_str(die, name):
    v = die.attr(name)
    if v is None:
        return None
    m = RE_PAREN_STR.match(v)
    s = m.group(1) if m else v.strip("()")
    return s.replace('\\"', '"').replace("\\\\", "\\")  # dwarfdump escapes quotes and backslashes


def attr_int(die, name):
    v = die.attr(name)
    if v is None:
        return None
    m = RE_PAREN_HEX.match(v) or RE_PAREN_INT.match(v)
    if m:
        return int(m.group(1), 0)
    return None


def attr_ref(die, name):
    v = die.attr(name)
    if v is None:
        return None, None
    m = RE_REF.match(v)
    if m:
        return int(m.group(1), 16), m.group(2)
    return None, None


def die_name(die, by_off, seen=None):
    """DW_AT_name, else the name of the abstract origin / specification."""
    seen = seen or set()
    if die.off in seen:
        return None
    seen.add(die.off)
    n = attr_str(die, "DW_AT_name")
    if n is not None:
        return n
    for ref in ("DW_AT_abstract_origin", "DW_AT_specification"):
        off, quoted = attr_ref(die, ref)
        if off is not None:
            t = by_off.get(off)
            if t is not None:
                r = die_name(t, by_off, seen)
                if r is not None:
                    return r
            if quoted is not None and t is None:
                return quoted
    return None


def die_linkage_name(die, by_off, seen=None):
    seen = seen or set()
    if die.off in seen:
        return None
    seen.add(die.off)
    for a in ("DW_AT_linkage_name", "DW_AT_MIPS_linkage_name"):
        n = attr_str(die, a)
        if n is not None:
            return n
    for ref in ("DW_AT_abstract_origin", "DW_AT_specification"):
        off, _ = attr_ref(die, ref)
        if off is not None and off in by_off:
            r = die_linkage_name(by_off[off], by_off, seen)
            if r is not None:
                return r
    return None


def die_ranges(die, base):
    """Concrete address ranges as voffs: DW_AT_ranges, else low_pc/high_pc."""
    out = []
    v = die.attr("DW_AT_ranges")
    if v is not None:
        for m in RE_RANGE.finditer(v):
            lo, hi = int(m.group(1), 16), int(m.group(2), 16)
            if hi > lo:
                out.append((lo - base, hi - base))
    lo = attr_int(die, "DW_AT_low_pc")
    hi = attr_int(die, "DW_AT_high_pc")
    if lo is not None and hi is not None and hi > lo:
        out.append((lo - base, hi - base))
    return out


def die_has_ancestor_tag(die, tags):
    p = die.parent
    while p is not None:
        if p.tag in tags:
            return True
        p = p.parent
    return False


_range_cache = {}


def die_ranges_cached(die):
    return _range_cache[id(die)]


def is_abstract_instance(die):
    return die.tag == "DW_TAG_subprogram" and die.has("DW_AT_inline") and not die_ranges_cached(die)


def is_scope_die(die):
    """DIEs the converter turns into scopes: concrete subprograms, blocks, inline sites."""
    if die.tag in ("DW_TAG_lexical_block", "DW_TAG_inlined_subroutine"):
        return True
    if die.tag == "DW_TAG_subprogram":
        return not die.has("DW_AT_declaration") and not is_abstract_instance(die)
    return False


def scope_dies_below(die):
    """Scope DIEs reachable from `die` without crossing another scope DIE (in DIE order)."""
    out = []
    stack = list(reversed(die.children))
    while stack:
        d = stack.pop()
        if is_scope_die(d):
            out.append(d)
        elif d.tag == "DW_TAG_subprogram":
            continue  # declaration / abstract instance: nothing beneath becomes a symbol
        else:
            stack.extend(reversed(d.children))  # namespaces, modules, classes, ...
    return out


def nearest_scope_ancestor(die):
    p = die.parent
    while p is not None and not is_scope_die(p):
        p = p.parent
    return p


def enclosing_subprogram(die):
    p = die.parent
    while p is not None and p.tag != "DW_TAG_subprogram":
        p = p.parent
    return p


################################################################################
# radbin --dump

class Scope:
    __slots__ = ("idx", "proc_idx", "proc_name", "inline_site_idx", "inline_site_name", "ranges", "parent", "children")

    def __init__(self, idx):
        self.idx = idx
        self.proc_idx = 0
        self.proc_name = ""
        self.inline_site_idx = 0
        self.inline_site_name = None
        self.ranges = []
        self.parent = None
        self.children = []

    def label(self):
        return "%s%s" % (self.inline_site_name if self.inline_site_idx else self.proc_name, fmt_ranges(self.ranges))


class RDI:
    def __init__(self):
        self.guid = None
        self.voff_max = None
        self.sections = []           # (name, flags, lo, hi)
        self.source_files = {}       # idx -> (path, source_line_map idx)
        self.units = []              # dicts in index order (unit[0] is nil)
        self.line_tables = {}        # idx -> [(lo, hi, file, line)] without terminators
        self.line_table_raw = {}     # idx -> raw rows incl terminators
        self.source_line_maps = {}   # idx -> {line: [addrs]}
        self.procedures = {}         # idx -> (name, root_scope_idx)
        self.procedure_types = {}    # idx -> type_idx
        self.scopes = {}             # idx -> Scope
        self.inline_sites = {}       # idx -> (name, line_table_idx, type_idx)
        self.type_kinds = {}         # idx -> kind string
        self.globals = collections.defaultdict(list)   # name -> [voff or None]
        self.thread_vars = {}        # name -> bytecode text

    def is_proc_root(self, sc):
        return sc.proc_idx != 0 and self.procedures.get(sc.proc_idx, ("", 0))[1] == sc.idx


def split_dump_sections(text):
    parts = re.split(r"^////////////////////////////////\n//~ (.*)$", text, flags=re.M)
    sections = {}
    for i in range(1, len(parts), 2):
        sections[parts[i].strip()] = parts[i + 1]
    return sections


RE_LT_ROW = re.compile(r"\{ \[0x([0-9a-f]+), 0x([0-9a-f]+)\) file:\s*(\d+), line:\s*(\d+) \}")


def parse_rdi_dump(text):
    r = RDI()
    sec = split_dump_sections(text)

    s = sec.get("TOP LEVEL INFO", "")
    m = re.search(r"voff_max:\s*(0x[0-9a-fA-F]+)", s)
    if m:
        r.voff_max = int(m.group(1), 16)
    m = re.search(r"guid:\s*([0-9A-Fa-f-]+)", s)
    if m:
        r.guid = m.group(1).upper()

    for m in re.finditer(r"\{\s+'([^']*)'\s+`([^`]*)`\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\}", sec.get("BINARY SECTIONS", "")):
        r.sections.append((m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4), 16)))

    for m in re.finditer(r"source_line_map:\s*(\d+),.*?path: '([^']*)'\s*\} // source_file\[(\d+)\]", sec.get("SOURCE FILES", "")):
        r.source_files[int(m.group(3))] = (m.group(2), int(m.group(1)))

    for m in re.finditer(r"// unit\[(\d+)\]\n\s*\{(.*?)\n\s*\}", sec.get("UNITS", ""), re.S):
        body = m.group(2)
        u = {"idx": int(m.group(1))}
        mm = re.search(r"unit_name: '([^']*)'", body)
        u["name"] = mm.group(1) if mm else ""
        mm = re.search(r"line_table_idx: (\d+)", body)
        u["line_table_idx"] = int(mm.group(1)) if mm else 0
        mm = re.search(r"procedures_idx_range: \[(\d+), (\d+)\)", body)
        u["procs"] = (int(mm.group(1)), int(mm.group(2))) if mm else (0, 0)
        r.units.append(u)
    r.units.sort(key=lambda u: u["idx"])

    cur = None
    for line in sec.get("LINE TABLES", "").splitlines():
        m = re.match(r"\s*// line_table\[(\d+)\]", line)
        if m:
            cur = int(m.group(1))
            r.line_tables[cur] = []
            r.line_table_raw[cur] = []
            continue
        m = RE_LT_ROW.search(line)
        if m and cur is not None:
            lo, hi, f, ln = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), int(m.group(4))
            r.line_table_raw[cur].append((lo, hi, f, ln))
            if not (f == 0 and ln == 0):
                r.line_tables[cur].append((lo, hi, f, ln))

    cur = None
    for line in sec.get("SOURCE LINE MAPS", "").splitlines():
        m = re.match(r"\s*// source_line_map\[(\d+)\]", line)
        if m:
            cur = int(m.group(1))
            r.source_line_maps[cur] = {}
            continue
        m = re.match(r"\s*(\d+): \((.*)\)\s*$", line)
        if m and cur is not None:
            r.source_line_maps[cur][int(m.group(1))] = [int(a, 16) for a in re.findall(r"0x[0-9a-f]+", m.group(2))]

    # NOTE: names are printed raw (quotes and all), one symbol per line: match greedily up to the marker
    for m in re.finditer(r"^  '([^\n]*)': // procedures\[(\d+)\]\n\s*\{(.*?)\n  \}", sec.get("PROCEDURES", ""), re.S | re.M):
        mm = re.search(r"root_scope_idx: (\d+)", m.group(3))
        mt = re.search(r"type_idx: (\d+)", m.group(3))
        r.procedures[int(m.group(2))] = (m.group(1), int(mm.group(1)) if mm else 0)
        r.procedure_types[int(m.group(2))] = int(mt.group(1)) if mt else 0

    for m in re.finditer(r"^  '([^\n]*)': // global_variables\[(\d+)\]\n\s*\{(.*?)\n  \}", sec.get("GLOBAL VARIABLES", ""), re.S | re.M):
        voffs = [int(mm.group(1), 16) for mm in re.finditer(r"voff: (0x[0-9a-f]+)", m.group(3))]
        r.globals[m.group(1)].extend(voffs or [None])

    for m in re.finditer(r"^  '([^\n]*)': // thread_variables\[(\d+)\]\n\s*\{(.*?)\n  \}", sec.get("THREAD VARIABLES", ""), re.S | re.M):
        r.thread_vars[m.group(1)] = m.group(3)

    for m in re.finditer(r"\{ type_idx:\s*(\d+),\s*owner_type_idx:\s*(\d+),\s*line_table_idx:\s*(\d+),\s*name: '(.*)'\s*\} // inline_site\[(\d+)\]", sec.get("INLINE SITES", "")):
        r.inline_sites[int(m.group(5))] = (m.group(4), int(m.group(3)), int(m.group(1)))

    for m in re.finditer(r"// type\[(\d+)\]\n\s*\{\n\s*kind: (\w*)", sec.get("TYPE NODES", "")):
        r.type_kinds[int(m.group(1))] = m.group(2)

    stack = []
    for line in sec.get("SCOPES", "").splitlines():
        s = line.strip()
        m = re.match(r"// scope\[(\d+)\]$", s)
        if m:
            sc = Scope(int(m.group(1)))
            r.scopes[sc.idx] = sc
            if stack:
                sc.parent = stack[-1]
                stack[-1].children.append(sc)
            stack.append(sc)
            continue
        m = re.match(r"\} // scope\[/(\d+)\]", s)
        if m:
            if stack and stack[-1].idx == int(m.group(1)):
                stack.pop()
            continue
        if not stack:
            continue
        sc = stack[-1]
        m = re.match(r"proc_idx: (\d+) // (.*)$", s)
        if m:
            sc.proc_idx = int(m.group(1))
            sc.proc_name = m.group(2)
            continue
        m = re.match(r"inline_site_idx: (\d+) \('(.*)'\)$", s)
        if m:
            sc.inline_site_idx = int(m.group(1))
            sc.inline_site_name = m.group(2)
            continue
        if s.startswith("voff_ranges:"):
            sc.ranges = [(int(a, 16), int(b, 16)) for a, b in RE_RANGE.findall(s)]
    return r


################################################################################
# lookups

class Table:
    """Sorted line table with coverage queries."""

    def __init__(self, entries):
        self.entries = sorted(entries)
        self.starts = [e[0] for e in self.entries]

    def covering(self, voff):
        i = bisect.bisect_right(self.starts, voff) - 1
        out = []
        while i >= 0 and self.entries[i][0] <= voff:
            e = self.entries[i]
            if e[0] <= voff < e[1]:
                out.append(e)
            if self.entries[i][1] <= voff and i > 0 and self.entries[i - 1][1] <= self.entries[i][0]:
                break
            i -= 1
        return out


class Sweep:
    """Innermost DIE per address from (lo, hi, die) ranges: deepest in the tag tree, then smallest."""

    def __init__(self, ranges):
        events = []
        for lo, hi, die in ranges:
            events.append((lo, 1, die))
            events.append((hi, 0, die))
        events.sort(key=lambda e: (e[0], e[1]))
        active = {}
        self.points = []
        self.values = []
        i = 0
        while i < len(events):
            addr = events[i][0]
            while i < len(events) and events[i][0] == addr:
                _, kind, die = events[i]
                if kind == 1:
                    active[id(die)] = die
                else:
                    active.pop(id(die), None)
                i += 1
            best = None
            for d in active.values():
                key = (d.depth, -sum(h - l for l, h in die_ranges_cached(d)))
                if best is None or key > best[0]:
                    best = (key, d)
            self.points.append(addr)
            self.values.append(best[1] if best else None)

    def at(self, voff):
        i = bisect.bisect_right(self.points, voff) - 1
        return self.values[i] if i >= 0 else None

    def next_point(self, voff):
        i = bisect.bisect_right(self.points, voff)
        return self.points[i] if i < len(self.points) else None


################################################################################
# the checks

class Result:
    def __init__(self):
        self.cells = collections.OrderedDict()
        self.reasons = collections.OrderedDict()
        self.convert_time = None
        self.rdi_size = None
        self.notes = []

    def set(self, check, reasons, na=False):
        if na and not reasons:
            self.cells[check] = "n/a"
        else:
            self.cells[check] = "fail" if reasons else "pass"
        if reasons:
            self.reasons[check] = reasons

    def fail_all(self, checks, why):
        for c in checks:
            self.cells[c] = "fail"
            self.reasons[c] = [why]


CHECKS = ["convert", "guid", "voff_max", "units", "line_rows", "inline_lines", "line_ranges",
          "procedures", "scopes", "inline_sites", "inline_types", "src_line_maps", "globals", "tls"]


class Reasons(list):
    def add(self, msg):
        if len(self) < MAX_COLLECT:
            self.append(msg)


def names_match(rdi_name, name, linkage):
    """The RDI name against DW_AT_name (possibly qualified on either side) or the linkage name."""
    if name is None:
        return True  # nameless DIE (Swift thunks, closures): any name, including none
    if not rdi_name:
        return False
    if rdi_name == name or rdi_name.endswith("::" + name) or name.endswith("::" + rdi_name) or name.endswith("." + rdi_name):
        return True
    if linkage is not None and rdi_name == linkage:
        return True
    return False


def check_binary(row, args, res):
    exe = row["exe"]
    arch = args.arch
    work = os.path.join(args.work, row["name"].replace("/", "_") + "-" + row["variant"])
    os.makedirs(work, exist_ok=True)
    rdi_path = os.path.join(work, os.path.basename(exe) + ".rdi")

    #- image facts
    img = image_info(exe, arch)
    base = img.base
    if img.text is None:
        res.notes.append("no __TEXT,__text section")
        text_lo, text_hi = 0, 0
    else:
        text_lo, text_hi = img.text[0] - base, img.text[0] + img.text[1] - base

    #- convert
    if os.path.exists(rdi_path):
        os.remove(rdi_path)
    rc, out, err, dt = run([args.radbin, "--rdi", exe, "--out:" + rdi_path, "--verbose"])
    res.convert_time = dt
    with open(os.path.join(work, "radbin_convert.log"), "w") as f:
        f.write(out + err)
    if rc != 0 or not os.path.exists(rdi_path):
        res.fail_all(CHECKS, "radbin --rdi exit %d: %s" % (rc, ((err or out).strip().splitlines() or ["no output"])[-1]))
        return
    res.rdi_size = os.path.getsize(rdi_path)
    res.set("convert", [])

    #- reference dSYM
    dwarf = row["dsym_dwarf"]
    if dwarf == "-" or not os.path.exists(dwarf):
        bundle = os.path.join(work, os.path.basename(exe) + ".dSYM")
        shutil.rmtree(bundle, ignore_errors=True)
        rc, out, err, _ = run(["dsymutil", exe, "-o", bundle])
        dwarf = os.path.join(bundle, "Contents", "Resources", "DWARF", os.path.basename(exe))
        if rc != 0 or not os.path.exists(dwarf):
            res.fail_all(CHECKS[1:], "reference dsymutil failed: " + (err.strip().splitlines() or ["?"])[-1])
            return
        res.notes.append("reference dSYM generated by the checker")
    arch_args = ["--arch", arch] if img.is_fat else []
    rc, dl, err, _ = run(["dwarfdump"] + arch_args + ["--debug-line", dwarf])
    rc2, di, err2, _ = run(["dwarfdump"] + arch_args + ["--debug-info", dwarf])
    rc3, du, _, _ = run(["dwarfdump"] + arch_args + ["--uuid", dwarf])
    if rc != 0 or rc2 != 0:
        res.fail_all(CHECKS[1:], "dwarfdump failed: " + (err + err2).strip())
        return
    dsym_uuid = None
    m = re.search(r"UUID: ([0-9A-Fa-f-]+)", du)
    if m:
        dsym_uuid = m.group(1).upper()
    line_cus = parse_debug_line(dl)
    roots, by_off = parse_debug_info(di)
    for d in by_off.values():
        _range_cache[id(d)] = die_ranges(d, base)

    #- dump
    rc, dump, err, _ = run([args.radbin, "--dump", rdi_path, "--omit:" + RDI_DUMP_OMIT])
    with open(os.path.join(work, "rdi_dump.txt"), "w") as f:
        f.write(dump)
    if rc != 0:
        res.fail_all(CHECKS[1:], "radbin --dump exit %d" % rc)
        return
    rdi = parse_rdi_dump(dump)
    tables = {idx: Table(entries) for idx, entries in rdi.line_tables.items()}
    rdi_files_norm = {idx: norm_path(p) for idx, (p, _) in rdi.source_files.items()}

    def file_matches(rdi_file_idx, dwarf_path):
        return dwarf_path is None or rdi_files_norm.get(rdi_file_idx) == dwarf_path

    def rdi_file_base(idx):
        return os.path.basename(rdi.source_files.get(idx, ("?",))[0])

    def describe(entries):
        return ", ".join("line %d file %s" % (e[3], rdi_file_base(e[2])) for e in entries) or "nothing"

    #- guid
    reasons = Reasons()
    if rdi.guid is None:
        reasons.add("no guid in top_level_info")
    elif rdi.guid != img.uuid:
        reasons.add("guid %s != image LC_UUID %s" % (rdi.guid, img.uuid))
    if dsym_uuid and img.uuid and dsym_uuid != img.uuid:
        reasons.add("dSYM uuid %s != image uuid %s" % (dsym_uuid, img.uuid))
    res.set("guid", reasons)

    #- voff_max and the text section
    reasons = Reasons()
    if rdi.voff_max is None:
        reasons.add("no voff_max")
    else:
        if rdi.voff_max < text_hi:
            reasons.add("voff_max %#x < end of __TEXT,__text %#x" % (rdi.voff_max, text_hi))
        hi_line = max([e[1] for t in rdi.line_tables.values() for e in t] or [0])
        if hi_line > rdi.voff_max:
            reasons.add("line table reaches %#x > voff_max %#x" % (hi_line, rdi.voff_max))
        hi_scope = max([rg[1] for s in rdi.scopes.values() for rg in s.ranges] or [0])
        if hi_scope > rdi.voff_max:
            reasons.add("scope reaches %#x > voff_max %#x" % (hi_scope, rdi.voff_max))
    text_secs = [s for s in rdi.sections if s[0] == "__TEXT,__text"]
    if img.text is not None:
        if not text_secs:
            reasons.add("no __TEXT,__text binary section")
        elif (text_secs[0][2], text_secs[0][3]) != (text_lo, text_hi):
            reasons.add("__TEXT,__text section [%#x, %#x) != image [%#x, %#x)" % (text_secs[0][2], text_secs[0][3], text_lo, text_hi))
    res.set("voff_max", reasons)

    #- units
    cus = [d for d in roots if d.tag == "DW_TAG_compile_unit"]
    reasons = Reasons()
    units = [u for u in rdi.units if u["idx"] != 0]
    if len(units) != len(cus):
        reasons.add("%d RDI units for %d compile units" % (len(units), len(cus)))
    cu_unit = {}
    for i, cu in enumerate(cus):
        if i < len(units):
            u = units[i]
            cu_unit[cu.off] = u
            cu_name = attr_str(cu, "DW_AT_name") or ""
            if norm_path(u["name"]) != norm_path(cu_name) and u["name"] != cu_name:
                reasons.add("unit[%d] name '%s' != CU '%s'" % (u["idx"], u["name"], cu_name))
            if u["line_table_idx"] not in rdi.line_tables:
                reasons.add("unit[%d] line table %d missing" % (u["idx"], u["line_table_idx"]))
    res.set("units", reasons)

    #- structural scope matching: DIE tree -> RDI scope tree
    scope_of = {}         # id(die) -> Scope
    missing = {}          # id(die) -> reason
    matched_scopes = set()

    def kind_ok(d, sc):
        if d.tag == "DW_TAG_inlined_subroutine":
            return sc.inline_site_idx != 0
        if d.tag == "DW_TAG_subprogram":
            return sc.inline_site_idx == 0 and rdi.is_proc_root(sc)
        return sc.inline_site_idx == 0 and not rdi.is_proc_root(sc)

    def match_children(die, pool, parent_label):
        used = set()
        for child in scope_dies_below(die):
            key = frozenset(die_ranges_cached(child))
            cands = [s for s in pool if s.idx not in used and frozenset(s.ranges) == key and kind_ok(child, s)]
            if len(cands) > 1:
                nm, lk = die_name(child, by_off), die_linkage_name(child, by_off)
                named = [s for s in cands if names_match(s.inline_site_name if child.tag == "DW_TAG_inlined_subroutine" else s.proc_name, nm, lk)]
                if named:
                    cands = named
            if not cands:
                there = [s for s in pool if kind_ok(child, s)]
                missing[id(child)] = "%s %s %s: no matching scope under %s (there: %s)" % (
                    child.tag[7:], die_name(child, by_off), fmt_ranges(die_ranges_cached(child)), parent_label,
                    "; ".join(s.label() for s in there[:4]) or "none")
                continue
            sc = cands[0]
            used.add(sc.idx)
            scope_of[id(child)] = sc
            matched_scopes.add(sc.idx)
            match_children(child, sc.children, "%s %s" % (child.tag[7:], die_name(child, by_off)))

    for cu in cus:
        u = cu_unit.get(cu.off)
        if u is None:
            continue
        a, b = u["procs"]
        pool = []
        for pidx in range(a, b):
            root_idx = rdi.procedures.get(pidx, ("", 0))[1]
            sc = rdi.scopes.get(root_idx)
            if sc is not None and sc.parent is None:
                pool.append(sc)
        match_children(cu, pool, "unit[%d]" % u["idx"])

    def site_table(d):
        sc = scope_of.get(id(d))
        if sc is None or not sc.inline_site_idx:
            return None
        site = rdi.inline_sites.get(sc.inline_site_idx)
        return tables.get(site[1]) if site else None

    def site_chain(d):
        chain = [d]
        p = d.parent
        while p is not None:
            if p.tag == "DW_TAG_inlined_subroutine":
                chain.append(p)
            p = p.parent
        return chain

    def is_site_ancestor(anc, d):
        if anc is None:
            return True
        p = d.parent
        while p is not None:
            if p is anc:
                return True
            p = p.parent
        return False

    #- line rows & inline attribution, per CU
    row_reasons = Reasons()
    inl_reasons = Reasons()
    range_reasons = Reasons()
    n_rows = n_inl_rows = 0
    any_inline = False
    for cu in cus:
        u = cu_unit.get(cu.off)
        if u is None:
            continue
        comp_dir = attr_str(cu, "DW_AT_comp_dir") or ""
        stmt_list = attr_int(cu, "DW_AT_stmt_list")
        lcu = line_cus.get(stmt_list) if stmt_list is not None else None
        unit_table = tables.get(u["line_table_idx"])
        if unit_table is None or lcu is None:
            continue
        sites = []
        stack = list(cu.children)
        while stack:
            d = stack.pop()
            stack.extend(d.children)
            if d.tag == "DW_TAG_inlined_subroutine" and die_ranges_cached(d):
                sites.append(d)
        any_inline = any_inline or bool(sites)
        sweep = Sweep([(lo, hi, d) for d in sites for lo, hi in die_ranges_cached(d)])

        def call_site(child):
            call_line = attr_int(child, "DW_AT_call_line")
            call_file = attr_str(child, "DW_AT_call_file")
            if call_file:
                call_file = norm_path(call_file if call_file.startswith("/") else os.path.join(comp_dir, call_file))
            return call_line, call_file

        def expect_call(tbl, voff, child, holder):
            """`tbl` (the table of `holder`, a site DIE or None for the unit) must carry `child`'s call line at voff."""
            if tbl is None:
                return
            call_line, call_file = call_site(child)
            got = tbl.covering(voff)
            if call_line is None or call_line == 0:
                if not got:
                    inl_reasons.add("%#x: %s table has a hole where inlined %s (no DW_AT_call_line) begins" % (
                        voff, ("inlined %s's" % die_name(holder, by_off)) if holder is not None else "unit", die_name(child, by_off)))
                return
            if not [e for e in got if e[3] == call_line and file_matches(e[2], call_file)]:
                inl_reasons.add("%#x: %s table should carry call line %d (%s) of inlined %s; there: %s" % (
                    voff, ("inlined %s's" % die_name(holder, by_off)) if holder is not None else "unit",
                    call_line, os.path.basename(call_file or "?"), die_name(child, by_off), describe(got)))

        # DWARF sequences
        seqs = []
        seq_rows = []
        cur = []
        for r in lcu.rows:
            cur.append(r)
            if "end_sequence" in r[4]:
                seqs.append((cur[0][0] - base, r[0] - base))
                seq_rows.append(cur)
                cur = []
        seqs_sorted = sorted(seqs)
        seq_starts = [s[0] for s in seqs_sorted]
        all_rows = sorted((r[0] - base, r) for sr in seq_rows for r in sr)
        all_row_addrs = [a for a, _ in all_rows]

        for sr in seq_rows:
            merged_start = None   # start of the run of rows the converter merges into one (same file & line)
            prev = None
            for i in range(len(sr) - 1):
                addr, line, col, fidx, flags = sr[i]
                is_stmt = "is_stmt" in flags
                # rows with line 0 are not rows to the converter: the previous row runs on across
                # them; a row with the line and file of the row which began the current run does
                # not begin a new one (unless it is a statement following a non-statement)
                if line != 0 and (prev is None or prev[1] != line or prev[3] != fidx or (is_stmt and "is_stmt" not in prev[4])):
                    merged_start = addr - base
                    prev = sr[i]
                if merged_start is None:
                    merged_start = addr - base
                voff0, voff1 = addr - base, sr[i + 1][0] - base
                if addr < base or voff1 <= voff0 or not (text_lo <= voff0 < text_hi):
                    continue
                fpath = line_cu_file_path(lcu, fidx, comp_dir)
                # the converter's "row" for the run-past rule begins where the merged run begins
                row_inner = sweep.at(merged_start)
                p = voff0
                while p < voff1:
                    nxt = sweep.next_point(p)
                    q = min(voff1, nxt) if nxt is not None else voff1
                    inner = sweep.at(p)
                    first = (p == voff0)
                    if first and is_stmt and line != 0:
                        n_rows += 1
                        tbl = site_table(inner) if inner is not None else unit_table
                        if inner is not None:
                            n_inl_rows += 1
                        if inner is not None and tbl is None:
                            row_reasons.add("row %#x line %d inside inlined %s %s which has no inline-site scope" % (
                                p, line, die_name(inner, by_off), fmt_ranges(die_ranges_cached(inner))))
                        elif not [e for e in tbl.covering(p) if e[3] == line and file_matches(e[2], fpath)]:
                            row_reasons.add("row %#x line %d (%s) not in %s table; there: %s" % (
                                p, line, os.path.basename(fpath or "?"),
                                ("inlined %s's" % die_name(inner, by_off)) if inner is not None else "unit", describe(tbl.covering(p))))
                    # call lines in the enclosing tables
                    if row_inner is not None and inner is not row_inner and is_site_ancestor(inner, row_inner):
                        # the row began in a deeper site which has ended: `inner`'s table (or the unit's)
                        # carries the call line of the chain member directly below it
                        below = row_inner
                        while below.parent is not None and nearest_site(below) is not inner:
                            below = nearest_site(below)
                        expect_call(site_table(inner) if inner is not None else unit_table, p, below, inner)
                    if inner is not None:
                        chain = site_chain(inner)
                        for k, child in enumerate(chain):
                            parent = chain[k + 1] if k + 1 < len(chain) else None
                            expect_call(site_table(parent) if parent is not None else unit_table, p, child, parent)
                    p = q

        # the first entry of a site's table: the line of the DWARF row at the site's start
        # (only where the site is the innermost one there; a deeper site beginning at the
        # same address puts its call line into this table instead)
        for d in sites:
            tbl = site_table(d)
            if tbl is None:
                continue
            for lo, hi in die_ranges_cached(d):
                if sweep.at(lo) is not d:
                    continue
                j = bisect.bisect_right(all_row_addrs, lo) - 1
                if j < 0:
                    continue
                raddr, r = all_rows[j]
                if "end_sequence" in r[4] or r[1] == 0:
                    continue
                k = j + 1
                while k < len(all_rows) and all_rows[k][0] == raddr:  # several rows at one address: the last wins
                    raddr, r = all_rows[k]
                    k += 1
                    if "end_sequence" in r[4]:
                        r = None
                        break
                if r is None or r[1] == 0:
                    continue
                fpath = line_cu_file_path(lcu, r[3], comp_dir)
                got = tbl.covering(lo)
                if not [e for e in got if e[3] == r[1] and file_matches(e[2], fpath)]:
                    inl_reasons.add("site start %#x of inlined %s: table should begin with line %d (%s%s) of the row at %#x; there: %s" % (
                        lo, die_name(d, by_off), r[1], os.path.basename(fpath or "?"), "" if "is_stmt" in r[4] else ", not a statement", raddr, describe(got)))

        # unit table entries inside some DWARF sequence
        for lo, hi, f, ln in rdi.line_tables.get(u["line_table_idx"], []):
            i = bisect.bisect_right(seq_starts, lo) - 1
            if i < 0 or not (seqs_sorted[i][0] <= lo and hi <= seqs_sorted[i][1]):
                range_reasons.add("unit[%d] entry [%#x, %#x) line %d outside every DWARF sequence of its CU" % (u["idx"], lo, hi, ln))
    res.set("line_rows", row_reasons)
    res.set("inline_lines", inl_reasons, na=not any_inline)

    #- line table sanity
    for idx, raw in rdi.line_table_raw.items():
        prev_hi = None
        for lo, hi, f, ln in raw:
            if f == 0 and ln == 0:
                prev_hi = None
                continue
            if hi <= lo:
                range_reasons.add("line_table[%d]: empty or decreasing entry [%#x, %#x) line %d" % (idx, lo, hi, ln))
            if img.text is not None and not (text_lo <= lo and hi <= text_hi):
                range_reasons.add("line_table[%d]: entry [%#x, %#x) outside __TEXT,__text [%#x, %#x)" % (idx, lo, hi, text_lo, text_hi))
            if prev_hi is not None and lo < prev_hi:
                range_reasons.add("line_table[%d]: entry [%#x, %#x) overlaps the previous one (ends %#x)" % (idx, lo, hi, prev_hi))
            prev_hi = hi
    res.set("line_ranges", range_reasons)

    #- procedures
    reasons = Reasons()
    concrete = [d for d in by_off.values() if d.tag == "DW_TAG_subprogram" and is_scope_die(d) and die_ranges_cached(d)]
    for d in concrete:
        sc = scope_of.get(id(d))
        if sc is None:
            reasons.add(missing.get(id(d), "subprogram %s %s has no procedure" % (die_name(d, by_off), fmt_ranges(die_ranges_cached(d)))))
            continue
        name, linkage = die_name(d, by_off), die_linkage_name(d, by_off)
        if not names_match(sc.proc_name, name, linkage):
            reasons.add("subprogram %s %s: procedure there is named %r" % (name, fmt_ranges(die_ranges_cached(d)), sc.proc_name))
    for pidx, (pname, root) in rdi.procedures.items():
        sc = rdi.scopes.get(root)
        if pidx == 0 or sc is None:
            continue
        if not sc.ranges:
            reasons.add("procedure '%s' has no ranges (an abstract instance or declaration?)" % pname)
        elif sc.idx not in matched_scopes:
            reasons.add("procedure '%s' %s matches no DW_TAG_subprogram" % (pname, fmt_ranges(sc.ranges)))
    res.set("procedures", reasons)

    #- lexical blocks
    reasons = Reasons()
    n_blocks = 0
    for d in by_off.values():
        if d.tag != "DW_TAG_lexical_block" or not die_ranges_cached(d):
            continue
        if die_has_ancestor_tag(d, ("DW_TAG_subprogram",)) and is_abstract_instance(enclosing_subprogram(d)):
            continue
        n_blocks += 1
        sc = scope_of.get(id(d))
        p = enclosing_subprogram(d)
        pname = die_name(p, by_off) if p is not None else None
        if sc is None:
            reasons.add(missing.get(id(d), "lexical block %s in %s has no scope" % (fmt_ranges(die_ranges_cached(d)), pname)))
            continue
        if p is not None and not names_match(sc.proc_name, pname, die_linkage_name(p, by_off)):
            reasons.add("lexical block %s in %s: scope belongs to %r" % (fmt_ranges(die_ranges_cached(d)), pname, sc.proc_name))
    res.set("scopes", reasons, na=(n_blocks == 0))

    #- inline sites
    reasons = Reasons()
    type_reasons = Reasons()
    n_sites = 0
    for d in by_off.values():
        if d.tag != "DW_TAG_inlined_subroutine" or not die_ranges_cached(d):
            continue
        n_sites += 1
        name, linkage = die_name(d, by_off), die_linkage_name(d, by_off)
        sc = scope_of.get(id(d))
        if sc is None:
            reasons.add(missing.get(id(d), "inlined %s %s has no inline-site scope" % (name, fmt_ranges(die_ranges_cached(d)))))
            continue
        if not names_match(sc.inline_site_name, name, linkage):
            reasons.add("inlined %s %s: site there is named %r" % (name, fmt_ranges(die_ranges_cached(d)), sc.inline_site_name))
        site = rdi.inline_sites.get(sc.inline_site_idx)
        if site is None:
            reasons.add("inlined %s: inline_site[%d] missing" % (name, sc.inline_site_idx))
            continue
        if site[1] not in rdi.line_tables:
            reasons.add("inlined %s: line table %d missing" % (name, site[1]))
        kind = rdi.type_kinds.get(site[2])
        if site[2] == 0 or kind not in ("Function", "Method"):
            type_reasons.add("inlined %s: type_idx %d is %s, not a function type" % (name, site[2], kind or "nil"))
    res.set("inline_sites", reasons, na=(n_sites == 0))
    typed_procs = sum(1 for pidx, t in rdi.procedure_types.items() if pidx != 0 and t != 0)
    if n_sites and typed_procs == 0 and len(rdi.procedures) > 1:
        # the producer's function types are not converted at all (Swift): not a matter of the inline sites
        res.notes.append("no procedure has a type either: function types of this producer are not converted; inline_types not judged")
        type_reasons = Reasons()
        n_sites_for_types = 0
    else:
        n_sites_for_types = n_sites
    res.set("inline_types", type_reasons, na=(n_sites_for_types == 0))

    #- source line maps
    reasons = Reasons()
    starts_by_file = collections.defaultdict(set)
    for idx, entries in rdi.line_tables.items():
        for lo, hi, f, ln in entries:
            starts_by_file[f].add((lo, ln))
    for fidx, (path, map_idx) in rdi.source_files.items():
        if fidx == 0:
            continue
        m = rdi.source_line_maps.get(map_idx, {})
        starts = starts_by_file.get(fidx, set())
        mapped = set()
        for ln, addrs in m.items():
            for a in addrs:
                mapped.add((a, ln))
                if (a, ln) not in starts:
                    reasons.add("%s:%d -> %#x is not the start of a line-%d entry in any table" % (os.path.basename(path), ln, a, ln))
        for (lo, ln) in starts:
            if (lo, ln) not in mapped:
                reasons.add("%s: table entry %#x line %d missing from the source line map" % (os.path.basename(path), lo, ln))
    res.set("src_line_maps", reasons)

    #- globals and thread variables
    g_reasons = Reasons()
    t_reasons = Reasons()
    n_g = n_t = 0
    for d in by_off.values():
        if d.tag != "DW_TAG_variable":
            continue
        loc = d.attr("DW_AT_location")
        if loc is None:
            continue
        name = die_name(d, by_off)
        linkage = die_linkage_name(d, by_off)
        if name is None:
            continue
        if "DW_OP_form_tls_address" in loc or "DW_OP_GNU_push_tls_address" in loc:
            n_t += 1
            m = re.search(r"DW_OP_(?:const8u|const4u|constu|addr) (0x[0-9a-f]+|\d+)", loc)
            addr = int(m.group(1), 0) if m else None
            hits = [k for k in rdi.thread_vars if names_match(k, name, linkage)]
            if not hits:
                t_reasons.add("thread variable %s (%s) missing" % (name, loc.replace("\n", " ")[:60]))
            elif addr is not None:
                bc = rdi.thread_vars[hits[0]]
                if ("(%d)" % addr) not in bc and ("(%d)" % (addr - base)) not in bc and ("%#x" % addr) not in bc:
                    t_reasons.add("thread variable %s: bytecode does not mention %#x: %s" % (name, addr, bc.strip().replace("\n", " ")[:120]))
            continue
        m = re.match(r"\(DW_OP_addr (0x[0-9a-f]+)\)$", loc.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        if die_has_ancestor_tag(d, ("DW_TAG_subprogram", "DW_TAG_inlined_subroutine", "DW_TAG_lexical_block")):
            continue  # function-scope statics: not required to be globals
        n_g += 1
        hits = [k for k in rdi.globals if names_match(k, name, linkage)]
        if not hits:
            g_reasons.add("global %s @%#x (voff %#x) missing" % (name, addr, addr - base))
        else:
            voffs = [v for k in hits for v in rdi.globals[k]]
            if (addr - base) not in voffs:
                g_reasons.add("global %s: voff %#x not among %s" % (name, addr - base, ", ".join("%#x" % v if v is not None else "none" for v in voffs)))
    res.set("globals", g_reasons, na=(n_g == 0))
    res.set("tls", t_reasons, na=(n_t == 0))
    res.notes.append("%d is_stmt rows (%d in inline sites), %d subprograms, %d inline sites, %d blocks" % (n_rows, n_inl_rows, len(concrete), n_sites, n_blocks))


def nearest_site(die):
    p = die.parent
    while p is not None and p.tag != "DW_TAG_inlined_subroutine":
        p = p.parent
    return p


################################################################################
# driver

def read_manifest(path):
    rows = []
    with open(path) as f:
        header = None
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if header is None:
                header = parts
                continue
            rows.append(dict(zip(header, parts)))
    return rows


def git_describe():
    try:
        rc, out, _, _ = run(["git", "-C", REPO, "describe", "--always", "--dirty"])
        return out.strip() if rc == 0 else "?"
    except Exception:
        return "?"


def human_size(n):
    if n is None:
        return "?"
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return "%.0f%s" % (n, unit) if unit == "B" else "%.1f%s" % (n, unit)
        n /= 1024.0
    return "%.1fTB" % n


FINDINGS_MARKER = "## Findings (maintained by hand)"


def write_results(path, rows, results, args, total_time):
    # rjf: everything from the findings marker on is written by hand and kept across runs
    kept = ""
    if os.path.exists(path):
        with open(path) as f:
            old = f.read()
        i = old.find(FINDINGS_MARKER)
        if i >= 0:
            kept = old[i:]
    lines = []
    lines.append("# Mach-O corpus: converter conformance results")
    lines.append("")
    lines.append("Generated by `src/mac/tests/macho_corpus/check_corpus.py` on %s (radbin `%s`, commit %s)." % (
        datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), os.path.relpath(args.radbin, REPO), git_describe()))
    lines.append("Corpus: `%s`, %d binaries (built by `build_corpus.sh`)." % (args.build_dir, len(rows)))
    lines.append("")
    lines.append("Cells: `pass`, `fail`, `n/a` (nothing of that kind in the binary). The `convert` cell shows the wall time of `radbin --rdi` and the `.rdi` size. Reasons for every `fail` follow the table.")
    lines.append("")
    hdr = ["binary", "variant"] + CHECKS
    lines.append("| " + " | ".join(hdr) + " |")
    lines.append("|" + "|".join("---" for _ in hdr) + "|")
    fails = []
    for row, res in zip(rows, results):
        cells = []
        for c in CHECKS:
            v = res.cells.get(c, "-")
            if c == "convert" and res.convert_time is not None and v == "pass":
                v = "pass (%.2fs, %s)" % (res.convert_time, human_size(res.rdi_size))
            cells.append(v)
            if res.cells.get(c) == "fail":
                fails.append((row, c, res.reasons.get(c, [])))
        lines.append("| %s | %s | %s |" % (row["name"], row["variant"], " | ".join(cells)))
    lines.append("")
    slowest = max(zip(rows, results), key=lambda rr: rr[1].convert_time or 0)
    lines.append("Total checker time: %.0fs. Slowest conversion: %s (%s), %.2fs for a %s `.rdi`." % (
        total_time, slowest[0]["name"], slowest[0]["variant"], slowest[1].convert_time or 0, human_size(slowest[1].rdi_size)))
    lines.append("")
    lines.append("## Corpus matrix")
    lines.append("")
    lines.append("| name | language | configuration | kind | arch | notes |")
    lines.append("|---|---|---|---|---|---|")
    seen = set()
    for row in rows:
        if row["name"] in seen:
            continue
        seen.add(row["name"])
        lines.append("| %s | %s | %s | %s | %s | %s |" % (row["name"], row["lang"], row["cfg"], row["kind"], row["arch"], row.get("notes", "")))
    lines.append("")
    lines.append("Every entry comes in two variants: `dsym` (a `.dSYM` bundle next to the executable) and `debugmap` (no bundle: radbin runs `dsymutil` itself on the executable's `N_OSO` debug map; the checker generates its own reference dSYM).")
    lines.append("")
    lines.append("## Per-binary notes")
    lines.append("")
    for row, res in zip(rows, results):
        if res.notes:
            lines.append("- %s (%s): %s" % (row["name"], row["variant"], "; ".join(res.notes)))
    lines.append("")
    lines.append("## Failures")
    lines.append("")
    if not fails:
        lines.append("None.")
    for row, c, reasons in fails:
        lines.append("### %s (%s): %s" % (row["name"], row["variant"], c))
        lines.append("")
        for r in reasons[:MAX_REASONS]:
            lines.append("- " + r.replace("\n", " "))
        if len(reasons) > MAX_REASONS:
            lines.append("- ... %d more (run the checker with -v)" % (len(reasons) - MAX_REASONS))
        lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
        if kept:
            f.write(kept if kept.endswith("\n") else kept + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.environ.get("MACHO_CORPUS_DIR") or os.path.join(os.environ.get("TMPDIR", "/tmp"), "raddbg_macho_corpus"))
    ap.add_argument("--radbin", default=os.path.join(REPO, "build", "radbin"))
    ap.add_argument("--work", default=None, help="scratch directory for .rdi files, dumps and generated dSYMs (default: <build-dir>/check)")
    ap.add_argument("--only", default="*", help="glob over manifest names (e.g. 'cprog-*')")
    ap.add_argument("--variant", default=None, help="only this variant (dsym / debugmap)")
    ap.add_argument("--arch", default="arm64", help="slice of fat binaries to compare against")
    ap.add_argument("--results", default=None, help="write the results table to this Markdown file")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)
    args.radbin = os.path.abspath(args.radbin)
    args.work = os.path.abspath(args.work or os.path.join(args.build_dir, "check"))
    os.makedirs(args.work, exist_ok=True)

    manifest = os.path.join(args.build_dir, "manifest.tsv")
    if not os.path.exists(manifest):
        print("no manifest at %s; run build_corpus.sh first" % manifest, file=sys.stderr)
        return 2
    rows = [r for r in read_manifest(manifest) if fnmatch.fnmatch(r["name"], args.only) and (args.variant is None or r["variant"] == args.variant)]
    if not rows:
        print("no manifest rows match", file=sys.stderr)
        return 2

    t0 = time.time()
    results = []
    n_fail = 0
    for row in rows:
        res = Result()
        try:
            check_binary(row, args, res)
        except Exception as e:  # keep going; report as a failure of every check
            import traceback
            res.fail_all(CHECKS, "checker error: %s" % e)
            if args.verbose:
                traceback.print_exc()
        results.append(res)
        failed = [c for c in CHECKS if res.cells.get(c) == "fail"]
        n_fail += bool(failed)
        status = "FAIL " + ",".join(failed) if failed else "ok"
        t = (" %.2fs" % res.convert_time) if res.convert_time is not None else ""
        print("%-40s %-9s%s  %s" % (row["name"], row["variant"], t, status))
        if args.verbose:
            for c in failed:
                for r in res.reasons.get(c, [])[:MAX_REASONS]:
                    print("    [%s] %s" % (c, r))
            for n in res.notes:
                print("    note: %s" % n)
    total = time.time() - t0
    if args.results:
        write_results(args.results, rows, results, args, total)
        print("results written to %s" % args.results)
    slowest = max(zip(rows, results), key=lambda rr: rr[1].convert_time or 0)
    print("%d binaries, %d with failures, %.0fs; slowest conversion: %s (%s) %.2fs, %s rdi" % (
        len(rows), n_fail, total, slowest[0]["name"], slowest[0]["variant"], slowest[1].convert_time or 0, human_size(slowest[1].rdi_size)))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
