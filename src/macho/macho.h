// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MACHO_H
#define MACHO_H

////////////////////////////////
//~ rjf: Mach-O Format Definitions
//
// NOTE: this file defines the Mach-O on-disk format itself (rather than
// including <mach-o/loader.h>), so that the parser also works on Linux and
// Windows hosts (e.g. radbin converting dSYM bundles). The names mirror the
// Apple definitions with a `MACHO_` prefix. All multi-byte fields are stored in
// the byte order of the target CPU (little-endian for arm64/x86-64), except
// for the fat (universal) binary headers, which are always big-endian.

////////////////////////////////
//~ rjf: Magic Numbers

#define MACHO_MH_MAGIC     0xfeedfaceu // 32-bit mach_header, host-endian
#define MACHO_MH_CIGAM     0xcefaedfeu // 32-bit mach_header, byte-swapped
#define MACHO_MH_MAGIC_64  0xfeedfacfu // 64-bit mach_header_64, host-endian
#define MACHO_MH_CIGAM_64  0xcffaedfeu // 64-bit mach_header_64, byte-swapped
#define MACHO_FAT_MAGIC    0xcafebabeu // fat_header (fields are big-endian)
#define MACHO_FAT_CIGAM    0xbebafecau
#define MACHO_FAT_MAGIC_64 0xcafebabfu // fat_header w/ 64-bit fat_arch_64 entries
#define MACHO_FAT_CIGAM_64 0xbfbafecau

// rjf: maximum number of slices we accept in a fat header; this disambiguates
// fat binaries from Java class files, which share the 0xcafebabe magic.
#define MACHO_FAT_ARCH_MAX_COUNT 64

////////////////////////////////
//~ rjf: CPU Types / Subtypes

#define MACHO_CPU_ARCH_MASK     0xff000000u
#define MACHO_CPU_ARCH_ABI64    0x01000000u
#define MACHO_CPU_ARCH_ABI64_32 0x02000000u

typedef U32 MACHO_CpuType;
enum
{
  MACHO_CPU_TYPE_ANY       = 0xffffffffu,
  MACHO_CPU_TYPE_VAX       = 1,
  MACHO_CPU_TYPE_MC680x0   = 6,
  MACHO_CPU_TYPE_X86       = 7,
  MACHO_CPU_TYPE_I386      = MACHO_CPU_TYPE_X86,
  MACHO_CPU_TYPE_X86_64    = (MACHO_CPU_TYPE_X86 | MACHO_CPU_ARCH_ABI64),
  MACHO_CPU_TYPE_MC98000   = 10,
  MACHO_CPU_TYPE_HPPA      = 11,
  MACHO_CPU_TYPE_ARM       = 12,
  MACHO_CPU_TYPE_ARM64     = (MACHO_CPU_TYPE_ARM | MACHO_CPU_ARCH_ABI64),
  MACHO_CPU_TYPE_ARM64_32  = (MACHO_CPU_TYPE_ARM | MACHO_CPU_ARCH_ABI64_32),
  MACHO_CPU_TYPE_MC88000   = 13,
  MACHO_CPU_TYPE_SPARC     = 14,
  MACHO_CPU_TYPE_I860      = 15,
  MACHO_CPU_TYPE_POWERPC   = 18,
  MACHO_CPU_TYPE_POWERPC64 = (MACHO_CPU_TYPE_POWERPC | MACHO_CPU_ARCH_ABI64),
};

#define MACHO_CPU_SUBTYPE_MASK  0xff000000u
#define MACHO_CPU_SUBTYPE_LIB64 0x80000000u
#define MACHO_CPU_SUBTYPE_PTRAUTH_ABI 0x80000000u

typedef U32 MACHO_CpuSubtype;
enum
{
  MACHO_CPU_SUBTYPE_ARM64_ALL = 0,
  MACHO_CPU_SUBTYPE_ARM64_V8  = 1,
  MACHO_CPU_SUBTYPE_ARM64E    = 2,
  MACHO_CPU_SUBTYPE_X86_64_ALL = 3,
  MACHO_CPU_SUBTYPE_X86_64_H   = 8,
  MACHO_CPU_SUBTYPE_I386_ALL   = 3,
  MACHO_CPU_SUBTYPE_ARM_ALL    = 0,
  MACHO_CPU_SUBTYPE_ARM_V7     = 9,
  MACHO_CPU_SUBTYPE_ARM_V7S    = 11,
  MACHO_CPU_SUBTYPE_ARM_V7K    = 12,
};

////////////////////////////////
//~ rjf: File Types

typedef U32 MACHO_FileType;
enum
{
  MACHO_MH_OBJECT      = 0x1, // relocatable object file
  MACHO_MH_EXECUTE     = 0x2, // demand paged executable file
  MACHO_MH_FVMLIB      = 0x3, // fixed VM shared library file
  MACHO_MH_CORE        = 0x4, // core file
  MACHO_MH_PRELOAD     = 0x5, // preloaded executable file
  MACHO_MH_DYLIB       = 0x6, // dynamically bound shared library
  MACHO_MH_DYLINKER    = 0x7, // dynamic link editor
  MACHO_MH_BUNDLE      = 0x8, // dynamically bound bundle file
  MACHO_MH_DYLIB_STUB  = 0x9, // shared library stub for static linking only
  MACHO_MH_DSYM        = 0xa, // companion file with only debug sections
  MACHO_MH_KEXT_BUNDLE = 0xb, // x86_64 kexts
  MACHO_MH_FILESET     = 0xc, // a file composed of other Mach-Os
};

////////////////////////////////
//~ rjf: Header Flags

typedef U32 MACHO_HeaderFlags;
enum
{
  MACHO_MH_NOUNDEFS                      = 0x1,
  MACHO_MH_INCRLINK                      = 0x2,
  MACHO_MH_DYLDLINK                      = 0x4,
  MACHO_MH_BINDATLOAD                    = 0x8,
  MACHO_MH_PREBOUND                      = 0x10,
  MACHO_MH_SPLIT_SEGS                    = 0x20,
  MACHO_MH_LAZY_INIT                     = 0x40,
  MACHO_MH_TWOLEVEL                      = 0x80,
  MACHO_MH_FORCE_FLAT                    = 0x100,
  MACHO_MH_NOMULTIDEFS                   = 0x200,
  MACHO_MH_NOFIXPREBINDING               = 0x400,
  MACHO_MH_PREBINDABLE                   = 0x800,
  MACHO_MH_ALLMODSBOUND                  = 0x1000,
  MACHO_MH_SUBSECTIONS_VIA_SYMBOLS       = 0x2000,
  MACHO_MH_CANONICAL                     = 0x4000,
  MACHO_MH_WEAK_DEFINES                  = 0x8000,
  MACHO_MH_BINDS_TO_WEAK                 = 0x10000,
  MACHO_MH_ALLOW_STACK_EXECUTION         = 0x20000,
  MACHO_MH_ROOT_SAFE                     = 0x40000,
  MACHO_MH_SETUID_SAFE                   = 0x80000,
  MACHO_MH_NO_REEXPORTED_DYLIBS          = 0x100000,
  MACHO_MH_PIE                           = 0x200000,
  MACHO_MH_DEAD_STRIPPABLE_DYLIB         = 0x400000,
  MACHO_MH_HAS_TLV_DESCRIPTORS           = 0x800000,
  MACHO_MH_NO_HEAP_EXECUTION             = 0x1000000,
  MACHO_MH_APP_EXTENSION_SAFE            = 0x02000000,
  MACHO_MH_NLIST_OUTOFSYNC_WITH_DYLDINFO = 0x04000000,
  MACHO_MH_SIM_SUPPORT                   = 0x08000000,
  MACHO_MH_DYLIB_IN_CACHE                = 0x80000000,
};

////////////////////////////////
//~ rjf: Load Command Types

#define MACHO_LC_REQ_DYLD 0x80000000u

typedef U32 MACHO_LoadCommandType;
enum
{
  MACHO_LC_SEGMENT                  = 0x1,
  MACHO_LC_SYMTAB                   = 0x2,
  MACHO_LC_SYMSEG                   = 0x3,
  MACHO_LC_THREAD                   = 0x4,
  MACHO_LC_UNIXTHREAD               = 0x5,
  MACHO_LC_LOADFVMLIB               = 0x6,
  MACHO_LC_IDFVMLIB                 = 0x7,
  MACHO_LC_IDENT                    = 0x8,
  MACHO_LC_FVMFILE                  = 0x9,
  MACHO_LC_PREPAGE                  = 0xa,
  MACHO_LC_DYSYMTAB                 = 0xb,
  MACHO_LC_LOAD_DYLIB               = 0xc,
  MACHO_LC_ID_DYLIB                 = 0xd,
  MACHO_LC_LOAD_DYLINKER            = 0xe,
  MACHO_LC_ID_DYLINKER              = 0xf,
  MACHO_LC_PREBOUND_DYLIB           = 0x10,
  MACHO_LC_ROUTINES                 = 0x11,
  MACHO_LC_SUB_FRAMEWORK            = 0x12,
  MACHO_LC_SUB_UMBRELLA             = 0x13,
  MACHO_LC_SUB_CLIENT               = 0x14,
  MACHO_LC_SUB_LIBRARY              = 0x15,
  MACHO_LC_TWOLEVEL_HINTS           = 0x16,
  MACHO_LC_PREBIND_CKSUM            = 0x17,
  MACHO_LC_LOAD_WEAK_DYLIB          = (0x18 | MACHO_LC_REQ_DYLD),
  MACHO_LC_SEGMENT_64               = 0x19,
  MACHO_LC_ROUTINES_64              = 0x1a,
  MACHO_LC_UUID                     = 0x1b,
  MACHO_LC_RPATH                    = (0x1c | MACHO_LC_REQ_DYLD),
  MACHO_LC_CODE_SIGNATURE           = 0x1d,
  MACHO_LC_SEGMENT_SPLIT_INFO       = 0x1e,
  MACHO_LC_REEXPORT_DYLIB           = (0x1f | MACHO_LC_REQ_DYLD),
  MACHO_LC_LAZY_LOAD_DYLIB          = 0x20,
  MACHO_LC_ENCRYPTION_INFO          = 0x21,
  MACHO_LC_DYLD_INFO                = 0x22,
  MACHO_LC_DYLD_INFO_ONLY           = (0x22 | MACHO_LC_REQ_DYLD),
  MACHO_LC_LOAD_UPWARD_DYLIB        = (0x23 | MACHO_LC_REQ_DYLD),
  MACHO_LC_VERSION_MIN_MACOSX       = 0x24,
  MACHO_LC_VERSION_MIN_IPHONEOS     = 0x25,
  MACHO_LC_FUNCTION_STARTS          = 0x26,
  MACHO_LC_DYLD_ENVIRONMENT         = 0x27,
  MACHO_LC_MAIN                     = (0x28 | MACHO_LC_REQ_DYLD),
  MACHO_LC_DATA_IN_CODE             = 0x29,
  MACHO_LC_SOURCE_VERSION           = 0x2a,
  MACHO_LC_DYLIB_CODE_SIGN_DRS      = 0x2b,
  MACHO_LC_ENCRYPTION_INFO_64       = 0x2c,
  MACHO_LC_LINKER_OPTION            = 0x2d,
  MACHO_LC_LINKER_OPTIMIZATION_HINT = 0x2e,
  MACHO_LC_VERSION_MIN_TVOS         = 0x2f,
  MACHO_LC_VERSION_MIN_WATCHOS      = 0x30,
  MACHO_LC_NOTE                     = 0x31,
  MACHO_LC_BUILD_VERSION            = 0x32,
  MACHO_LC_DYLD_EXPORTS_TRIE        = (0x33 | MACHO_LC_REQ_DYLD),
  MACHO_LC_DYLD_CHAINED_FIXUPS      = (0x34 | MACHO_LC_REQ_DYLD),
  MACHO_LC_FILESET_ENTRY            = (0x35 | MACHO_LC_REQ_DYLD),
  MACHO_LC_ATOM_INFO                = 0x36,
};

////////////////////////////////
//~ rjf: VM Protection Flags (segment maxprot/initprot)

typedef U32 MACHO_VmProt;
enum
{
  MACHO_VM_PROT_NONE    = 0x0,
  MACHO_VM_PROT_READ    = 0x1,
  MACHO_VM_PROT_WRITE   = 0x2,
  MACHO_VM_PROT_EXECUTE = 0x4,
};

////////////////////////////////
//~ rjf: Segment Flags

typedef U32 MACHO_SegmentFlags;
enum
{
  MACHO_SG_HIGHVM              = 0x1,
  MACHO_SG_FVMLIB              = 0x2,
  MACHO_SG_NORELOC             = 0x4,
  MACHO_SG_PROTECTED_VERSION_1 = 0x8,
  MACHO_SG_READ_ONLY           = 0x10,
};

////////////////////////////////
//~ rjf: Section Types & Attributes (section.flags)

#define MACHO_SECTION_TYPE       0x000000ffu
#define MACHO_SECTION_ATTRIBUTES 0xffffff00u
#define MACHO_SECTION_ATTRIBUTES_USR 0xff000000u
#define MACHO_SECTION_ATTRIBUTES_SYS 0x00ffff00u

typedef U32 MACHO_SectionType;
enum
{
  MACHO_S_REGULAR                             = 0x0,
  MACHO_S_ZEROFILL                            = 0x1,
  MACHO_S_CSTRING_LITERALS                    = 0x2,
  MACHO_S_4BYTE_LITERALS                      = 0x3,
  MACHO_S_8BYTE_LITERALS                      = 0x4,
  MACHO_S_LITERAL_POINTERS                    = 0x5,
  MACHO_S_NON_LAZY_SYMBOL_POINTERS            = 0x6,
  MACHO_S_LAZY_SYMBOL_POINTERS                = 0x7,
  MACHO_S_SYMBOL_STUBS                        = 0x8,
  MACHO_S_MOD_INIT_FUNC_POINTERS              = 0x9,
  MACHO_S_MOD_TERM_FUNC_POINTERS              = 0xa,
  MACHO_S_COALESCED                           = 0xb,
  MACHO_S_GB_ZEROFILL                         = 0xc,
  MACHO_S_INTERPOSING                         = 0xd,
  MACHO_S_16BYTE_LITERALS                     = 0xe,
  MACHO_S_DTRACE_DOF                          = 0xf,
  MACHO_S_LAZY_DYLIB_SYMBOL_POINTERS          = 0x10,
  MACHO_S_THREAD_LOCAL_REGULAR                = 0x11,
  MACHO_S_THREAD_LOCAL_ZEROFILL               = 0x12,
  MACHO_S_THREAD_LOCAL_VARIABLES              = 0x13,
  MACHO_S_THREAD_LOCAL_VARIABLE_POINTERS      = 0x14,
  MACHO_S_THREAD_LOCAL_INIT_FUNCTION_POINTERS = 0x15,
  MACHO_S_INIT_FUNC_OFFSETS                   = 0x16,
};

typedef U32 MACHO_SectionAttributes;
enum
{
  MACHO_S_ATTR_PURE_INSTRUCTIONS   = 0x80000000u,
  MACHO_S_ATTR_NO_TOC              = 0x40000000u,
  MACHO_S_ATTR_STRIP_STATIC_SYMS   = 0x20000000u,
  MACHO_S_ATTR_NO_DEAD_STRIP       = 0x10000000u,
  MACHO_S_ATTR_LIVE_SUPPORT        = 0x08000000u,
  MACHO_S_ATTR_SELF_MODIFYING_CODE = 0x04000000u,
  MACHO_S_ATTR_DEBUG               = 0x02000000u,
  MACHO_S_ATTR_SOME_INSTRUCTIONS   = 0x00000400u,
  MACHO_S_ATTR_EXT_RELOC           = 0x00000200u,
  MACHO_S_ATTR_LOC_RELOC           = 0x00000100u,
};

////////////////////////////////
//~ rjf: Symbol Table Entries (nlist_64)

// rjf: n_type bit masks
#define MACHO_N_STAB 0xe0u // if any of these bits set, a symbolic debugging entry
#define MACHO_N_PEXT 0x10u // private external symbol bit
#define MACHO_N_TYPE 0x0eu // mask for the type bits
#define MACHO_N_EXT  0x01u // external symbol bit, set for external symbols

// rjf: values of (n_type & MACHO_N_TYPE)
typedef U8 MACHO_NType;
enum
{
  MACHO_N_UNDF = 0x0, // undefined, n_sect == NO_SECT
  MACHO_N_ABS  = 0x2, // absolute, n_sect == NO_SECT
  MACHO_N_SECT = 0xe, // defined in section number n_sect
  MACHO_N_PBUD = 0xc, // prebound undefined (defined in a dylib)
  MACHO_N_INDR = 0xa, // indirect
};

#define MACHO_NO_SECT  0   // symbol is not in any section
#define MACHO_MAX_SECT 255 // 1 thru 255 inclusive

// rjf: n_desc bits
typedef U16 MACHO_NDesc;
enum
{
  MACHO_REFERENCE_TYPE                           = 0x7,
  MACHO_REFERENCE_FLAG_UNDEFINED_NON_LAZY        = 0,
  MACHO_REFERENCE_FLAG_UNDEFINED_LAZY            = 1,
  MACHO_REFERENCE_FLAG_DEFINED                   = 2,
  MACHO_REFERENCE_FLAG_PRIVATE_DEFINED           = 3,
  MACHO_REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY= 4,
  MACHO_REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY    = 5,
  MACHO_N_ARM_THUMB_DEF                          = 0x0008,
  MACHO_REFERENCED_DYNAMICALLY                   = 0x0010,
  MACHO_N_NO_DEAD_STRIP                          = 0x0020,
  MACHO_N_DESC_DISCARDED                         = 0x0020,
  MACHO_N_WEAK_REF                               = 0x0040,
  MACHO_N_WEAK_DEF                               = 0x0080,
  MACHO_N_REF_TO_WEAK                            = 0x0080,
  MACHO_N_SYMBOL_RESOLVER                        = 0x0100,
  MACHO_N_ALT_ENTRY                              = 0x0200,
  MACHO_N_COLD_FUNC                              = 0x0400,
};
#define MACHO_GET_LIBRARY_ORDINAL(n_desc) (((n_desc) >> 8) & 0xff)

// rjf: stab types (values of n_type when (n_type & MACHO_N_STAB) != 0)
typedef U8 MACHO_StabType;
enum
{
  MACHO_N_GSYM    = 0x20, // global symbol: name,,NO_SECT,type,0
  MACHO_N_FNAME   = 0x22, // procedure name (f77 kludge): name,,NO_SECT,0,0
  MACHO_N_FUN     = 0x24, // procedure: name,,n_sect,linenumber,address
  MACHO_N_STSYM   = 0x26, // static symbol: name,,n_sect,type,address
  MACHO_N_LCSYM   = 0x28, // .lcomm symbol: name,,n_sect,type,address
  MACHO_N_BNSYM   = 0x2e, // begin nsect sym: 0,,n_sect,0,address
  MACHO_N_AST     = 0x32, // AST file path: name,,NO_SECT,0,0
  MACHO_N_OPT     = 0x3c, // emitted with gcc2_compiled and in gcc source
  MACHO_N_RSYM    = 0x40, // register sym: name,,NO_SECT,type,register
  MACHO_N_SLINE   = 0x44, // src line: 0,,n_sect,linenumber,address
  MACHO_N_ENSYM   = 0x4e, // end nsect sym: 0,,n_sect,0,address
  MACHO_N_SSYM    = 0x60, // structure elt: name,,NO_SECT,type,struct_offset
  MACHO_N_SO      = 0x64, // source file name: name,,n_sect,0,address
  MACHO_N_OSO     = 0x66, // object file name: name,,(see below),1,st_mtime
  MACHO_N_LSYM    = 0x80, // local sym: name,,NO_SECT,type,offset
  MACHO_N_BINCL   = 0x82, // include file beginning: name,,NO_SECT,0,sum
  MACHO_N_SOL     = 0x84, // #included file name: name,,n_sect,0,address
  MACHO_N_PARAMS  = 0x86, // compiler parameters: name,,NO_SECT,0,0
  MACHO_N_VERSION = 0x88, // compiler version: name,,NO_SECT,0,0
  MACHO_N_OLEVEL  = 0x8a, // compiler -O level: name,,NO_SECT,0,0
  MACHO_N_PSYM    = 0xa0, // parameter: name,,NO_SECT,type,offset
  MACHO_N_EINCL   = 0xa2, // include file end: name,,NO_SECT,0,0
  MACHO_N_ENTRY   = 0xa4, // alternate entry: name,,n_sect,linenumber,address
  MACHO_N_LBRAC   = 0xc0, // left bracket: 0,,NO_SECT,nesting level,address
  MACHO_N_EXCL    = 0xc2, // deleted include file: name,,NO_SECT,0,sum
  MACHO_N_RBRAC   = 0xe0, // right bracket: 0,,NO_SECT,nesting level,address
  MACHO_N_BCOMM   = 0xe2, // begin common: name,,NO_SECT,0,0
  MACHO_N_ECOMM   = 0xe4, // end common: name,,n_sect,0,0
  MACHO_N_ECOML   = 0xe8, // end common (local name): 0,,n_sect,0,address
  MACHO_N_LENG    = 0xfe, // second stab entry with length information
};

////////////////////////////////
//~ rjf: Thread State Flavors (LC_UNIXTHREAD)

typedef U32 MACHO_ThreadStateFlavor;
enum
{
  MACHO_x86_THREAD_STATE64  = 4,  // 42 U32s: rax..r15, rip, rflags, cs, fs, gs
  MACHO_ARM_THREAD_STATE64  = 6,  // 68 U32s: x0..x28, fp, lr, sp, pc, cpsr, pad
};
#define MACHO_x86_THREAD_STATE64_RIP_INDEX 16 // U64 index of rip in x86_thread_state64_t
#define MACHO_ARM_THREAD_STATE64_PC_INDEX  32 // U64 index of pc in arm_thread_state64_t

////////////////////////////////
//~ rjf: Platforms (LC_BUILD_VERSION)

typedef U32 MACHO_Platform;
enum
{
  MACHO_PLATFORM_UNKNOWN          = 0,
  MACHO_PLATFORM_MACOS            = 1,
  MACHO_PLATFORM_IOS              = 2,
  MACHO_PLATFORM_TVOS             = 3,
  MACHO_PLATFORM_WATCHOS          = 4,
  MACHO_PLATFORM_BRIDGEOS         = 5,
  MACHO_PLATFORM_MACCATALYST      = 6,
  MACHO_PLATFORM_IOSSIMULATOR     = 7,
  MACHO_PLATFORM_TVOSSIMULATOR    = 8,
  MACHO_PLATFORM_WATCHOSSIMULATOR = 9,
  MACHO_PLATFORM_DRIVERKIT        = 10,
  MACHO_PLATFORM_VISIONOS         = 11,
  MACHO_PLATFORM_VISIONOSSIMULATOR= 12,
};

typedef U32 MACHO_Tool;
enum
{
  MACHO_TOOL_CLANG = 1,
  MACHO_TOOL_SWIFT = 2,
  MACHO_TOOL_LD    = 3,
  MACHO_TOOL_LLD   = 4,
};

////////////////////////////////
//~ rjf: On-Disk Structures

#pragma pack(push, 1)

//- rjf: fat (universal) binaries; all fields big-endian

typedef struct MACHO_FatHeader MACHO_FatHeader;
struct MACHO_FatHeader
{
  U32 magic;     // MACHO_FAT_MAGIC or MACHO_FAT_MAGIC_64
  U32 nfat_arch; // number of structs that follow
};

typedef struct MACHO_FatArch MACHO_FatArch;
struct MACHO_FatArch
{
  U32 cputype;
  U32 cpusubtype;
  U32 offset;    // file offset to this object file
  U32 size;      // size of this object file
  U32 align;     // alignment as a power of 2
};

typedef struct MACHO_FatArch64 MACHO_FatArch64;
struct MACHO_FatArch64
{
  U32 cputype;
  U32 cpusubtype;
  U64 offset;
  U64 size;
  U32 align;
  U32 reserved;
};

//- rjf: headers

typedef struct MACHO_Header MACHO_Header;
struct MACHO_Header
{
  U32 magic;      // MACHO_MH_MAGIC
  U32 cputype;
  U32 cpusubtype;
  U32 filetype;
  U32 ncmds;      // number of load commands
  U32 sizeofcmds; // the size of all the load commands
  U32 flags;
};

typedef struct MACHO_Header64 MACHO_Header64;
struct MACHO_Header64
{
  U32 magic;      // MACHO_MH_MAGIC_64
  U32 cputype;
  U32 cpusubtype;
  U32 filetype;
  U32 ncmds;      // number of load commands
  U32 sizeofcmds; // the size of all the load commands
  U32 flags;
  U32 reserved;
};

//- rjf: load commands

typedef struct MACHO_LoadCommand MACHO_LoadCommand;
struct MACHO_LoadCommand
{
  U32 cmd;     // type of load command
  U32 cmdsize; // total size of command in bytes (including this header)
};

typedef struct MACHO_SegmentCommand64 MACHO_SegmentCommand64;
struct MACHO_SegmentCommand64
{
  U32 cmd;          // MACHO_LC_SEGMENT_64
  U32 cmdsize;      // includes sizeof section_64 structs
  char segname[16]; // segment name (not necessarily NUL-terminated)
  U64 vmaddr;       // memory address of this segment
  U64 vmsize;       // memory size of this segment
  U64 fileoff;      // file offset of this segment
  U64 filesize;     // amount to map from the file
  U32 maxprot;      // maximum VM protection
  U32 initprot;     // initial VM protection
  U32 nsects;       // number of sections in segment
  U32 flags;        // flags
};

typedef struct MACHO_Section64 MACHO_Section64;
struct MACHO_Section64
{
  char sectname[16]; // name of this section
  char segname[16];  // segment this section goes in
  U64 addr;          // memory address of this section
  U64 size;          // size in bytes of this section
  U32 offset;        // file offset of this section
  U32 align;         // section alignment (power of 2)
  U32 reloff;        // file offset of relocation entries
  U32 nreloc;        // number of relocation entries
  U32 flags;         // flags (section type and attributes)
  U32 reserved1;     // reserved (for offset or index)
  U32 reserved2;     // reserved (for count or sizeof)
  U32 reserved3;     // reserved
};

typedef struct MACHO_SymtabCommand MACHO_SymtabCommand;
struct MACHO_SymtabCommand
{
  U32 cmd;     // MACHO_LC_SYMTAB
  U32 cmdsize; // sizeof(MACHO_SymtabCommand)
  U32 symoff;  // symbol table offset
  U32 nsyms;   // number of symbol table entries
  U32 stroff;  // string table offset
  U32 strsize; // string table size in bytes
};

typedef struct MACHO_DysymtabCommand MACHO_DysymtabCommand;
struct MACHO_DysymtabCommand
{
  U32 cmd;            // MACHO_LC_DYSYMTAB
  U32 cmdsize;        // sizeof(MACHO_DysymtabCommand)
  U32 ilocalsym;      // index to local symbols
  U32 nlocalsym;      // number of local symbols
  U32 iextdefsym;     // index to externally defined symbols
  U32 nextdefsym;     // number of externally defined symbols
  U32 iundefsym;      // index to undefined symbols
  U32 nundefsym;      // number of undefined symbols
  U32 tocoff;         // file offset to table of contents
  U32 ntoc;           // number of entries in table of contents
  U32 modtaboff;      // file offset to module table
  U32 nmodtab;        // number of module table entries
  U32 extrefsymoff;   // offset to referenced symbol table
  U32 nextrefsyms;    // number of referenced symbol table entries
  U32 indirectsymoff; // file offset to the indirect symbol table
  U32 nindirectsyms;  // number of indirect symbol table entries
  U32 extreloff;      // offset to external relocation entries
  U32 nextrel;        // number of external relocation entries
  U32 locreloff;      // offset to local relocation entries
  U32 nlocrel;        // number of local relocation entries
};

typedef struct MACHO_UUIDCommand MACHO_UUIDCommand;
struct MACHO_UUIDCommand
{
  U32 cmd;      // MACHO_LC_UUID
  U32 cmdsize;  // sizeof(MACHO_UUIDCommand)
  U8 uuid[16];  // the 128-bit uuid
};

typedef struct MACHO_EntryPointCommand MACHO_EntryPointCommand;
struct MACHO_EntryPointCommand
{
  U32 cmd;       // MACHO_LC_MAIN
  U32 cmdsize;   // 24
  U64 entryoff;  // file (__TEXT) offset of main()
  U64 stacksize; // if not zero, initial stack size
};

typedef struct MACHO_LinkeditDataCommand MACHO_LinkeditDataCommand;
struct MACHO_LinkeditDataCommand
{
  U32 cmd;      // MACHO_LC_CODE_SIGNATURE, MACHO_LC_SEGMENT_SPLIT_INFO, MACHO_LC_FUNCTION_STARTS, MACHO_LC_DATA_IN_CODE, MACHO_LC_DYLIB_CODE_SIGN_DRS, MACHO_LC_LINKER_OPTIMIZATION_HINT, MACHO_LC_DYLD_EXPORTS_TRIE, or MACHO_LC_DYLD_CHAINED_FIXUPS
  U32 cmdsize;  // sizeof(MACHO_LinkeditDataCommand)
  U32 dataoff;  // file offset of data in __LINKEDIT segment
  U32 datasize; // file size of data in __LINKEDIT segment
};

typedef struct MACHO_DylibCommand MACHO_DylibCommand;
struct MACHO_DylibCommand
{
  U32 cmd;                   // MACHO_LC_ID_DYLIB, MACHO_LC_LOAD_{,WEAK_}DYLIB, MACHO_LC_REEXPORT_DYLIB
  U32 cmdsize;               // includes pathname string
  U32 name_offset;           // offset of the library's path name, relative to the start of this command (lc_str)
  U32 timestamp;             // library's build time stamp
  U32 current_version;       // library's current version number
  U32 compatibility_version; // library's compatibility version number
};

typedef struct MACHO_DylinkerCommand MACHO_DylinkerCommand;
struct MACHO_DylinkerCommand
{
  U32 cmd;         // MACHO_LC_ID_DYLINKER, MACHO_LC_LOAD_DYLINKER or MACHO_LC_DYLD_ENVIRONMENT
  U32 cmdsize;     // includes pathname string
  U32 name_offset; // dynamic linker's path name (lc_str), relative to the start of this command
};

typedef struct MACHO_RpathCommand MACHO_RpathCommand;
struct MACHO_RpathCommand
{
  U32 cmd;         // MACHO_LC_RPATH
  U32 cmdsize;     // includes string
  U32 path_offset; // path to add to run path (lc_str)
};

typedef struct MACHO_BuildVersionCommand MACHO_BuildVersionCommand;
struct MACHO_BuildVersionCommand
{
  U32 cmd;      // MACHO_LC_BUILD_VERSION
  U32 cmdsize;  // sizeof(MACHO_BuildVersionCommand) + ntools * sizeof(MACHO_BuildToolVersion)
  U32 platform; // MACHO_PLATFORM_*
  U32 minos;    // X.Y.Z is encoded in nibbles xxxx.yy.zz
  U32 sdk;      // X.Y.Z is encoded in nibbles xxxx.yy.zz
  U32 ntools;   // number of tool entries following this
};

typedef struct MACHO_BuildToolVersion MACHO_BuildToolVersion;
struct MACHO_BuildToolVersion
{
  U32 tool;    // MACHO_TOOL_*
  U32 version; // version number of the tool
};

typedef struct MACHO_VersionMinCommand MACHO_VersionMinCommand;
struct MACHO_VersionMinCommand
{
  U32 cmd;     // MACHO_LC_VERSION_MIN_MACOSX / IPHONEOS / TVOS / WATCHOS
  U32 cmdsize; // sizeof(MACHO_VersionMinCommand)
  U32 version; // X.Y.Z is encoded in nibbles xxxx.yy.zz
  U32 sdk;     // X.Y.Z is encoded in nibbles xxxx.yy.zz
};

typedef struct MACHO_SourceVersionCommand MACHO_SourceVersionCommand;
struct MACHO_SourceVersionCommand
{
  U32 cmd;     // MACHO_LC_SOURCE_VERSION
  U32 cmdsize; // 16
  U64 version; // A.B.C.D.E packed as a24.b10.c10.d10.e10
};

typedef struct MACHO_DyldInfoCommand MACHO_DyldInfoCommand;
struct MACHO_DyldInfoCommand
{
  U32 cmd;            // MACHO_LC_DYLD_INFO or MACHO_LC_DYLD_INFO_ONLY
  U32 cmdsize;        // sizeof(MACHO_DyldInfoCommand)
  U32 rebase_off;     // file offset to rebase info
  U32 rebase_size;    // size of rebase info
  U32 bind_off;       // file offset to binding info
  U32 bind_size;      // size of binding info
  U32 weak_bind_off;  // file offset to weak binding info
  U32 weak_bind_size; // size of weak binding info
  U32 lazy_bind_off;  // file offset to lazy binding info
  U32 lazy_bind_size; // size of lazy binding infs
  U32 export_off;     // file offset to lazy binding info
  U32 export_size;    // size of lazy binding infs
};

typedef struct MACHO_EncryptionInfoCommand64 MACHO_EncryptionInfoCommand64;
struct MACHO_EncryptionInfoCommand64
{
  U32 cmd;       // MACHO_LC_ENCRYPTION_INFO_64
  U32 cmdsize;   // sizeof(MACHO_EncryptionInfoCommand64)
  U32 cryptoff;  // file offset of encrypted range
  U32 cryptsize; // file size of encrypted range
  U32 cryptid;   // which encryption system, 0 means not-encrypted yet
  U32 pad;       // padding to make this struct's size a multiple of 8 bytes
};

typedef struct MACHO_LinkerOptionCommand MACHO_LinkerOptionCommand;
struct MACHO_LinkerOptionCommand
{
  U32 cmd;     // MACHO_LC_LINKER_OPTION
  U32 cmdsize; // includes the concatenation of zero-terminated UTF8 strings, zero padded to a multiple of 4
  U32 count;   // number of strings
};

typedef struct MACHO_NoteCommand MACHO_NoteCommand;
struct MACHO_NoteCommand
{
  U32 cmd;            // MACHO_LC_NOTE
  U32 cmdsize;        // sizeof(MACHO_NoteCommand)
  char data_owner[16];// owner name for this LC_NOTE
  U64 offset;         // file offset of this data
  U64 size;           // length of data region
};

typedef struct MACHO_ThreadCommand MACHO_ThreadCommand;
struct MACHO_ThreadCommand
{
  U32 cmd;     // MACHO_LC_THREAD or MACHO_LC_UNIXTHREAD
  U32 cmdsize; // total size of this command
  // followed by one or more of: U32 flavor; U32 count; U32 state[count];
};

//- rjf: symbol table entry

typedef struct MACHO_NList64 MACHO_NList64;
struct MACHO_NList64
{
  U32 n_strx;  // index into the string table
  U8  n_type;  // type flag, see MACHO_N_*
  U8  n_sect;  // section number or MACHO_NO_SECT
  U16 n_desc;  // see MACHO_N_* n_desc bits / stab info
  U64 n_value; // value of this symbol (or stab offset)
};

//- rjf: relocation entry (non-scattered)

typedef struct MACHO_RelocationInfo MACHO_RelocationInfo;
struct MACHO_RelocationInfo
{
  S32 r_address; // offset in the section to what is being relocated
  U32 r_info;    // r_symbolnum:24, r_pcrel:1, r_length:2, r_extern:1, r_type:4
};
#define MACHO_RELOC_SYMBOLNUM(r_info) ((r_info) & 0xffffffu)
#define MACHO_RELOC_PCREL(r_info)     (((r_info) >> 24) & 0x1u)
#define MACHO_RELOC_LENGTH(r_info)    (((r_info) >> 25) & 0x3u)
#define MACHO_RELOC_EXTERN(r_info)    (((r_info) >> 27) & 0x1u)
#define MACHO_RELOC_TYPE(r_info)      (((r_info) >> 28) & 0xfu)

#pragma pack(pop)

StaticAssert(sizeof(MACHO_FatHeader) == 8,              macho_fat_header_size_check);
StaticAssert(sizeof(MACHO_FatArch) == 20,               macho_fat_arch_size_check);
StaticAssert(sizeof(MACHO_FatArch64) == 32,             macho_fat_arch64_size_check);
StaticAssert(sizeof(MACHO_Header) == 28,                macho_header_size_check);
StaticAssert(sizeof(MACHO_Header64) == 32,              macho_header64_size_check);
StaticAssert(sizeof(MACHO_LoadCommand) == 8,            macho_load_command_size_check);
StaticAssert(sizeof(MACHO_SegmentCommand64) == 72,      macho_segment_command64_size_check);
StaticAssert(sizeof(MACHO_Section64) == 80,             macho_section64_size_check);
StaticAssert(sizeof(MACHO_SymtabCommand) == 24,         macho_symtab_command_size_check);
StaticAssert(sizeof(MACHO_DysymtabCommand) == 80,       macho_dysymtab_command_size_check);
StaticAssert(sizeof(MACHO_UUIDCommand) == 24,           macho_uuid_command_size_check);
StaticAssert(sizeof(MACHO_EntryPointCommand) == 24,     macho_entry_point_command_size_check);
StaticAssert(sizeof(MACHO_LinkeditDataCommand) == 16,   macho_linkedit_data_command_size_check);
StaticAssert(sizeof(MACHO_DylibCommand) == 24,          macho_dylib_command_size_check);
StaticAssert(sizeof(MACHO_DylinkerCommand) == 12,       macho_dylinker_command_size_check);
StaticAssert(sizeof(MACHO_BuildVersionCommand) == 24,   macho_build_version_command_size_check);
StaticAssert(sizeof(MACHO_VersionMinCommand) == 16,     macho_version_min_command_size_check);
StaticAssert(sizeof(MACHO_SourceVersionCommand) == 16,  macho_source_version_command_size_check);
StaticAssert(sizeof(MACHO_DyldInfoCommand) == 48,       macho_dyld_info_command_size_check);
StaticAssert(sizeof(MACHO_NoteCommand) == 40,           macho_note_command_size_check);
StaticAssert(sizeof(MACHO_NList64) == 16,               macho_nlist64_size_check);
StaticAssert(sizeof(MACHO_RelocationInfo) == 8,         macho_relocation_info_size_check);

////////////////////////////////
//~ rjf: Mach-O -> Codebase Enum Conversions

internal Arch arch_from_macho_cputype(MACHO_CpuType cputype);

////////////////////////////////
//~ rjf: Helpers

internal String8 macho_string_from_name16(char *name);
internal B32     macho_section_type_is_zerofill(U32 section_flags);

////////////////////////////////
//~ rjf: Stringizing

internal String8 macho_string_from_cputype(MACHO_CpuType cputype);
internal String8 macho_string_from_filetype(MACHO_FileType filetype);
internal String8 macho_string_from_load_command_type(MACHO_LoadCommandType cmd);
internal String8 macho_string_from_section_type(MACHO_SectionType type);
internal String8 macho_string_from_stab_type(MACHO_StabType stab);
internal String8 macho_string_from_platform(MACHO_Platform platform);

#endif // MACHO_H
