// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef RDI_FROM_DWARF_H
#define RDI_FROM_DWARF_H

////////////////////////////////
//~ rjf: Tag Hash Cache Types

typedef struct D2R_TagHashNode D2R_TagHashNode;
struct D2R_TagHashNode
{
  D2R_TagHashNode *next;
  U64 container_ancestor_info_off;
  U64 info_off;
  U64 hash;
};

////////////////////////////////
//~ rjf: Unique Tag Tree Deduplication Types

typedef struct D2R_TreeHash D2R_TreeHash;
struct D2R_TreeHash
{
  U64 info_off;
  U64 hash;
};

typedef struct D2R_TreeHashNode D2R_TreeHashNode;
struct D2R_TreeHashNode
{
  D2R_TreeHashNode *next;
  D2R_TreeHash v;
};

typedef enum D2R_UniqueTagKind
{
  D2R_UniqueTagKind_Type,
  D2R_UniqueTagKind_Namespace,
  D2R_UniqueTagKind_COUNT
}
D2R_UniqueTagKind;

typedef struct D2R_UniqueTagNode D2R_UniqueTagNode;
struct D2R_UniqueTagNode
{
  D2R_UniqueTagNode *next;
  D2R_UniqueTagKind kind;
  U64 hash;
  U64 info_off;
  U64 container_ancestor_info_off;
  U64 dependency_count;
  U64 order_idx;
};

typedef struct D2R_UnitDedupedTagNode D2R_UnitDedupedTagNode;
struct D2R_UnitDedupedTagNode
{
  D2R_UnitDedupedTagNode *next;
  U64 src_info_off;
  U64 dst_hash;
  U64 dependency_count;
};

typedef struct D2R_UnitDedupedTagMap D2R_UnitDedupedTagMap;
struct D2R_UnitDedupedTagMap
{
  D2R_UnitDedupedTagNode **slots;
  U64 slots_count;
};

////////////////////////////////
//~ rjf: Container Lookup Types (see d2r_container_from_info_off)

// the tables of d2r_convert which take a tag's .debug_info offset to the RDIM
// namespace / UDT type which contains it
typedef struct D2R_ContainerLookup D2R_ContainerLookup;
struct D2R_ContainerLookup
{
  D2R_TagHashNode **tag_hash_slots;
  U64 tag_hash_slots_count;
  Rng1U64Array unit_info_tag_ranges_array;
  D2R_UnitDedupedTagMap *unit_deduped_tag_maps;
  D2R_UniqueTagNode **unique_tag_slots;
  U64 unique_tag_slots_count;
  RDIM_Type **type_from_idx_map;
  RDIM_Namespace **namespace_from_idx_map;
};

////////////////////////////////
//~ rjf: Conversion Stage Inputs (New)

typedef struct D2R_ConvertParams D2R_ConvertParams;
struct D2R_ConvertParams
{
  String8 exe_name;
  String8 exe_data;
  ExecutableImageKind exe_kind; // NOTE: may be Null; Macho selects the Mach-O TLS descriptor model
  Arch arch;
  U64 base_vaddr;
  RDIM_BinarySectionList binary_sections;
  DW_Raw raw;
  PathStyle path_style;
  RDIM_SubsetFlags subset_flags;
  B32 deterministic;
};

////////////////////////////////
//~ rjf: Helpers

internal int d2r_tree_hash_is_less_than(D2R_TreeHash *a, D2R_TreeHash *b);
internal int d2r_unique_tag_node_is_less_than(D2R_UniqueTagNode **l, D2R_UniqueTagNode **r);

////////////////////////////////
//~ rjf: Line Table Building Types (see d2r_convert)
//
// a unit's line program is first decoded into rows; once the inline sites are
// known, the rows are distributed into the unit's line table and the inline
// sites' line tables (RDI, like PDB, attributes inlined code to the call site's
// line in the unit's table, and to the inlined function's lines in the site's).

// a row begins a line; its code extends to the next row of its sequence.
// rows which are not statements (is_stmt == 0) extend the line before them
// in whichever table they land in, and only begin one where there is none.
typedef struct D2R_LineRow D2R_LineRow;
struct D2R_LineRow
{
  U64 voff;
  RDIM_SrcFile *src_file;
  U32 line_num;
  U16 col_num;
  B16 is_stmt;
};

typedef struct D2R_LineRowChunk D2R_LineRowChunk;
struct D2R_LineRowChunk
{
  D2R_LineRowChunk *next;
  D2R_LineRow *v;
  U64 count;
  U64 cap;
};

// one DWARF line sequence: rows with increasing voffs, the last of which ends at voff_opl
typedef struct D2R_LineSeq D2R_LineSeq;
struct D2R_LineSeq
{
  D2R_LineSeq *next;
  D2R_LineRowChunk *first_chunk;
  D2R_LineRowChunk *last_chunk;
  U64 voff_opl;
};

typedef struct D2R_UnitLineSeqs D2R_UnitLineSeqs;
struct D2R_UnitLineSeqs
{
  D2R_LineSeq *first;
  D2R_LineSeq *last;
};

// per-unit map: src file -> the sequences built for it (for the source line maps)
typedef struct D2R_FileSeqNode D2R_FileSeqNode;
struct D2R_FileSeqNode
{
  D2R_FileSeqNode *next;
  RDIM_SrcFile *src_file;
  RDIM_SrcFileLineMapFragment *first_line_map_fragment;
  RDIM_SrcFileLineMapFragment *last_line_map_fragment;
};

typedef struct D2R_FileSeqMap D2R_FileSeqMap;
struct D2R_FileSeqMap
{
  U64 slots_count;
  D2R_FileSeqNode **slots;
};

// accumulates the sequence currently being built for one line table
typedef struct D2R_LineTableBuilder D2R_LineTableBuilder;
struct D2R_LineTableBuilder
{
  D2R_LineTableBuilder *next_open;
  RDIM_LineTable *table;
  RDIM_SrcFile *src_file;
  U64 *voffs;
  U32 *line_nums;
  U16 *col_nums;
  U64 line_count;
  U64 line_cap;
  U64 voff_opl;
  B32 is_open;
};

// an inline site, as found among a unit's tags
typedef struct D2R_InlineNode D2R_InlineNode;
struct D2R_InlineNode
{
  D2R_InlineNode *next;
  RDIM_InlineSite *site;
  RDIM_Rng1U64List ranges;
  RDIM_SrcFile *call_src_file;
  U32 call_line;
  U16 call_col;
  U64 depth;               // number of inlined subroutines this one is nested in (tag tree)
  D2R_InlineNode *parent;  // the nearest of them (tag tree); 0 when inlined into the function itself
  D2R_LineTableBuilder *builder;
};

// one range of an inline site; sorted by (min ascending, max descending), linked to the enclosing range
typedef struct D2R_InlineRange D2R_InlineRange;
struct D2R_InlineRange
{
  U64 min;
  U64 max;
  D2R_InlineNode *node;
  D2R_InlineRange *parent;
};

internal int d2r_inline_range_compare(D2R_InlineRange *a, D2R_InlineRange *b);
internal D2R_InlineRange *d2r_inline_range_from_voff(D2R_InlineRange *ranges, U64 count, U64 voff, U64 *next_range_min_out);
internal void d2r_line_table_builder_flush(Arena *arena, RDIM_LineTableChunkList *line_tables, D2R_FileSeqMap *file_seq_map, D2R_LineTableBuilder *b);
internal void d2r_line_table_builder_append(Arena *arena, Arena *scratch_arena, RDIM_LineTableChunkList *line_tables, D2R_FileSeqMap *file_seq_map, D2R_LineTableBuilder *b, D2R_LineTableBuilder **first_open_builder, U64 voff_min, U64 voff_max, RDIM_SrcFile *src_file, U32 line_num, U16 col_num, B32 is_stmt);
internal void d2r_distribute_line_row(Arena *arena, Arena *scratch_arena, RDIM_LineTableChunkList *line_tables, D2R_FileSeqMap *file_seq_map, D2R_LineTableBuilder *unit_builder, D2R_LineTableBuilder **first_open_builder, D2R_InlineRange *inline_ranges, U64 inline_range_count, D2R_LineRow *row, U64 row_voff_opl);

////////////////////////////////
//~ rjf: Main Conversion Entry Point (New)

internal RDIM_BakeParams d2r_convert(Arena *arena, D2R_ConvertParams *params);

#endif // RDI_FROM_DWARF_H
