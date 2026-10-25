// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MACHO_DUMP_H
#define MACHO_DUMP_H

#define MACHO_DumpSubset_XList \
  X(Header,         macho_header,          "MACH-O HEADER")\
  X(LoadCommands,   macho_load_commands,   "MACH-O LOAD COMMANDS")\
  X(Segments,       macho_segments,        "MACH-O SEGMENTS")\
  X(Symbols,        macho_symbols,         "MACH-O SYMBOLS")\
  X(FunctionStarts, macho_function_starts, "MACH-O FUNCTION STARTS")

typedef U32 MACHO_DumpSubset;
enum
{
#define X(name, name_lower, title) MACHO_DumpSubset_##name,
  MACHO_DumpSubset_XList
#undef X
};

typedef U32 MACHO_DumpSubsetFlags;
enum
{
#define X(name, name_lower, title) MACHO_DumpSubsetFlag_##name = (1 << MACHO_DumpSubset_##name),
  MACHO_DumpSubset_XList
#undef X
  MACHO_DumpSubsetFlag_All = ~0,
};

internal String8List macho_dump(Arena *arena, String8 data, MACHO_Bin *bin, MACHO_DumpSubsetFlags flags);

#endif // MACHO_DUMP_H
