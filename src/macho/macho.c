// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Mach-O -> Codebase Enum Conversions

internal Arch
arch_from_macho_cputype(MACHO_CpuType cputype)
{
  Arch arch = Arch_Null;
  switch(cputype)
  {
    default:{}break;
    case MACHO_CPU_TYPE_ARM64:  {arch = Arch_arm64;}break;
    case MACHO_CPU_TYPE_X86_64: {arch = Arch_x64;}break;
    case MACHO_CPU_TYPE_ARM:    {arch = Arch_arm32;}break;
    case MACHO_CPU_TYPE_I386:   {arch = Arch_x86;}break;
  }
  return arch;
}

////////////////////////////////
//~ rjf: Helpers

internal String8
macho_string_from_name16(char *name)
{
  // NOTE: segment/section names are 16 bytes and are NOT NUL-terminated when
  // they use all 16 characters (e.g. "__debug_str_offs").
  String8 result = str8_cstring_capped(name, name + 16);
  return result;
}

internal B32
macho_section_type_is_zerofill(U32 section_flags)
{
  U32 type = (section_flags & MACHO_SECTION_TYPE);
  B32 result = (type == MACHO_S_ZEROFILL ||
                type == MACHO_S_GB_ZEROFILL ||
                type == MACHO_S_THREAD_LOCAL_ZEROFILL);
  return result;
}

////////////////////////////////
//~ rjf: Stringizing

internal String8
macho_string_from_cputype(MACHO_CpuType cputype)
{
  String8 result = str8_lit("");
  switch(cputype)
  {
    default:{}break;
    case MACHO_CPU_TYPE_ANY:       {result = str8_lit("ANY");}break;
    case MACHO_CPU_TYPE_VAX:       {result = str8_lit("VAX");}break;
    case MACHO_CPU_TYPE_MC680x0:   {result = str8_lit("MC680x0");}break;
    case MACHO_CPU_TYPE_X86:       {result = str8_lit("X86");}break;
    case MACHO_CPU_TYPE_X86_64:    {result = str8_lit("X86_64");}break;
    case MACHO_CPU_TYPE_MC98000:   {result = str8_lit("MC98000");}break;
    case MACHO_CPU_TYPE_HPPA:      {result = str8_lit("HPPA");}break;
    case MACHO_CPU_TYPE_ARM:       {result = str8_lit("ARM");}break;
    case MACHO_CPU_TYPE_ARM64:     {result = str8_lit("ARM64");}break;
    case MACHO_CPU_TYPE_ARM64_32:  {result = str8_lit("ARM64_32");}break;
    case MACHO_CPU_TYPE_MC88000:   {result = str8_lit("MC88000");}break;
    case MACHO_CPU_TYPE_SPARC:     {result = str8_lit("SPARC");}break;
    case MACHO_CPU_TYPE_I860:      {result = str8_lit("I860");}break;
    case MACHO_CPU_TYPE_POWERPC:   {result = str8_lit("POWERPC");}break;
    case MACHO_CPU_TYPE_POWERPC64: {result = str8_lit("POWERPC64");}break;
  }
  return result;
}

internal String8
macho_string_from_filetype(MACHO_FileType filetype)
{
  String8 result = str8_lit("");
  switch(filetype)
  {
    default:{}break;
    case MACHO_MH_OBJECT:      {result = str8_lit("MH_OBJECT");}break;
    case MACHO_MH_EXECUTE:     {result = str8_lit("MH_EXECUTE");}break;
    case MACHO_MH_FVMLIB:      {result = str8_lit("MH_FVMLIB");}break;
    case MACHO_MH_CORE:        {result = str8_lit("MH_CORE");}break;
    case MACHO_MH_PRELOAD:     {result = str8_lit("MH_PRELOAD");}break;
    case MACHO_MH_DYLIB:       {result = str8_lit("MH_DYLIB");}break;
    case MACHO_MH_DYLINKER:    {result = str8_lit("MH_DYLINKER");}break;
    case MACHO_MH_BUNDLE:      {result = str8_lit("MH_BUNDLE");}break;
    case MACHO_MH_DYLIB_STUB:  {result = str8_lit("MH_DYLIB_STUB");}break;
    case MACHO_MH_DSYM:        {result = str8_lit("MH_DSYM");}break;
    case MACHO_MH_KEXT_BUNDLE: {result = str8_lit("MH_KEXT_BUNDLE");}break;
    case MACHO_MH_FILESET:     {result = str8_lit("MH_FILESET");}break;
  }
  return result;
}

internal String8
macho_string_from_load_command_type(MACHO_LoadCommandType cmd)
{
  String8 result = str8_lit("");
  switch(cmd)
  {
    default:{}break;
#define X(name) case MACHO_##name: {result = str8_lit(#name);}break;
    X(LC_SEGMENT)
    X(LC_SYMTAB)
    X(LC_SYMSEG)
    X(LC_THREAD)
    X(LC_UNIXTHREAD)
    X(LC_LOADFVMLIB)
    X(LC_IDFVMLIB)
    X(LC_IDENT)
    X(LC_FVMFILE)
    X(LC_PREPAGE)
    X(LC_DYSYMTAB)
    X(LC_LOAD_DYLIB)
    X(LC_ID_DYLIB)
    X(LC_LOAD_DYLINKER)
    X(LC_ID_DYLINKER)
    X(LC_PREBOUND_DYLIB)
    X(LC_ROUTINES)
    X(LC_SUB_FRAMEWORK)
    X(LC_SUB_UMBRELLA)
    X(LC_SUB_CLIENT)
    X(LC_SUB_LIBRARY)
    X(LC_TWOLEVEL_HINTS)
    X(LC_PREBIND_CKSUM)
    X(LC_LOAD_WEAK_DYLIB)
    X(LC_SEGMENT_64)
    X(LC_ROUTINES_64)
    X(LC_UUID)
    X(LC_RPATH)
    X(LC_CODE_SIGNATURE)
    X(LC_SEGMENT_SPLIT_INFO)
    X(LC_REEXPORT_DYLIB)
    X(LC_LAZY_LOAD_DYLIB)
    X(LC_ENCRYPTION_INFO)
    X(LC_DYLD_INFO)
    X(LC_DYLD_INFO_ONLY)
    X(LC_LOAD_UPWARD_DYLIB)
    X(LC_VERSION_MIN_MACOSX)
    X(LC_VERSION_MIN_IPHONEOS)
    X(LC_FUNCTION_STARTS)
    X(LC_DYLD_ENVIRONMENT)
    X(LC_MAIN)
    X(LC_DATA_IN_CODE)
    X(LC_SOURCE_VERSION)
    X(LC_DYLIB_CODE_SIGN_DRS)
    X(LC_ENCRYPTION_INFO_64)
    X(LC_LINKER_OPTION)
    X(LC_LINKER_OPTIMIZATION_HINT)
    X(LC_VERSION_MIN_TVOS)
    X(LC_VERSION_MIN_WATCHOS)
    X(LC_NOTE)
    X(LC_BUILD_VERSION)
    X(LC_DYLD_EXPORTS_TRIE)
    X(LC_DYLD_CHAINED_FIXUPS)
    X(LC_FILESET_ENTRY)
    X(LC_ATOM_INFO)
#undef X
  }
  return result;
}

internal String8
macho_string_from_section_type(MACHO_SectionType type)
{
  String8 result = str8_lit("");
  switch(type & MACHO_SECTION_TYPE)
  {
    default:{}break;
#define X(name) case MACHO_##name: {result = str8_lit(#name);}break;
    X(S_REGULAR)
    X(S_ZEROFILL)
    X(S_CSTRING_LITERALS)
    X(S_4BYTE_LITERALS)
    X(S_8BYTE_LITERALS)
    X(S_LITERAL_POINTERS)
    X(S_NON_LAZY_SYMBOL_POINTERS)
    X(S_LAZY_SYMBOL_POINTERS)
    X(S_SYMBOL_STUBS)
    X(S_MOD_INIT_FUNC_POINTERS)
    X(S_MOD_TERM_FUNC_POINTERS)
    X(S_COALESCED)
    X(S_GB_ZEROFILL)
    X(S_INTERPOSING)
    X(S_16BYTE_LITERALS)
    X(S_DTRACE_DOF)
    X(S_LAZY_DYLIB_SYMBOL_POINTERS)
    X(S_THREAD_LOCAL_REGULAR)
    X(S_THREAD_LOCAL_ZEROFILL)
    X(S_THREAD_LOCAL_VARIABLES)
    X(S_THREAD_LOCAL_VARIABLE_POINTERS)
    X(S_THREAD_LOCAL_INIT_FUNCTION_POINTERS)
    X(S_INIT_FUNC_OFFSETS)
#undef X
  }
  return result;
}

internal String8
macho_string_from_stab_type(MACHO_StabType stab)
{
  String8 result = str8_lit("");
  switch(stab)
  {
    default:{}break;
#define X(name) case MACHO_##name: {result = str8_lit(#name);}break;
    X(N_GSYM)
    X(N_FNAME)
    X(N_FUN)
    X(N_STSYM)
    X(N_LCSYM)
    X(N_BNSYM)
    X(N_AST)
    X(N_OPT)
    X(N_RSYM)
    X(N_SLINE)
    X(N_ENSYM)
    X(N_SSYM)
    X(N_SO)
    X(N_OSO)
    X(N_LSYM)
    X(N_BINCL)
    X(N_SOL)
    X(N_PARAMS)
    X(N_VERSION)
    X(N_OLEVEL)
    X(N_PSYM)
    X(N_EINCL)
    X(N_ENTRY)
    X(N_LBRAC)
    X(N_EXCL)
    X(N_RBRAC)
    X(N_BCOMM)
    X(N_ECOMM)
    X(N_ECOML)
    X(N_LENG)
#undef X
  }
  return result;
}

internal String8
macho_string_from_platform(MACHO_Platform platform)
{
  String8 result = str8_lit("");
  switch(platform)
  {
    default:{}break;
    case MACHO_PLATFORM_MACOS:             {result = str8_lit("macOS");}break;
    case MACHO_PLATFORM_IOS:               {result = str8_lit("iOS");}break;
    case MACHO_PLATFORM_TVOS:              {result = str8_lit("tvOS");}break;
    case MACHO_PLATFORM_WATCHOS:           {result = str8_lit("watchOS");}break;
    case MACHO_PLATFORM_BRIDGEOS:          {result = str8_lit("bridgeOS");}break;
    case MACHO_PLATFORM_MACCATALYST:       {result = str8_lit("Mac Catalyst");}break;
    case MACHO_PLATFORM_IOSSIMULATOR:      {result = str8_lit("iOS Simulator");}break;
    case MACHO_PLATFORM_TVOSSIMULATOR:     {result = str8_lit("tvOS Simulator");}break;
    case MACHO_PLATFORM_WATCHOSSIMULATOR:  {result = str8_lit("watchOS Simulator");}break;
    case MACHO_PLATFORM_DRIVERKIT:         {result = str8_lit("DriverKit");}break;
    case MACHO_PLATFORM_VISIONOS:          {result = str8_lit("visionOS");}break;
    case MACHO_PLATFORM_VISIONOSSIMULATOR: {result = str8_lit("visionOS Simulator");}break;
  }
  return result;
}
