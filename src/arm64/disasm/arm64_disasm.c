// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Overview
//
// A64 instruction decoder / disassembler. The decoder is total: every 32-bit
// word produces an instruction of size 4; words that are not understood are
// rendered as `.inst 0x........` with no flags. The top-level dispatch follows
// the "Top-level encodings for A64" table of the Arm ARM (bits [28:25]), and
// each major class is then decoded with explicit masks. Text is accumulated in
// a small stack buffer and copied to the arena once at the end.

////////////////////////////////
//~ rjf: Decoder Context

typedef struct ARM64_DasmCtx ARM64_DasmCtx;
struct ARM64_DasmCtx
{
  // inputs
  U32 inst;
  U64 vaddr;

  // analysis outputs
  DASM_InstFlags flags;
  U64 dst_vaddr;
  ARCH_RegCode reg_codes[2]; // [0] = dst slot, [1] = src slot
  S64 reg_offs[2];

  // text building
  U32 op_count;
  U64 len;
  char buf[192];
};

////////////////////////////////
//~ rjf: Bit Helpers

#define ARM64_DASM_BITS(v, hi, lo) ((U32)(((U64)(v) >> (lo)) & ((1ull << ((hi) - (lo) + 1)) - 1)))
#define ARM64_DASM_BIT(v, b)       ((U32)(((v) >> (b)) & 1u))

internal S64
arm64_dasm_sext(U64 v, U32 bits)
{
  U64 m = 1ull << (bits - 1);
  v &= (m << 1) - 1;
  return (S64)((v ^ m) - m);
}

internal U32
arm64_dasm_highest_set_bit(U32 v)
{
  U32 result = 0;
  for(U32 i = 0; i < 32; i += 1)
  {
    if(v & (1u << i)) { result = i; }
  }
  return result;
}

////////////////////////////////
//~ rjf: Text Building Helpers

internal void
arm64_dasm_put(ARM64_DasmCtx *ctx, char *s)
{
  for(; *s != 0 && ctx->len < sizeof(ctx->buf) - 1; s += 1)
  {
    ctx->buf[ctx->len] = *s;
    ctx->len += 1;
  }
  ctx->buf[ctx->len] = 0;
}

internal void
arm64_dasm_putf(ARM64_DasmCtx *ctx, char *fmt, ...)
{
  if(ctx->len < sizeof(ctx->buf) - 1)
  {
    va_list args;
    va_start(args, fmt);
    int n = raddbg_vsnprintf(ctx->buf + ctx->len, (int)(sizeof(ctx->buf) - ctx->len), fmt, args);
    va_end(args);
    if(n < 0) { n = 0; }
    ctx->len = Min(ctx->len + (U64)n, sizeof(ctx->buf) - 1);
    ctx->buf[ctx->len] = 0;
  }
}

// rjf: mnemonic; resets the operand list
internal void
arm64_dasm_mnem(ARM64_DasmCtx *ctx, char *m)
{
  arm64_dasm_put(ctx, m);
  ctx->op_count = 0;
}

internal void
arm64_dasm_mnemf(ARM64_DasmCtx *ctx, char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  if(ctx->len < sizeof(ctx->buf) - 1)
  {
    int n = raddbg_vsnprintf(ctx->buf + ctx->len, (int)(sizeof(ctx->buf) - ctx->len), fmt, args);
    if(n < 0) { n = 0; }
    ctx->len = Min(ctx->len + (U64)n, sizeof(ctx->buf) - 1);
    ctx->buf[ctx->len] = 0;
  }
  va_end(args);
  ctx->op_count = 0;
}

// rjf: operand separator (" " after the mnemonic, ", " between operands)
internal void
arm64_dasm_op(ARM64_DasmCtx *ctx)
{
  arm64_dasm_put(ctx, ctx->op_count == 0 ? " " : ", ");
  ctx->op_count += 1;
}

internal void
arm64_dasm_op_str(ARM64_DasmCtx *ctx, char *s)
{
  arm64_dasm_op(ctx);
  arm64_dasm_put(ctx, s);
}

internal void
arm64_dasm_op_f(ARM64_DasmCtx *ctx, char *fmt, ...)
{
  arm64_dasm_op(ctx);
  va_list args;
  va_start(args, fmt);
  if(ctx->len < sizeof(ctx->buf) - 1)
  {
    int n = raddbg_vsnprintf(ctx->buf + ctx->len, (int)(sizeof(ctx->buf) - ctx->len), fmt, args);
    if(n < 0) { n = 0; }
    ctx->len = Min(ctx->len + (U64)n, sizeof(ctx->buf) - 1);
    ctx->buf[ctx->len] = 0;
  }
  va_end(args);
}

// rjf: immediates: small values in decimal, larger ones in hex (sign kept)
internal void
arm64_dasm_put_imm(ARM64_DasmCtx *ctx, S64 v)
{
  if(v < 0)
  {
    U64 mag = (U64)0 - (U64)v;
    if(mag < 10) { arm64_dasm_putf(ctx, "#-%I64u", mag); }
    else         { arm64_dasm_putf(ctx, "#-0x%I64x", mag); }
  }
  else
  {
    if((U64)v < 10) { arm64_dasm_putf(ctx, "#%I64u", (U64)v); }
    else            { arm64_dasm_putf(ctx, "#0x%I64x", (U64)v); }
  }
}

internal void
arm64_dasm_op_imm(ARM64_DasmCtx *ctx, S64 v)
{
  arm64_dasm_op(ctx);
  arm64_dasm_put_imm(ctx, v);
}

// rjf: unsigned immediate (bitmasks etc.) - always hex unless tiny
internal void
arm64_dasm_op_immu(ARM64_DasmCtx *ctx, U64 v)
{
  arm64_dasm_op(ctx);
  if(v < 10) { arm64_dasm_putf(ctx, "#%I64u", v); }
  else       { arm64_dasm_putf(ctx, "#0x%I64x", v); }
}

// rjf: small decimal immediates (shift amounts, bit numbers, lane indices)
internal void
arm64_dasm_op_dec(ARM64_DasmCtx *ctx, U64 v)
{
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "#%I64u", v);
}

internal void
arm64_dasm_op_addr(ARM64_DasmCtx *ctx, U64 addr)
{
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "0x%I64x", addr);
}

// rjf: floating point immediate (FMOV etc.) - minimal exact decimal form
internal void
arm64_dasm_op_fimm(ARM64_DasmCtx *ctx, F64 v)
{
  char tmp[64];
  int n = raddbg_snprintf(tmp, sizeof(tmp), "%.8f", v);
  if(n < 0) { n = 0; }
  if(n > (int)sizeof(tmp) - 1) { n = (int)sizeof(tmp) - 1; }
  while(n > 2 && tmp[n-1] == '0' && tmp[n-2] != '.') { n -= 1; }
  tmp[n] = 0;
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "#%s", tmp);
}

////////////////////////////////
//~ rjf: Register Helpers

// rjf: general purpose register text. `sp_ok` selects the SP (vs. ZR) meaning of register 31.
internal void
arm64_dasm_put_gpr(ARM64_DasmCtx *ctx, U32 reg, B32 is64, B32 sp_ok)
{
  reg &= 31;
  if(reg == 31)
  {
    if(sp_ok) { arm64_dasm_put(ctx, is64 ? "sp" : "wsp"); }
    else      { arm64_dasm_put(ctx, is64 ? "xzr" : "wzr"); }
  }
  else
  {
    arm64_dasm_putf(ctx, "%c%u", is64 ? 'x' : 'w', reg);
  }
}

internal void
arm64_dasm_op_gpr(ARM64_DasmCtx *ctx, U32 reg, B32 is64, B32 sp_ok)
{
  arm64_dasm_op(ctx);
  arm64_dasm_put_gpr(ctx, reg, is64, sp_ok);
}

// rjf: general purpose register -> ARM64_RegCode (0 for the zero register)
internal ARCH_RegCode
arm64_dasm_gpr_code(U32 reg, B32 is64, B32 sp_ok)
{
  ARCH_RegCode result = 0;
  reg &= 31;
  if(is64)
  {
    if(reg < 29)       { result = (ARCH_RegCode)(ARM64_RegCode_x0 + reg); }
    else if(reg == 29) { result = ARM64_RegCode_fp; }
    else if(reg == 30) { result = ARM64_RegCode_lr; }
    else if(sp_ok)     { result = ARM64_RegCode_sp; }
  }
  else
  {
    if(reg < 31)       { result = (ARCH_RegCode)(ARM64_RegCode_w0 + reg); }
    else if(sp_ok)     { result = ARM64_RegCode_wsp; }
  }
  return result;
}

// rjf: SIMD&FP register -> ARM64_RegCode; kind is one of 'v','q','d','s','h','b'
internal ARCH_RegCode
arm64_dasm_vreg_code(U32 reg, char kind)
{
  U32 code = 0;
  reg &= 31;
  switch(kind)
  {
    default:
    case 'v':{code = ARM64_RegCode_v0 + reg;}break;
    case 'q':{code = ARM64_RegCode_q0 + reg;}break;
    case 'd':{code = ARM64_RegCode_d0 + reg;}break;
    case 's':{code = ARM64_RegCode_s0 + reg;}break;
    case 'h':{code = ARM64_RegCode_h0 + reg;}break;
    case 'b':{code = ARM64_RegCode_b0 + reg;}break;
  }
  return (ARCH_RegCode)code;
}

// rjf: scalar SIMD&FP register text, e.g. "d0"
internal void
arm64_dasm_op_sreg(ARM64_DasmCtx *ctx, U32 reg, char kind)
{
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "%c%u", kind, reg & 31);
}

// rjf: vector register with arrangement, e.g. "v0.16b"
internal void
arm64_dasm_op_vreg(ARM64_DasmCtx *ctx, U32 reg, char *arr)
{
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "v%u.%s", reg & 31, arr);
}

// rjf: vector element, e.g. "v0.s[1]"
internal void
arm64_dasm_op_velem(ARM64_DasmCtx *ctx, U32 reg, char kind, U32 index)
{
  arm64_dasm_op(ctx);
  arm64_dasm_putf(ctx, "v%u.%c[%u]", reg & 31, kind, index);
}

// rjf: vector register list, e.g. "{v0.16b, v1.16b}"
internal void
arm64_dasm_op_vlist(ARM64_DasmCtx *ctx, U32 reg, U32 count, char *arr)
{
  arm64_dasm_op(ctx);
  arm64_dasm_put(ctx, "{");
  for(U32 i = 0; i < count; i += 1)
  {
    arm64_dasm_putf(ctx, "%sv%u.%s", i == 0 ? "" : ", ", (reg + i) & 31, arr);
  }
  arm64_dasm_put(ctx, "}");
}

// rjf: scalar register kind from an element size (0=B,1=H,2=S,3=D,4=Q)
internal char
arm64_dasm_kind_from_esz(U32 esz)
{
  char result = 'b';
  switch(esz)
  {
    default:
    case 0:{result = 'b';}break;
    case 1:{result = 'h';}break;
    case 2:{result = 's';}break;
    case 3:{result = 'd';}break;
    case 4:{result = 'q';}break;
  }
  return result;
}

// rjf: arrangement specifier from element size (0=B..3=D) and Q
internal char *
arm64_dasm_arr(U32 esz, U32 q)
{
  local_persist char *names[4][2] =
  {
    {"8b", "16b"},
    {"4h", "8h"},
    {"2s", "4s"},
    {"1d", "2d"},
  };
  return names[esz & 3][q & 1];
}

////////////////////////////////
//~ rjf: Memory Operand Helpers

typedef enum ARM64_DasmMemKind
{
  ARM64_DasmMemKind_Offset,  // [xn, #imm]   (imm omitted when 0)
  ARM64_DasmMemKind_Pre,     // [xn, #imm]!
  ARM64_DasmMemKind_Post,    // [xn], #imm
  ARM64_DasmMemKind_Base,    // [xn]
}
ARM64_DasmMemKind;

internal void
arm64_dasm_op_mem(ARM64_DasmCtx *ctx, U32 base, S64 imm, ARM64_DasmMemKind kind)
{
  arm64_dasm_op(ctx);
  arm64_dasm_put(ctx, "[");
  arm64_dasm_put_gpr(ctx, base, 1, 1);
  switch(kind)
  {
    default:
    case ARM64_DasmMemKind_Base:
    {
      arm64_dasm_put(ctx, "]");
    }break;
    case ARM64_DasmMemKind_Offset:
    {
      if(imm != 0)
      {
        arm64_dasm_put(ctx, ", ");
        arm64_dasm_put_imm(ctx, imm);
      }
      arm64_dasm_put(ctx, "]");
    }break;
    case ARM64_DasmMemKind_Pre:
    {
      arm64_dasm_put(ctx, ", ");
      arm64_dasm_put_imm(ctx, imm);
      arm64_dasm_put(ctx, "]!");
    }break;
    case ARM64_DasmMemKind_Post:
    {
      arm64_dasm_put(ctx, "], ");
      arm64_dasm_put_imm(ctx, imm);
    }break;
  }
}

// rjf: register-offset memory operand: [xn, xm{, ext{ #amount}}]
internal void
arm64_dasm_op_mem_reg(ARM64_DasmCtx *ctx, U32 base, U32 rm, U32 option, U32 s, U32 scale)
{
  local_persist char *ext_names[8] = {"?", "?", "uxtw", "lsl", "?", "?", "sxtw", "sxtx"};
  arm64_dasm_op(ctx);
  arm64_dasm_put(ctx, "[");
  arm64_dasm_put_gpr(ctx, base, 1, 1);
  arm64_dasm_put(ctx, ", ");
  arm64_dasm_put_gpr(ctx, rm, (option & 1) != 0, 0);
  if(option == 3)
  {
    if(s != 0) { arm64_dasm_putf(ctx, ", lsl #%u", scale); }
  }
  else
  {
    arm64_dasm_putf(ctx, ", %s", ext_names[option & 7]);
    if(s != 0) { arm64_dasm_putf(ctx, " #%u", scale); }
  }
  arm64_dasm_put(ctx, "]");
}

// rjf: record a register operand / memory base for the dst (slot 0) or src (slot 1) slot
internal void
arm64_dasm_set_reg(ARM64_DasmCtx *ctx, U32 slot, ARCH_RegCode code, S64 off)
{
  if(slot < 2)
  {
    ctx->reg_codes[slot] = code;
    ctx->reg_offs[slot] = off;
  }
}

// rjf: memory base + offset -> given slot; also marks SP writeback when requested
internal void
arm64_dasm_set_mem(ARM64_DasmCtx *ctx, U32 slot, U32 base, S64 off)
{
  arm64_dasm_set_reg(ctx, slot, arm64_dasm_gpr_code(base, 1, 1), off);
}

internal void
arm64_dasm_mark_sp_writeback(ARM64_DasmCtx *ctx, U32 base, B32 variable)
{
  if((base & 31) == 31)
  {
    ctx->flags |= DASM_InstFlag_ChangesStackPointer;
    if(variable)
    {
      ctx->flags |= DASM_InstFlag_ChangesStackPointerVariably;
    }
  }
}

////////////////////////////////
//~ rjf: Immediate Decoding Helpers

// rjf: DecodeBitMasks (logical immediates); returns 0 for reserved encodings
internal B32
arm64_dasm_decode_bitmask(U32 n, U32 imms, U32 immr, B32 is64, U64 *out)
{
  B32 result = 0;
  U32 nimms = ((n & 1) << 6) | (~imms & 0x3f);
  if(nimms != 0)
  {
    U32 len = arm64_dasm_highest_set_bit(nimms);
    if(len >= 1 && (is64 || len < 6))
    {
      U32 esize = 1u << len;
      U32 levels = esize - 1;
      U32 s = imms & levels;
      U32 r = immr & levels;
      if(s != levels)
      {
        U64 welem = (s + 1 >= 64) ? ~0ull : ((1ull << (s + 1)) - 1);
        U64 emask = (esize >= 64) ? ~0ull : ((1ull << esize) - 1);
        U64 rotated = welem;
        if(r != 0)
        {
          rotated = ((welem >> r) | (welem << (esize - r))) & emask;
        }
        U64 value = 0;
        for(U32 i = 0; i < 64; i += esize)
        {
          value |= rotated << i;
        }
        if(!is64)
        {
          value &= 0xffffffffull;
        }
        *out = value;
        result = 1;
      }
    }
  }
  return result;
}

// rjf: MoveWidePreferred (Arm ARM): whether ORR-immediate's MOV alias must not be used
internal B32
arm64_dasm_move_wide_preferred(B32 sf, U32 immn, U32 imms, U32 immr)
{
  U32 s = imms & 0x3f;
  U32 r = immr & 0x3f;
  U32 width = sf ? 64 : 32;
  B32 result = 0;
  if(sf && immn != 1)
  {
    result = 0;
  }
  else if(!sf && (immn != 0 || (imms & 0x20) != 0))
  {
    result = 0;
  }
  else if(s < 16)
  {
    result = (((0u - r) & 15) <= (15 - s));
  }
  else if(s >= width - 15)
  {
    result = ((r & 15) <= (s - (width - 15)));
  }
  return result;
}

// rjf: BFXPreferred (Arm ARM)
internal B32
arm64_dasm_bfx_preferred(B32 sf, U32 uns, U32 imms, U32 immr)
{
  B32 result = 1;
  if(imms < immr)
  {
    result = 0;
  }
  else if(imms == (sf ? 63u : 31u))
  {
    result = 0;
  }
  else if(immr == 0)
  {
    if(!sf && (imms == 7 || imms == 15))
    {
      result = 0;
    }
    if(sf && uns == 0 && (imms == 7 || imms == 15 || imms == 31))
    {
      result = 0;
    }
  }
  return result;
}

// rjf: VFPExpandImm: 8-bit FP immediate -> value
internal F64
arm64_dasm_fp_imm8(U32 imm8)
{
  U32 sign = (imm8 >> 7) & 1;
  U32 b    = (imm8 >> 6) & 1;
  U32 cd   = (imm8 >> 4) & 3;
  U32 frac = imm8 & 15;
  S32 e    = b ? ((S32)cd - 3) : ((S32)cd + 1);
  F64 v    = (1.0 + (F64)frac / 16.0);
  if(e > 0) { v *= (F64)(1u << e); }
  if(e < 0) { v /= (F64)(1u << (-e)); }
  if(sign)  { v = -v; }
  return v;
}

// rjf: AdvSIMDExpandImm for the 64-bit MOVI form: each imm8 bit -> a byte
internal U64
arm64_dasm_simd_imm64(U32 imm8)
{
  U64 result = 0;
  for(U32 i = 0; i < 8; i += 1)
  {
    if(imm8 & (1u << i))
    {
      result |= 0xffull << (i * 8);
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Name Tables

internal char *
arm64_dasm_cond_name(U32 cond)
{
  local_persist char *names[16] =
  {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
  };
  return names[cond & 15];
}

internal char *
arm64_dasm_shift_name(U32 shift)
{
  local_persist char *names[4] = {"lsl", "lsr", "asr", "ror"};
  return names[shift & 3];
}

internal char *
arm64_dasm_extend_name(U32 option)
{
  local_persist char *names[8] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
  return names[option & 7];
}

internal char *
arm64_dasm_barrier_name(U32 crm)
{
  local_persist char *names[16] =
  {
    0, "oshld", "oshst", "osh", 0, "nshld", "nshst", "nsh",
    0, "ishld", "ishst", "ish", 0, "ld", "st", "sy",
  };
  return names[crm & 15];
}

typedef struct ARM64_DasmSysReg ARM64_DasmSysReg;
struct ARM64_DasmSysReg
{
  U16 enc; // op0<<14 | op1<<11 | crn<<7 | crm<<3 | op2
  char *name;
  ARCH_RegCode reg_code;
};

#define ARM64_DASM_SYSREG(op0, op1, crn, crm, op2) ((U16)(((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2)))

read_only global ARM64_DasmSysReg arm64_dasm_sysreg_table[] =
{
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 0), "nzcv", 0},
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 1), "daif", 0},
  {ARM64_DASM_SYSREG(3, 3, 4,  4, 0), "fpcr", ARM64_RegCode_fpcr},
  {ARM64_DASM_SYSREG(3, 3, 4,  4, 1), "fpsr", ARM64_RegCode_fpsr},
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 5), "dit", 0},
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 6), "ssbs", 0},
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 7), "tco", 0},
  {ARM64_DASM_SYSREG(3, 3, 13, 0, 2), "tpidr_el0", ARM64_RegCode_tpidr_el0},
  {ARM64_DASM_SYSREG(3, 3, 13, 0, 3), "tpidrro_el0", ARM64_RegCode_tpidrro_el0},
  {ARM64_DASM_SYSREG(3, 0, 13, 0, 4), "tpidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 0, 0), "cntfrq_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 0, 1), "cntpct_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 0, 2), "cntvct_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 2, 0), "cntp_tval_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 2, 1), "cntp_ctl_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 2, 2), "cntp_cval_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 3, 0), "cntv_tval_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 3, 1), "cntv_ctl_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 14, 3, 2), "cntv_cval_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 0,  0, 1), "ctr_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 0,  0, 7), "dczid_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 9,  12, 0), "pmcr_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 9,  13, 0), "pmccntr_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 9,  14, 0), "pmuserenr_el0", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  0, 0), "midr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  0, 5), "mpidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  0, 6), "revidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  4, 0), "id_aa64pfr0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  4, 1), "id_aa64pfr1_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  5, 0), "id_aa64dfr0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  6, 0), "id_aa64isar0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  6, 1), "id_aa64isar1_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  6, 2), "id_aa64isar2_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  7, 0), "id_aa64mmfr0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  7, 1), "id_aa64mmfr1_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 0,  7, 2), "id_aa64mmfr2_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 1,  0, 0), "sctlr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 1,  0, 1), "actlr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 1,  0, 2), "cpacr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  0, 0), "ttbr0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  0, 1), "ttbr1_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  0, 2), "tcr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  0, 0), "spsr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  0, 1), "elr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  1, 0), "sp_el0", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  2, 0), "spsel", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  2, 2), "currentel", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  2, 3), "pan", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  2, 4), "uao", 0},
  {ARM64_DASM_SYSREG(3, 0, 5,  1, 0), "afsr0_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 5,  1, 1), "afsr1_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 5,  2, 0), "esr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 6,  0, 0), "far_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 10, 2, 0), "mair_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 10, 3, 0), "amair_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 12, 0, 0), "vbar_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 12, 1, 0), "isr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 13, 0, 1), "contextidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 14, 1, 0), "cntkctl_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  1, 0), "apiakeylo_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  1, 1), "apiakeyhi_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  1, 2), "apibkeylo_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  1, 3), "apibkeyhi_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  2, 0), "apdakeylo_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  2, 1), "apdakeyhi_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  2, 2), "apdbkeylo_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  2, 3), "apdbkeyhi_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  3, 0), "apgakeylo_el1", 0},
  {ARM64_DASM_SYSREG(3, 0, 2,  3, 1), "apgakeyhi_el1", 0},
  {ARM64_DASM_SYSREG(2, 0, 0,  2, 2), "mdscr_el1", 0},
  {ARM64_DASM_SYSREG(2, 0, 1,  0, 0), "mdrar_el1", 0},
  {ARM64_DASM_SYSREG(2, 0, 1,  0, 4), "oslar_el1", 0},
  {ARM64_DASM_SYSREG(2, 3, 1,  0, 0), "mdccsr_el0", 0},
  {ARM64_DASM_SYSREG(2, 3, 0,  4, 0), "dbgdtr_el0", 0},
  {ARM64_DASM_SYSREG(2, 3, 0,  5, 0), "dbgdtrrx_el0", 0},
  {ARM64_DASM_SYSREG(2, 0, 7,  14, 6), "dbgauthstatus_el1", 0},
  {ARM64_DASM_SYSREG(3, 1, 0,  0, 0), "ccsidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 1, 0,  0, 1), "clidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 1, 0,  0, 7), "aidr_el1", 0},
  {ARM64_DASM_SYSREG(3, 2, 0,  0, 0), "csselr_el1", 0},
  {ARM64_DASM_SYSREG(3, 3, 13, 0, 5), "tpidr2_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 13, 0, 7), "scxtnum_el0", 0},
  {ARM64_DASM_SYSREG(3, 3, 2,  4, 0), "rndr", 0},
  {ARM64_DASM_SYSREG(3, 3, 2,  4, 1), "rndrrs", 0},
  {ARM64_DASM_SYSREG(3, 3, 4,  2, 4), "svcr", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  3, 0), "allint", 0},
  {ARM64_DASM_SYSREG(3, 0, 4,  6, 0), "icc_pmr_el1", 0},
};

internal void
arm64_dasm_op_sysreg(ARM64_DasmCtx *ctx, U32 op0, U32 op1, U32 crn, U32 crm, U32 op2, ARCH_RegCode *code_out)
{
  U16 enc = ARM64_DASM_SYSREG(op0, op1, crn, crm, op2);
  char *name = 0;
  ARCH_RegCode code = 0;
  for EachElement(idx, arm64_dasm_sysreg_table)
  {
    if(arm64_dasm_sysreg_table[idx].enc == enc)
    {
      name = arm64_dasm_sysreg_table[idx].name;
      code = arm64_dasm_sysreg_table[idx].reg_code;
      break;
    }
  }
  if(name != 0)
  {
    arm64_dasm_op_str(ctx, name);
  }
  else
  {
    arm64_dasm_op_f(ctx, "s%u_%u_c%u_c%u_%u", op0, op1, crn, crm, op2);
  }
  if(code_out != 0)
  {
    *code_out = code;
  }
}

// rjf: prefetch operation name (PRFM/PRFUM)
internal void
arm64_dasm_op_prfop(ARM64_DasmCtx *ctx, U32 rt)
{
  local_persist char *types[4]   = {"pld", "pli", "pst", 0};
  local_persist char *targets[4] = {"l1", "l2", "l3", 0};
  U32 type   = (rt >> 3) & 3;
  U32 target = (rt >> 1) & 3;
  U32 policy = rt & 1;
  if(types[type] != 0 && targets[target] != 0)
  {
    arm64_dasm_op_f(ctx, "%s%s%s", types[type], targets[target], policy ? "strm" : "keep");
  }
  else
  {
    arm64_dasm_op_dec(ctx, rt & 31);
  }
}

////////////////////////////////
//~ rjf: Data Processing (Immediate)

internal B32
arm64_dasm_decode_dp_imm(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 op1  = ARM64_DASM_BITS(inst, 25, 23);
  U32 sf   = ARM64_DASM_BIT(inst, 31);
  U32 rd   = ARM64_DASM_BITS(inst, 4, 0);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  switch(op1)
  {
    //- rjf: PC-relative addressing
    case 0:
    case 1:
    {
      U32 op = ARM64_DASM_BIT(inst, 31);
      U64 imm = ((U64)ARM64_DASM_BITS(inst, 23, 5) << 2) | ARM64_DASM_BITS(inst, 30, 29);
      S64 off = arm64_dasm_sext(imm, 21);
      U64 target = 0;
      if(op == 0)
      {
        target = ctx->vaddr + (U64)off;
        arm64_dasm_mnem(ctx, "adr");
      }
      else
      {
        target = (ctx->vaddr & ~0xfffull) + ((U64)off << 12);
        arm64_dasm_mnem(ctx, "adrp");
      }
      arm64_dasm_op_gpr(ctx, rd, 1, 0);
      arm64_dasm_op_addr(ctx, target);
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, 1, 0), 0);
      arm64_dasm_set_reg(ctx, 1, ARM64_RegCode_pc, (S64)(target - ctx->vaddr));
      result = 1;
    }break;

    //- rjf: add/subtract (immediate)
    case 2:
    {
      U32 op    = ARM64_DASM_BIT(inst, 30);
      U32 s     = ARM64_DASM_BIT(inst, 29);
      U32 sh    = ARM64_DASM_BIT(inst, 22);
      U32 imm12 = ARM64_DASM_BITS(inst, 21, 10);
      if(op == 0 && s == 0 && sh == 0 && imm12 == 0 && (rd == 31 || rn == 31))
      {
        arm64_dasm_mnem(ctx, "mov");
        arm64_dasm_op_gpr(ctx, rd, sf, 1);
        arm64_dasm_op_gpr(ctx, rn, sf, 1);
        if(rd == 31) { ctx->flags |= DASM_InstFlag_ChangesStackPointer|DASM_InstFlag_ChangesStackPointerVariably; }
      }
      else
      {
        if(s && rd == 31)
        {
          arm64_dasm_mnem(ctx, op ? "cmp" : "cmn");
        }
        else
        {
          local_persist char *names[2][2] = {{"add", "adds"}, {"sub", "subs"}};
          arm64_dasm_mnem(ctx, names[op][s]);
          arm64_dasm_op_gpr(ctx, rd, sf, !s);
        }
        arm64_dasm_op_gpr(ctx, rn, sf, 1);
        arm64_dasm_op_imm(ctx, imm12);
        if(sh) { arm64_dasm_op_str(ctx, "lsl #12"); }
        if(rd == 31 && s == 0) { ctx->flags |= DASM_InstFlag_ChangesStackPointer; }
      }
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, !s), 0);
      arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 1), 0);
      result = 1;
    }break;

    //- rjf: add/subtract (immediate, with tags)
    case 3:
    {
      U32 op    = ARM64_DASM_BIT(inst, 30);
      U32 s     = ARM64_DASM_BIT(inst, 29);
      U32 o2    = ARM64_DASM_BIT(inst, 22);
      U32 uimm6 = ARM64_DASM_BITS(inst, 21, 16);
      U32 op3   = ARM64_DASM_BITS(inst, 15, 14);
      U32 uimm4 = ARM64_DASM_BITS(inst, 13, 10);
      if(sf == 1 && s == 0 && o2 == 0 && op3 == 0)
      {
        arm64_dasm_mnem(ctx, op ? "subg" : "addg");
        arm64_dasm_op_gpr(ctx, rd, 1, 1);
        arm64_dasm_op_gpr(ctx, rn, 1, 1);
        arm64_dasm_op_imm(ctx, uimm6 << 4);
        arm64_dasm_op_dec(ctx, uimm4);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, 1, 1), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, 1, 1), 0);
        if(rd == 31) { ctx->flags |= DASM_InstFlag_ChangesStackPointer; }
        result = 1;
      }
    }break;

    //- rjf: logical (immediate)
    case 4:
    {
      U32 opc  = ARM64_DASM_BITS(inst, 30, 29);
      U32 n    = ARM64_DASM_BIT(inst, 22);
      U32 immr = ARM64_DASM_BITS(inst, 21, 16);
      U32 imms = ARM64_DASM_BITS(inst, 15, 10);
      U64 imm  = 0;
      if((sf || n == 0) && arm64_dasm_decode_bitmask(n, imms, immr, sf, &imm))
      {
        B32 rd_sp = (opc != 3);
        if(opc == 1 && rn == 31 && !arm64_dasm_move_wide_preferred(sf, n, imms, immr))
        {
          arm64_dasm_mnem(ctx, "mov");
          arm64_dasm_op_gpr(ctx, rd, sf, 1);
          arm64_dasm_op_immu(ctx, imm);
        }
        else if(opc == 3 && rd == 31)
        {
          arm64_dasm_mnem(ctx, "tst");
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_immu(ctx, imm);
        }
        else
        {
          local_persist char *names[4] = {"and", "orr", "eor", "ands"};
          arm64_dasm_mnem(ctx, names[opc]);
          arm64_dasm_op_gpr(ctx, rd, sf, rd_sp);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_immu(ctx, imm);
        }
        if(rd == 31 && rd_sp) { ctx->flags |= DASM_InstFlag_ChangesStackPointer; }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, rd_sp), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
        result = 1;
      }
    }break;

    //- rjf: move wide (immediate)
    case 5:
    {
      U32 opc   = ARM64_DASM_BITS(inst, 30, 29);
      U32 hw    = ARM64_DASM_BITS(inst, 22, 21);
      U32 imm16 = ARM64_DASM_BITS(inst, 20, 5);
      if(opc != 1 && (sf || hw < 2))
      {
        U32 shift = hw * 16;
        switch(opc)
        {
          default:{}break;
          case 0: // MOVN
          {
            B32 alias = !(imm16 == 0 && hw != 0) && (sf || imm16 != 0xffff);
            if(alias)
            {
              U64 value = ~((U64)imm16 << shift);
              if(!sf) { value &= 0xffffffffull; }
              S64 signed_value = sf ? (S64)value : (S64)(S32)(U32)value;
              arm64_dasm_mnem(ctx, "mov");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_imm(ctx, signed_value);
            }
            else
            {
              arm64_dasm_mnem(ctx, "movn");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_imm(ctx, imm16);
              if(hw != 0) { arm64_dasm_op_f(ctx, "lsl #%u", shift); }
            }
          }break;
          case 2: // MOVZ
          {
            B32 alias = !(imm16 == 0 && hw != 0);
            if(alias)
            {
              arm64_dasm_mnem(ctx, "mov");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_immu(ctx, (U64)imm16 << shift);
            }
            else
            {
              arm64_dasm_mnem(ctx, "movz");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_imm(ctx, imm16);
              if(hw != 0) { arm64_dasm_op_f(ctx, "lsl #%u", shift); }
            }
          }break;
          case 3: // MOVK
          {
            arm64_dasm_mnem(ctx, "movk");
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            arm64_dasm_op_imm(ctx, imm16);
            if(hw != 0) { arm64_dasm_op_f(ctx, "lsl #%u", shift); }
          }break;
        }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
        result = 1;
      }
    }break;

    //- rjf: bitfield
    case 6:
    {
      U32 opc  = ARM64_DASM_BITS(inst, 30, 29);
      U32 n    = ARM64_DASM_BIT(inst, 22);
      U32 immr = ARM64_DASM_BITS(inst, 21, 16);
      U32 imms = ARM64_DASM_BITS(inst, 15, 10);
      if(opc != 3 && n == sf && (sf || ((immr|imms) & 0x20) == 0))
      {
        U32 width = sf ? 64 : 32;
        U32 lsb   = (width - immr) & (width - 1);
        B32 rn_is_w = 0;
        switch(opc)
        {
          default:{}break;
          case 0: // SBFM
          {
            if(imms == width - 1)
            {
              arm64_dasm_mnem(ctx, "asr");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
            }
            else if(imms < immr)
            {
              arm64_dasm_mnem(ctx, "sbfiz");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, lsb);
              arm64_dasm_op_dec(ctx, imms + 1);
            }
            else if(arm64_dasm_bfx_preferred(sf, 0, imms, immr))
            {
              arm64_dasm_mnem(ctx, "sbfx");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
              arm64_dasm_op_dec(ctx, imms - immr + 1);
            }
            else if(immr == 0 && (imms == 7 || imms == 15 || (sf && imms == 31)))
            {
              arm64_dasm_mnem(ctx, imms == 7 ? "sxtb" : imms == 15 ? "sxth" : "sxtw");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, 0, 0);
              rn_is_w = 1;
            }
            else
            {
              arm64_dasm_mnem(ctx, "sbfm");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
              arm64_dasm_op_dec(ctx, imms);
            }
          }break;
          case 1: // BFM
          {
            if(imms < immr)
            {
              if(rn == 31)
              {
                arm64_dasm_mnem(ctx, "bfc");
                arm64_dasm_op_gpr(ctx, rd, sf, 0);
              }
              else
              {
                arm64_dasm_mnem(ctx, "bfi");
                arm64_dasm_op_gpr(ctx, rd, sf, 0);
                arm64_dasm_op_gpr(ctx, rn, sf, 0);
              }
              arm64_dasm_op_dec(ctx, lsb);
              arm64_dasm_op_dec(ctx, imms + 1);
            }
            else
            {
              arm64_dasm_mnem(ctx, "bfxil");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
              arm64_dasm_op_dec(ctx, imms - immr + 1);
            }
          }break;
          case 2: // UBFM
          {
            if(imms != width - 1 && imms + 1 == immr)
            {
              arm64_dasm_mnem(ctx, "lsl");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, width - 1 - imms);
            }
            else if(imms == width - 1)
            {
              arm64_dasm_mnem(ctx, "lsr");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
            }
            else if(imms < immr)
            {
              arm64_dasm_mnem(ctx, "ubfiz");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, lsb);
              arm64_dasm_op_dec(ctx, imms + 1);
            }
            else if(arm64_dasm_bfx_preferred(sf, 1, imms, immr))
            {
              arm64_dasm_mnem(ctx, "ubfx");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
              arm64_dasm_op_dec(ctx, imms - immr + 1);
            }
            else if(!sf && immr == 0 && (imms == 7 || imms == 15))
            {
              arm64_dasm_mnem(ctx, imms == 7 ? "uxtb" : "uxth");
              arm64_dasm_op_gpr(ctx, rd, 0, 0);
              arm64_dasm_op_gpr(ctx, rn, 0, 0);
              rn_is_w = 1;
            }
            else
            {
              arm64_dasm_mnem(ctx, "ubfm");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_dec(ctx, immr);
              arm64_dasm_op_dec(ctx, imms);
            }
          }break;
        }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, rn_is_w ? 0 : sf, 0), 0);
        result = 1;
      }
    }break;

    //- rjf: extract
    case 7:
    {
      U32 op21 = ARM64_DASM_BITS(inst, 30, 29);
      U32 n    = ARM64_DASM_BIT(inst, 22);
      U32 o0   = ARM64_DASM_BIT(inst, 21);
      U32 rm   = ARM64_DASM_BITS(inst, 20, 16);
      U32 imms = ARM64_DASM_BITS(inst, 15, 10);
      if(op21 == 0 && o0 == 0 && n == sf && (sf || (imms & 0x20) == 0))
      {
        if(rn == rm)
        {
          arm64_dasm_mnem(ctx, "ror");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
        }
        else
        {
          arm64_dasm_mnem(ctx, "extr");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
        }
        arm64_dasm_op_dec(ctx, imms);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
        result = 1;
      }
    }break;
  }
  return result;
}

////////////////////////////////
//~ rjf: Branches, Exception Generating & System Instructions

internal B32
arm64_dasm_decode_branch_reg(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 opc = ARM64_DASM_BITS(inst, 24, 21);
  U32 op2 = ARM64_DASM_BITS(inst, 20, 16);
  U32 op3 = ARM64_DASM_BITS(inst, 15, 10);
  U32 rn  = ARM64_DASM_BITS(inst, 9, 5);
  U32 op4 = ARM64_DASM_BITS(inst, 4, 0);
  if(op2 == 31)
  {
    switch(opc)
    {
      default:{}break;

      //- rjf: BR / BRAAZ / BRABZ, BLR / BLRAAZ / BLRABZ
      case 0:
      case 1:
      {
        char *name = 0;
        if(op3 == 0 && op4 == 0)       { name = opc ? "blr"    : "br"; }
        else if(op3 == 2 && op4 == 31) { name = opc ? "blraaz" : "braaz"; }
        else if(op3 == 3 && op4 == 31) { name = opc ? "blrabz" : "brabz"; }
        if(name != 0)
        {
          arm64_dasm_mnem(ctx, name);
          arm64_dasm_op_gpr(ctx, rn, 1, 0);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, 1, 0), 0);
          ctx->flags |= (opc ? DASM_InstFlag_Call : DASM_InstFlag_UnconditionalJump);
          result = 1;
        }
      }break;

      //- rjf: RET / RETAA / RETAB
      case 2:
      {
        if(op3 == 0 && op4 == 0)
        {
          arm64_dasm_mnem(ctx, "ret");
          if(rn != 30) { arm64_dasm_op_gpr(ctx, rn, 1, 0); }
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, 1, 0), 0);
          ctx->flags |= DASM_InstFlag_Return;
          result = 1;
        }
        else if((op3 == 2 || op3 == 3) && rn == 31 && op4 == 31)
        {
          arm64_dasm_mnem(ctx, op3 == 2 ? "retaa" : "retab");
          arm64_dasm_set_reg(ctx, 0, ARM64_RegCode_lr, 0);
          ctx->flags |= DASM_InstFlag_Return;
          result = 1;
        }
      }break;

      //- rjf: ERET / ERETAA / ERETAB
      case 4:
      {
        if(rn == 31 && ((op3 == 0 && op4 == 0) || ((op3 == 2 || op3 == 3) && op4 == 31)))
        {
          arm64_dasm_mnem(ctx, op3 == 0 ? "eret" : op3 == 2 ? "eretaa" : "eretab");
          ctx->flags |= DASM_InstFlag_Return;
          result = 1;
        }
      }break;

      //- rjf: DRPS
      case 5:
      {
        if(rn == 31 && op3 == 0 && op4 == 0)
        {
          arm64_dasm_mnem(ctx, "drps");
          result = 1;
        }
      }break;

      //- rjf: BRAA / BRAB / BLRAA / BLRAB
      case 8:
      case 9:
      {
        if(op3 == 2 || op3 == 3)
        {
          local_persist char *names[2][2] = {{"braa", "brab"}, {"blraa", "blrab"}};
          arm64_dasm_mnem(ctx, names[opc - 8][op3 - 2]);
          arm64_dasm_op_gpr(ctx, rn, 1, 0);
          arm64_dasm_op_gpr(ctx, op4, 1, 1);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, 1, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(op4, 1, 1), 0);
          ctx->flags |= (opc == 9 ? DASM_InstFlag_Call : DASM_InstFlag_UnconditionalJump);
          result = 1;
        }
      }break;
    }
  }
  return result;
}

internal B32
arm64_dasm_decode_exception(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 opc   = ARM64_DASM_BITS(inst, 23, 21);
  U32 imm16 = ARM64_DASM_BITS(inst, 20, 5);
  U32 op2   = ARM64_DASM_BITS(inst, 4, 2);
  U32 ll    = ARM64_DASM_BITS(inst, 1, 0);
  if(op2 == 0)
  {
    char *name = 0;
    switch(opc)
    {
      default:{}break;
      case 0:{name = (ll == 1 ? "svc" : ll == 2 ? "hvc" : ll == 3 ? "smc" : 0);}break;
      case 1:{name = (ll == 0 ? "brk" : 0);}break;
      case 2:{name = (ll == 0 ? "hlt" : 0);}break;
      case 3:{name = (ll == 0 ? "tcancel" : 0);}break;
      case 5:{name = (ll == 1 ? "dcps1" : ll == 2 ? "dcps2" : ll == 3 ? "dcps3" : 0);}break;
    }
    if(name != 0)
    {
      arm64_dasm_mnem(ctx, name);
      if(opc != 5 || imm16 != 0)
      {
        arm64_dasm_op_immu(ctx, imm16);
      }
      result = 1;
    }
  }
  return result;
}

internal B32
arm64_dasm_decode_system(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 l    = ARM64_DASM_BIT(inst, 21);
  U32 op0  = ARM64_DASM_BITS(inst, 20, 19);
  U32 op1  = ARM64_DASM_BITS(inst, 18, 16);
  U32 crn  = ARM64_DASM_BITS(inst, 15, 12);
  U32 crm  = ARM64_DASM_BITS(inst, 11, 8);
  U32 op2  = ARM64_DASM_BITS(inst, 7, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  if(ARM64_DASM_BITS(inst, 23, 22) == 0)
  {
    if(op0 == 0 && l == 0)
    {
      //- rjf: hints
      if(op1 == 3 && crn == 2 && rt == 31)
      {
        char *name = 0;
        switch((crm << 3) | op2)
        {
          default:{}break;
          case 0x00:{name = "nop";}break;
          case 0x01:{name = "yield";}break;
          case 0x02:{name = "wfe";}break;
          case 0x03:{name = "wfi";}break;
          case 0x04:{name = "sev";}break;
          case 0x05:{name = "sevl";}break;
          case 0x06:{name = "dgh";}break;
          case 0x07:{name = "xpaclri";}break;
          case 0x08:{name = "pacia1716";}break;
          case 0x0a:{name = "pacib1716";}break;
          case 0x0c:{name = "autia1716";}break;
          case 0x0e:{name = "autib1716";}break;
          case 0x10:{name = "esb";}break;
          case 0x11:{name = "psb csync";}break;
          case 0x12:{name = "tsb csync";}break;
          case 0x14:{name = "csdb";}break;
          case 0x16:{name = "clrbhb";}break;
          case 0x18:{name = "paciaz";}break;
          case 0x19:{name = "paciasp";}break;
          case 0x1a:{name = "pacibz";}break;
          case 0x1b:{name = "pacibsp";}break;
          case 0x1c:{name = "autiaz";}break;
          case 0x1d:{name = "autiasp";}break;
          case 0x1e:{name = "autibz";}break;
          case 0x1f:{name = "autibsp";}break;
          case 0x20:{name = "bti";}break;
          case 0x22:{name = "bti c";}break;
          case 0x24:{name = "bti j";}break;
          case 0x26:{name = "bti jc";}break;
        }
        if(name != 0)
        {
          arm64_dasm_mnem(ctx, name);
        }
        else
        {
          arm64_dasm_mnem(ctx, "hint");
          arm64_dasm_op_dec(ctx, (crm << 3) | op2);
        }
        result = 1;
      }

      //- rjf: barriers
      else if(op1 == 3 && crn == 3 && rt == 31)
      {
        switch(op2)
        {
          default:{}break;
          case 2:
          {
            arm64_dasm_mnem(ctx, "clrex");
            if(crm != 15) { arm64_dasm_op_dec(ctx, crm); }
            result = 1;
          }break;
          case 3:
          {
            if(crm == 0) { arm64_dasm_mnem(ctx, "tcommit"); result = 1; }
          }break;
          case 4:
          case 5:
          {
            if(op2 == 4 && crm == 0)      { arm64_dasm_mnem(ctx, "ssbb"); }
            else if(op2 == 4 && crm == 4) { arm64_dasm_mnem(ctx, "pssbb"); }
            else
            {
              arm64_dasm_mnem(ctx, op2 == 4 ? "dsb" : "dmb");
              char *opt = arm64_dasm_barrier_name(crm);
              if(opt != 0) { arm64_dasm_op_str(ctx, opt); }
              else         { arm64_dasm_op_dec(ctx, crm); }
            }
            result = 1;
          }break;
          case 6:
          {
            arm64_dasm_mnem(ctx, "isb");
            if(crm != 15) { arm64_dasm_op_dec(ctx, crm); }
            result = 1;
          }break;
          case 7:
          {
            if(crm == 0) { arm64_dasm_mnem(ctx, "sb"); result = 1; }
          }break;
        }
      }

      //- rjf: PSTATE
      else if(crn == 4 && rt == 31)
      {
        if(op1 == 0 && op2 == 0 && crm == 0)      { arm64_dasm_mnem(ctx, "cfinv"); result = 1; }
        else if(op1 == 0 && op2 == 1 && crm == 0) { arm64_dasm_mnem(ctx, "xaflag"); result = 1; }
        else if(op1 == 0 && op2 == 2 && crm == 0) { arm64_dasm_mnem(ctx, "axflag"); result = 1; }
        else
        {
          char *field = 0;
          switch((op1 << 3) | op2)
          {
            default:{}break;
            case 0x03:{field = "uao";}break;
            case 0x04:{field = "pan";}break;
            case 0x05:{field = "spsel";}break;
            case 0x08:{field = "allint";}break;
            case 0x19:{field = "ssbs";}break;
            case 0x1a:{field = "dit";}break;
            case 0x1c:{field = "tco";}break;
            case 0x1e:{field = "daifset";}break;
            case 0x1f:{field = "daifclr";}break;
          }
          if(field != 0)
          {
            arm64_dasm_mnem(ctx, "msr");
            arm64_dasm_op_str(ctx, field);
            arm64_dasm_op_dec(ctx, crm);
            result = 1;
          }
        }
      }

      //- rjf: WFET / WFIT
      else if(op1 == 3 && crn == 1 && crm == 0 && (op2 == 0 || op2 == 1))
      {
        arm64_dasm_mnem(ctx, op2 == 0 ? "wfet" : "wfit");
        arm64_dasm_op_gpr(ctx, rt, 1, 0);
        result = 1;
      }
    }

    //- rjf: system with result (TSTART / TTEST)
    else if(op0 == 0 && l == 1)
    {
      if(op1 == 3 && crn == 3 && crm == 0 && (op2 == 0 || op2 == 1))
      {
        arm64_dasm_mnem(ctx, op2 == 0 ? "tstart" : "ttest");
        arm64_dasm_op_gpr(ctx, rt, 1, 0);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, 1, 0), 0);
        result = 1;
      }
    }

    //- rjf: SYS / SYSL (with IC / DC aliases)
    else if(op0 == 1)
    {
      if(l == 0)
      {
        char *alias = 0;
        char *alias_op = 0;
        if(crn == 7)
        {
          switch((op1 << 7) | (crm << 3) | op2)
          {
            default:{}break;
            case (0 << 7) | (1  << 3) | 0:{alias = "ic"; alias_op = "ialluis";}break;
            case (0 << 7) | (5  << 3) | 0:{alias = "ic"; alias_op = "iallu";}break;
            case (3 << 7) | (5  << 3) | 1:{alias = "ic"; alias_op = "ivau";}break;
            case (3 << 7) | (4  << 3) | 1:{alias = "dc"; alias_op = "zva";}break;
            case (0 << 7) | (6  << 3) | 1:{alias = "dc"; alias_op = "ivac";}break;
            case (0 << 7) | (6  << 3) | 2:{alias = "dc"; alias_op = "isw";}break;
            case (0 << 7) | (10 << 3) | 2:{alias = "dc"; alias_op = "csw";}break;
            case (0 << 7) | (14 << 3) | 2:{alias = "dc"; alias_op = "cisw";}break;
            case (3 << 7) | (10 << 3) | 1:{alias = "dc"; alias_op = "cvac";}break;
            case (3 << 7) | (11 << 3) | 1:{alias = "dc"; alias_op = "cvau";}break;
            case (3 << 7) | (12 << 3) | 1:{alias = "dc"; alias_op = "cvap";}break;
            case (3 << 7) | (13 << 3) | 1:{alias = "dc"; alias_op = "cvadp";}break;
            case (3 << 7) | (14 << 3) | 1:{alias = "dc"; alias_op = "civac";}break;
          }
        }
        if(alias != 0)
        {
          arm64_dasm_mnem(ctx, alias);
          arm64_dasm_op_str(ctx, alias_op);
          if(rt != 31) { arm64_dasm_op_gpr(ctx, rt, 1, 0); }
        }
        else
        {
          arm64_dasm_mnem(ctx, "sys");
          arm64_dasm_op_dec(ctx, op1);
          arm64_dasm_op_f(ctx, "c%u", crn);
          arm64_dasm_op_f(ctx, "c%u", crm);
          arm64_dasm_op_dec(ctx, op2);
          if(rt != 31) { arm64_dasm_op_gpr(ctx, rt, 1, 0); }
        }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, 1, 0), 0);
      }
      else
      {
        arm64_dasm_mnem(ctx, "sysl");
        arm64_dasm_op_gpr(ctx, rt, 1, 0);
        arm64_dasm_op_dec(ctx, op1);
        arm64_dasm_op_f(ctx, "c%u", crn);
        arm64_dasm_op_f(ctx, "c%u", crm);
        arm64_dasm_op_dec(ctx, op2);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, 1, 0), 0);
      }
      result = 1;
    }

    //- rjf: MRS / MSR (register)
    else
    {
      U32 sys_op0 = 2 + (op0 & 1);
      ARCH_RegCode sys_code = 0;
      if(l)
      {
        arm64_dasm_mnem(ctx, "mrs");
        arm64_dasm_op_gpr(ctx, rt, 1, 0);
        arm64_dasm_op_sysreg(ctx, sys_op0, op1, crn, crm, op2, &sys_code);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, 1, 0), 0);
        arm64_dasm_set_reg(ctx, 1, sys_code, 0);
      }
      else
      {
        arm64_dasm_mnem(ctx, "msr");
        arm64_dasm_op_sysreg(ctx, sys_op0, op1, crn, crm, op2, &sys_code);
        arm64_dasm_op_gpr(ctx, rt, 1, 0);
        arm64_dasm_set_reg(ctx, 0, sys_code, 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rt, 1, 0), 0);
      }
      result = 1;
    }
  }
  return result;
}

internal B32
arm64_dasm_decode_branch_sys(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 op0 = ARM64_DASM_BITS(inst, 31, 29);
  switch(op0)
  {
    //- rjf: conditional branch (immediate)
    case 2:
    {
      if(ARM64_DASM_BIT(inst, 25) == 0 && ARM64_DASM_BIT(inst, 24) == 0)
      {
        U32 o0   = ARM64_DASM_BIT(inst, 4);
        U32 cond = ARM64_DASM_BITS(inst, 3, 0);
        S64 off  = arm64_dasm_sext(ARM64_DASM_BITS(inst, 23, 5), 19) * 4;
        U64 target = ctx->vaddr + (U64)off;
        arm64_dasm_mnemf(ctx, "%s.%s", o0 ? "bc" : "b", arm64_dasm_cond_name(cond));
        arm64_dasm_op_addr(ctx, target);
        ctx->dst_vaddr = target;
        ctx->flags |= DASM_InstFlag_Branch;
        result = 1;
      }
    }break;

    //- rjf: exception generation / system / unconditional branch (register)
    case 6:
    {
      if(ARM64_DASM_BIT(inst, 25) == 1)
      {
        result = arm64_dasm_decode_branch_reg(ctx);
      }
      else if(ARM64_DASM_BIT(inst, 24) == 0)
      {
        result = arm64_dasm_decode_exception(ctx);
      }
      else
      {
        result = arm64_dasm_decode_system(ctx);
      }
    }break;

    //- rjf: unconditional branch (immediate)
    case 0:
    case 4:
    {
      U32 op = ARM64_DASM_BIT(inst, 31);
      S64 off = arm64_dasm_sext(ARM64_DASM_BITS(inst, 25, 0), 26) * 4;
      U64 target = ctx->vaddr + (U64)off;
      arm64_dasm_mnem(ctx, op ? "bl" : "b");
      arm64_dasm_op_addr(ctx, target);
      ctx->dst_vaddr = target;
      ctx->flags |= (op ? DASM_InstFlag_Call : DASM_InstFlag_UnconditionalJump);
      result = 1;
    }break;

    //- rjf: compare & branch / test & branch
    case 1:
    case 5:
    {
      U32 rt = ARM64_DASM_BITS(inst, 4, 0);
      if(ARM64_DASM_BIT(inst, 25) == 0)
      {
        U32 sf = ARM64_DASM_BIT(inst, 31);
        U32 op = ARM64_DASM_BIT(inst, 24);
        S64 off = arm64_dasm_sext(ARM64_DASM_BITS(inst, 23, 5), 19) * 4;
        U64 target = ctx->vaddr + (U64)off;
        arm64_dasm_mnem(ctx, op ? "cbnz" : "cbz");
        arm64_dasm_op_gpr(ctx, rt, sf, 0);
        arm64_dasm_op_addr(ctx, target);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, sf, 0), 0);
        ctx->dst_vaddr = target;
        ctx->flags |= DASM_InstFlag_Branch;
      }
      else
      {
        U32 b5  = ARM64_DASM_BIT(inst, 31);
        U32 op  = ARM64_DASM_BIT(inst, 24);
        U32 b40 = ARM64_DASM_BITS(inst, 23, 19);
        U32 bit = (b5 << 5) | b40;
        S64 off = arm64_dasm_sext(ARM64_DASM_BITS(inst, 18, 5), 14) * 4;
        U64 target = ctx->vaddr + (U64)off;
        arm64_dasm_mnem(ctx, op ? "tbnz" : "tbz");
        arm64_dasm_op_gpr(ctx, rt, b5, 0);
        arm64_dasm_op_dec(ctx, bit);
        arm64_dasm_op_addr(ctx, target);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, b5, 0), 0);
        ctx->dst_vaddr = target;
        ctx->flags |= DASM_InstFlag_Branch;
      }
      result = 1;
    }break;

    default:{}break;
  }
  return result;
}

////////////////////////////////
//~ rjf: Loads & Stores

typedef struct ARM64_DasmLdStInfo ARM64_DasmLdStInfo;
struct ARM64_DasmLdStInfo
{
  char *prefix;  // "st" / "ld"
  char *suffix;  // "", "b", "sb", "h", "sh", "sw"
  char kind;     // 'w','x','b','h','s','d','q', or 'p' (prefetch operation)
  U32 scale;     // log2(access size in bytes)
  B32 is_load;
};

// rjf: decodes the (size, V, opc) triple shared by the register load/store
// classes; returns 0 for unallocated combinations
internal B32
arm64_dasm_ldst_info(U32 size, U32 v, U32 opc, ARM64_DasmLdStInfo *out)
{
  B32 result = 1;
  ARM64_DasmLdStInfo info = {0};
  info.prefix = (opc == 0) ? "st" : "ld";
  info.suffix = "";
  info.is_load = (opc != 0);
  if(v == 0)
  {
    info.scale = size;
    switch(size)
    {
      default:{}break;
      case 0:
      {
        info.suffix = (opc >= 2) ? "sb" : "b";
        info.kind = (opc == 2) ? 'x' : 'w';
      }break;
      case 1:
      {
        info.suffix = (opc >= 2) ? "sh" : "h";
        info.kind = (opc == 2) ? 'x' : 'w';
      }break;
      case 2:
      {
        if(opc == 3) { result = 0; }
        info.suffix = (opc == 2) ? "sw" : "";
        info.kind = (opc == 2) ? 'x' : 'w';
      }break;
      case 3:
      {
        if(opc == 3) { result = 0; }
        info.kind = (opc == 2) ? 'p' : 'x';
      }break;
    }
  }
  else
  {
    info.is_load = (opc & 1);
    info.prefix = (opc & 1) ? "ld" : "st";
    if(size == 0 && opc >= 2)
    {
      info.kind = 'q';
      info.scale = 4;
    }
    else if(opc >= 2)
    {
      result = 0;
    }
    else
    {
      info.kind = arm64_dasm_kind_from_esz(size);
      info.scale = size;
    }
  }
  if(out != 0) { *out = info; }
  return result;
}

internal void
arm64_dasm_ldst_mnem(ARM64_DasmCtx *ctx, ARM64_DasmLdStInfo *info, char *infix)
{
  if(info->kind == 'p')
  {
    arm64_dasm_mnem(ctx, (infix[0] == 'u') ? "prfum" : "prfm");
  }
  else
  {
    arm64_dasm_mnemf(ctx, "%s%s%s", info->prefix, infix, info->suffix);
  }
}

internal void
arm64_dasm_op_ldst_rt(ARM64_DasmCtx *ctx, ARM64_DasmLdStInfo *info, U32 rt)
{
  switch(info->kind)
  {
    case 'p':{arm64_dasm_op_prfop(ctx, rt);}break;
    case 'w':{arm64_dasm_op_gpr(ctx, rt, 0, 0);}break;
    case 'x':{arm64_dasm_op_gpr(ctx, rt, 1, 0);}break;
    default: {arm64_dasm_op_sreg(ctx, rt, info->kind);}break;
  }
}

internal ARCH_RegCode
arm64_dasm_ldst_rt_code(ARM64_DasmLdStInfo *info, U32 rt)
{
  ARCH_RegCode result = 0;
  switch(info->kind)
  {
    case 'p':{result = 0;}break;
    case 'w':{result = arm64_dasm_gpr_code(rt, 0, 0);}break;
    case 'x':{result = arm64_dasm_gpr_code(rt, 1, 0);}break;
    default: {result = arm64_dasm_vreg_code(rt, info->kind);}break;
  }
  return result;
}

//- rjf: load register (literal)

internal B32
arm64_dasm_decode_ldst_literal(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 opc = ARM64_DASM_BITS(inst, 31, 30);
  U32 v   = ARM64_DASM_BIT(inst, 26);
  U32 rt  = ARM64_DASM_BITS(inst, 4, 0);
  S64 off = arm64_dasm_sext(ARM64_DASM_BITS(inst, 23, 5), 19) * 4;
  U64 target = ctx->vaddr + (U64)off;
  char kind = 0;
  char *mnem = "ldr";
  if(v == 0)
  {
    switch(opc)
    {
      case 0:{kind = 'w';}break;
      case 1:{kind = 'x';}break;
      case 2:{kind = 'x'; mnem = "ldrsw";}break;
      case 3:{kind = 'p'; mnem = "prfm";}break;
    }
  }
  else
  {
    switch(opc)
    {
      case 0:{kind = 's';}break;
      case 1:{kind = 'd';}break;
      case 2:{kind = 'q';}break;
      default:{}break;
    }
  }
  if(kind != 0)
  {
    ARM64_DasmLdStInfo info = {0};
    info.kind = kind;
    arm64_dasm_mnem(ctx, mnem);
    arm64_dasm_op_ldst_rt(ctx, &info, rt);
    arm64_dasm_op_addr(ctx, target);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_ldst_rt_code(&info, rt), 0);
    arm64_dasm_set_reg(ctx, 1, ARM64_RegCode_pc, off);
    result = 1;
  }
  return result;
}

//- rjf: load/store register pair

internal B32
arm64_dasm_decode_ldst_pair(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 opc  = ARM64_DASM_BITS(inst, 31, 30);
  U32 v    = ARM64_DASM_BIT(inst, 26);
  U32 mode = ARM64_DASM_BITS(inst, 24, 23); // 0: no-allocate, 1: post-index, 2: signed offset, 3: pre-index
  U32 l    = ARM64_DASM_BIT(inst, 22);
  S64 imm7 = arm64_dasm_sext(ARM64_DASM_BITS(inst, 21, 15), 7);
  U32 rt2  = ARM64_DASM_BITS(inst, 14, 10);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  char kind = 0;
  U32 scale = 0;
  char *mnem = 0;
  if(v == 0)
  {
    switch(opc)
    {
      case 0:{kind = 'w'; scale = 2;}break;
      case 1:{if(l && mode != 0) { kind = 'x'; scale = 2; mnem = "ldpsw"; }}break;
      case 2:{kind = 'x'; scale = 3;}break;
      default:{}break;
    }
  }
  else
  {
    switch(opc)
    {
      case 0:{kind = 's'; scale = 2;}break;
      case 1:{kind = 'd'; scale = 3;}break;
      case 2:{kind = 'q'; scale = 4;}break;
      default:{}break;
    }
  }
  if(kind != 0)
  {
    if(mnem == 0)
    {
      mnem = (mode == 0) ? (l ? "ldnp" : "stnp") : (l ? "ldp" : "stp");
    }
    S64 off = imm7 * (S64)(1u << scale);
    ARM64_DasmMemKind mem_kind = ARM64_DasmMemKind_Offset;
    if(mode == 1) { mem_kind = ARM64_DasmMemKind_Post; }
    if(mode == 3) { mem_kind = ARM64_DasmMemKind_Pre; }
    ARM64_DasmLdStInfo info = {0};
    info.kind = kind;
    arm64_dasm_mnem(ctx, mnem);
    arm64_dasm_op_ldst_rt(ctx, &info, rt);
    arm64_dasm_op_ldst_rt(ctx, &info, rt2);
    arm64_dasm_op_mem(ctx, rn, off, mem_kind);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_ldst_rt_code(&info, rt), 0);
    arm64_dasm_set_mem(ctx, 1, rn, (mem_kind == ARM64_DasmMemKind_Post) ? 0 : off);
    if(mode == 1 || mode == 3)
    {
      arm64_dasm_mark_sp_writeback(ctx, rn, 0);
    }
    result = 1;
  }
  return result;
}

//- rjf: load/store register (unscaled imm / post-indexed / unprivileged / pre-indexed)

internal B32
arm64_dasm_decode_ldst_imm9(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size = ARM64_DASM_BITS(inst, 31, 30);
  U32 v    = ARM64_DASM_BIT(inst, 26);
  U32 opc  = ARM64_DASM_BITS(inst, 23, 22);
  S64 imm9 = arm64_dasm_sext(ARM64_DASM_BITS(inst, 20, 12), 9);
  U32 form = ARM64_DASM_BITS(inst, 11, 10); // 0: unscaled, 1: post-index, 2: unprivileged, 3: pre-index
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  ARM64_DasmLdStInfo info = {0};
  if(arm64_dasm_ldst_info(size, v, opc, &info) &&
     (form == 0 || info.kind != 'p') &&
     (form != 2 || v == 0))
  {
    char *infix = "r";
    ARM64_DasmMemKind mem_kind = ARM64_DasmMemKind_Offset;
    switch(form)
    {
      default:{}break;
      case 0:{infix = "ur";}break;
      case 1:{mem_kind = ARM64_DasmMemKind_Post;}break;
      case 2:{infix = "tr";}break;
      case 3:{mem_kind = ARM64_DasmMemKind_Pre;}break;
    }
    arm64_dasm_ldst_mnem(ctx, &info, infix);
    arm64_dasm_op_ldst_rt(ctx, &info, rt);
    arm64_dasm_op_mem(ctx, rn, imm9, mem_kind);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_ldst_rt_code(&info, rt), 0);
    arm64_dasm_set_mem(ctx, 1, rn, (mem_kind == ARM64_DasmMemKind_Post) ? 0 : imm9);
    if(form == 1 || form == 3)
    {
      arm64_dasm_mark_sp_writeback(ctx, rn, 0);
    }
    result = 1;
  }
  return result;
}

//- rjf: load/store register (unsigned immediate)

internal B32
arm64_dasm_decode_ldst_unsigned_imm(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size  = ARM64_DASM_BITS(inst, 31, 30);
  U32 v     = ARM64_DASM_BIT(inst, 26);
  U32 opc   = ARM64_DASM_BITS(inst, 23, 22);
  U32 imm12 = ARM64_DASM_BITS(inst, 21, 10);
  U32 rn    = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt    = ARM64_DASM_BITS(inst, 4, 0);
  ARM64_DasmLdStInfo info = {0};
  if(arm64_dasm_ldst_info(size, v, opc, &info))
  {
    S64 off = (S64)imm12 << info.scale;
    arm64_dasm_ldst_mnem(ctx, &info, "r");
    arm64_dasm_op_ldst_rt(ctx, &info, rt);
    arm64_dasm_op_mem(ctx, rn, off, ARM64_DasmMemKind_Offset);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_ldst_rt_code(&info, rt), 0);
    arm64_dasm_set_mem(ctx, 1, rn, off);
    result = 1;
  }
  return result;
}

//- rjf: load/store register (register offset)

internal B32
arm64_dasm_decode_ldst_reg_offset(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size   = ARM64_DASM_BITS(inst, 31, 30);
  U32 v      = ARM64_DASM_BIT(inst, 26);
  U32 opc    = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 option = ARM64_DASM_BITS(inst, 15, 13);
  U32 s      = ARM64_DASM_BIT(inst, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt     = ARM64_DASM_BITS(inst, 4, 0);
  ARM64_DasmLdStInfo info = {0};
  if((option & 2) != 0 && arm64_dasm_ldst_info(size, v, opc, &info))
  {
    arm64_dasm_ldst_mnem(ctx, &info, "r");
    arm64_dasm_op_ldst_rt(ctx, &info, rt);
    arm64_dasm_op_mem_reg(ctx, rn, rm, option, s, info.scale);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_ldst_rt_code(&info, rt), 0);
    arm64_dasm_set_mem(ctx, 1, rn, 0);
    result = 1;
  }
  return result;
}

//- rjf: load/store exclusive, ordered, compare-and-swap

internal B32
arm64_dasm_decode_ldst_exclusive(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size = ARM64_DASM_BITS(inst, 31, 30);
  U32 o2   = ARM64_DASM_BIT(inst, 23);
  U32 l    = ARM64_DASM_BIT(inst, 22);
  U32 o1   = ARM64_DASM_BIT(inst, 21);
  U32 rs   = ARM64_DASM_BITS(inst, 20, 16);
  U32 o0   = ARM64_DASM_BIT(inst, 15);
  U32 rt2  = ARM64_DASM_BITS(inst, 14, 10);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  char *sz = (size == 0) ? "b" : (size == 1) ? "h" : "";
  B32 is64 = (size == 3);
  U32 first_reg = rt;
  B32 first_is64 = is64;
  if(o2 == 0 && o1 == 0)
  {
    // exclusive register
    arm64_dasm_mnemf(ctx, "%s%s", l ? (o0 ? "ldaxr" : "ldxr") : (o0 ? "stlxr" : "stxr"), sz);
    if(!l)
    {
      arm64_dasm_op_gpr(ctx, rs, 0, 0);
      first_reg = rs;
      first_is64 = 0;
    }
    arm64_dasm_op_gpr(ctx, rt, is64, 0);
    arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
    result = 1;
  }
  else if(o2 == 0 && o1 == 1)
  {
    if(size & 2)
    {
      // exclusive pair
      arm64_dasm_mnem(ctx, l ? (o0 ? "ldaxp" : "ldxp") : (o0 ? "stlxp" : "stxp"));
      if(!l)
      {
        arm64_dasm_op_gpr(ctx, rs, 0, 0);
        first_reg = rs;
        first_is64 = 0;
      }
      arm64_dasm_op_gpr(ctx, rt, is64, 0);
      arm64_dasm_op_gpr(ctx, rt2, is64, 0);
      arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      result = 1;
    }
    else if(rt2 == 31)
    {
      // compare and swap pair
      is64 = (size & 1);
      arm64_dasm_mnemf(ctx, "casp%s%s", l ? "a" : "", o0 ? "l" : "");
      arm64_dasm_op_gpr(ctx, rs, is64, 0);
      arm64_dasm_op_gpr(ctx, (rs + 1) & 31, is64, 0);
      arm64_dasm_op_gpr(ctx, rt, is64, 0);
      arm64_dasm_op_gpr(ctx, (rt + 1) & 31, is64, 0);
      arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      first_reg = rs;
      first_is64 = is64;
      result = 1;
    }
  }
  else if(o2 == 1 && o1 == 0)
  {
    // load-acquire / store-release (and LORegion variants)
    arm64_dasm_mnemf(ctx, "%s%s", l ? (o0 ? "ldar" : "ldlar") : (o0 ? "stlr" : "stllr"), sz);
    arm64_dasm_op_gpr(ctx, rt, is64, 0);
    arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
    result = 1;
  }
  else if(rt2 == 31)
  {
    // compare and swap
    arm64_dasm_mnemf(ctx, "cas%s%s%s", l ? "a" : "", o0 ? "l" : "", sz);
    arm64_dasm_op_gpr(ctx, rs, is64, 0);
    arm64_dasm_op_gpr(ctx, rt, is64, 0);
    arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
    first_reg = rs;
    result = 1;
  }
  if(result)
  {
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(first_reg, first_is64, 0), 0);
    arm64_dasm_set_mem(ctx, 1, rn, 0);
  }
  return result;
}

//- rjf: LDAPUR / STLUR (unscaled immediate)

internal B32
arm64_dasm_decode_ldst_ldapur(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size = ARM64_DASM_BITS(inst, 31, 30);
  U32 opc  = ARM64_DASM_BITS(inst, 23, 22);
  S64 imm9 = arm64_dasm_sext(ARM64_DASM_BITS(inst, 20, 12), 9);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  char *name = 0;
  B32 is64 = 0;
  switch(size)
  {
    case 0:
    {
      local_persist char *names[4] = {"stlurb", "ldapurb", "ldapursb", "ldapursb"};
      name = names[opc];
      is64 = (opc == 2);
    }break;
    case 1:
    {
      local_persist char *names[4] = {"stlurh", "ldapurh", "ldapursh", "ldapursh"};
      name = names[opc];
      is64 = (opc == 2);
    }break;
    case 2:
    {
      local_persist char *names[4] = {"stlur", "ldapur", "ldapursw", 0};
      name = names[opc];
      is64 = (opc == 2);
    }break;
    case 3:
    {
      local_persist char *names[4] = {"stlur", "ldapur", 0, 0};
      name = names[opc];
      is64 = 1;
    }break;
  }
  if(name != 0)
  {
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_gpr(ctx, rt, is64, 0);
    arm64_dasm_op_mem(ctx, rn, imm9, ARM64_DasmMemKind_Offset);
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, is64, 0), 0);
    arm64_dasm_set_mem(ctx, 1, rn, imm9);
    result = 1;
  }
  return result;
}

//- rjf: atomic memory operations (LSE)

internal B32
arm64_dasm_decode_ldst_atomic(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 size = ARM64_DASM_BITS(inst, 31, 30);
  U32 v    = ARM64_DASM_BIT(inst, 26);
  U32 a    = ARM64_DASM_BIT(inst, 23);
  U32 r    = ARM64_DASM_BIT(inst, 22);
  U32 rs   = ARM64_DASM_BITS(inst, 20, 16);
  U32 o3   = ARM64_DASM_BIT(inst, 15);
  U32 opc  = ARM64_DASM_BITS(inst, 14, 12);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt   = ARM64_DASM_BITS(inst, 4, 0);
  if(v == 0)
  {
    char *sz = (size == 0) ? "b" : (size == 1) ? "h" : "";
    B32 is64 = (size == 3);
    if(o3 == 0)
    {
      local_persist char *ops[8] = {"add", "clr", "eor", "set", "smax", "smin", "umax", "umin"};
      if(rt == 31 && a == 0)
      {
        arm64_dasm_mnemf(ctx, "st%s%s%s", ops[opc], r ? "l" : "", sz);
        arm64_dasm_op_gpr(ctx, rs, is64, 0);
        arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      }
      else
      {
        arm64_dasm_mnemf(ctx, "ld%s%s%s%s", ops[opc], a ? "a" : "", r ? "l" : "", sz);
        arm64_dasm_op_gpr(ctx, rs, is64, 0);
        arm64_dasm_op_gpr(ctx, rt, is64, 0);
        arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      }
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rs, is64, 0), 0);
      arm64_dasm_set_mem(ctx, 1, rn, 0);
      result = 1;
    }
    else if(opc == 0)
    {
      arm64_dasm_mnemf(ctx, "swp%s%s%s", a ? "a" : "", r ? "l" : "", sz);
      arm64_dasm_op_gpr(ctx, rs, is64, 0);
      arm64_dasm_op_gpr(ctx, rt, is64, 0);
      arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rs, is64, 0), 0);
      arm64_dasm_set_mem(ctx, 1, rn, 0);
      result = 1;
    }
    else if(opc == 4 && a == 1 && r == 0 && rs == 31)
    {
      arm64_dasm_mnemf(ctx, "ldapr%s", sz);
      arm64_dasm_op_gpr(ctx, rt, is64, 0);
      arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rt, is64, 0), 0);
      arm64_dasm_set_mem(ctx, 1, rn, 0);
      result = 1;
    }
  }
  return result;
}

//- rjf: advanced SIMD load/store multiple structures

internal B32
arm64_dasm_decode_simd_ldst_multiple(ARM64_DasmCtx *ctx, B32 post)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 l      = ARM64_DASM_BIT(inst, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 12);
  U32 size   = ARM64_DASM_BITS(inst, 11, 10);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt     = ARM64_DASM_BITS(inst, 4, 0);
  U32 nregs = 0;
  char *n = 0;
  switch(opcode)
  {
    default:{}break;
    case 0x0:{nregs = 4; n = "4";}break;
    case 0x2:{nregs = 4; n = "1";}break;
    case 0x4:{nregs = 3; n = "3";}break;
    case 0x6:{nregs = 3; n = "1";}break;
    case 0x7:{nregs = 1; n = "1";}break;
    case 0x8:{nregs = 2; n = "2";}break;
    case 0xa:{nregs = 2; n = "1";}break;
  }
  if(nregs != 0 && (post || rm == 0) && (size != 3 || q || n[0] == '1'))
  {
    arm64_dasm_mnemf(ctx, "%s%s", l ? "ld" : "st", n);
    arm64_dasm_op_vlist(ctx, rt, nregs, arm64_dasm_arr(size, q));
    arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
    if(post)
    {
      if(rm == 31) { arm64_dasm_op_imm(ctx, nregs * (q ? 16 : 8)); }
      else         { arm64_dasm_op_gpr(ctx, rm, 1, 0); }
      arm64_dasm_mark_sp_writeback(ctx, rn, rm != 31);
    }
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rt, 'v'), 0);
    arm64_dasm_set_mem(ctx, 1, rn, 0);
    result = 1;
  }
  return result;
}

//- rjf: advanced SIMD load/store single structure (and replicate)

internal B32
arm64_dasm_decode_simd_ldst_single(ARM64_DasmCtx *ctx, B32 post)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 l      = ARM64_DASM_BIT(inst, 22);
  U32 r      = ARM64_DASM_BIT(inst, 21);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 13);
  U32 s      = ARM64_DASM_BIT(inst, 12);
  U32 size   = ARM64_DASM_BITS(inst, 11, 10);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rt     = ARM64_DASM_BITS(inst, 4, 0);
  U32 nregs  = (r ? 2 : 1) + ((opcode & 1) ? 2 : 0);
  U32 sel    = opcode >> 1;
  U32 esz    = 0;
  U32 index  = 0;
  B32 ok     = 1;
  B32 replicate = 0;
  switch(sel)
  {
    case 0:
    {
      esz = 0;
      index = (q << 3) | (s << 2) | size;
    }break;
    case 1:
    {
      if(size & 1) { ok = 0; }
      esz = 1;
      index = (q << 2) | (s << 1) | (size >> 1);
    }break;
    case 2:
    {
      if(size == 0)              { esz = 2; index = (q << 1) | s; }
      else if(size == 1 && !s)   { esz = 3; index = q; }
      else                       { ok = 0; }
    }break;
    case 3:
    {
      if(!l || s) { ok = 0; }
      replicate = 1;
      esz = size;
    }break;
  }
  if(ok && (post || rm == 0))
  {
    arm64_dasm_mnemf(ctx, "%s%u%s", l ? "ld" : "st", nregs, replicate ? "r" : "");
    if(replicate)
    {
      arm64_dasm_op_vlist(ctx, rt, nregs, arm64_dasm_arr(esz, q));
    }
    else
    {
      arm64_dasm_op(ctx);
      arm64_dasm_put(ctx, "{");
      for(U32 i = 0; i < nregs; i += 1)
      {
        arm64_dasm_putf(ctx, "%sv%u.%c", i == 0 ? "" : ", ", (rt + i) & 31, arm64_dasm_kind_from_esz(esz));
      }
      arm64_dasm_putf(ctx, "}[%u]", index);
    }
    arm64_dasm_op_mem(ctx, rn, 0, ARM64_DasmMemKind_Base);
    if(post)
    {
      if(rm == 31) { arm64_dasm_op_imm(ctx, nregs * (1u << esz)); }
      else         { arm64_dasm_op_gpr(ctx, rm, 1, 0); }
      arm64_dasm_mark_sp_writeback(ctx, rn, rm != 31);
    }
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rt, 'v'), 0);
    arm64_dasm_set_mem(ctx, 1, rn, 0);
    result = 1;
  }
  return result;
}

//- rjf: load/store top-level

internal B32
arm64_dasm_decode_ldst(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 op0 = ARM64_DASM_BITS(inst, 31, 28);
  U32 op1 = ARM64_DASM_BIT(inst, 26);
  U32 op2 = ARM64_DASM_BITS(inst, 24, 23);
  U32 op3 = ARM64_DASM_BITS(inst, 21, 16);
  U32 op4 = ARM64_DASM_BITS(inst, 11, 10);
  if((op0 & 0xb) == 0 && op1 == 1)
  {
    // advanced SIMD load/store structures
    switch(op2)
    {
      case 0:{if(op3 == 0)            { result = arm64_dasm_decode_simd_ldst_multiple(ctx, 0); }}break;
      case 1:{if((op3 & 0x20) == 0)   { result = arm64_dasm_decode_simd_ldst_multiple(ctx, 1); }}break;
      case 2:{if((op3 & 0x1f) == 0)   { result = arm64_dasm_decode_simd_ldst_single(ctx, 0); }}break;
      case 3:{                          result = arm64_dasm_decode_simd_ldst_single(ctx, 1); }break;
    }
  }
  else if((op0 & 3) == 0 && op1 == 0 && (op2 & 2) == 0)
  {
    result = arm64_dasm_decode_ldst_exclusive(ctx);
  }
  else if((op0 & 3) == 1)
  {
    if((op2 & 2) == 0)
    {
      result = arm64_dasm_decode_ldst_literal(ctx);
    }
    else if(op1 == 0 && (op3 & 0x20) == 0 && op4 == 0)
    {
      result = arm64_dasm_decode_ldst_ldapur(ctx);
    }
  }
  else if((op0 & 3) == 2)
  {
    result = arm64_dasm_decode_ldst_pair(ctx);
  }
  else if((op0 & 3) == 3)
  {
    if(op2 & 2)
    {
      result = arm64_dasm_decode_ldst_unsigned_imm(ctx);
    }
    else if((op3 & 0x20) == 0)
    {
      result = arm64_dasm_decode_ldst_imm9(ctx);
    }
    else if(op4 == 0)
    {
      result = arm64_dasm_decode_ldst_atomic(ctx);
    }
    else if(op4 == 2)
    {
      result = arm64_dasm_decode_ldst_reg_offset(ctx);
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Data Processing (Register)

// rjf: optional ", <shift> #amount" suffix
internal void
arm64_dasm_op_shift(ARM64_DasmCtx *ctx, U32 shift, U32 amount)
{
  if(shift != 0 || amount != 0)
  {
    arm64_dasm_op_f(ctx, "%s #%u", arm64_dasm_shift_name(shift), amount);
  }
}

internal B32
arm64_dasm_decode_dp_reg(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 op0 = ARM64_DASM_BIT(inst, 30);
  U32 op1 = ARM64_DASM_BIT(inst, 28);
  U32 op2 = ARM64_DASM_BITS(inst, 24, 21);
  U32 op3 = ARM64_DASM_BITS(inst, 15, 10);
  U32 sf  = ARM64_DASM_BIT(inst, 31);
  U32 rm  = ARM64_DASM_BITS(inst, 20, 16);
  U32 rn  = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd  = ARM64_DASM_BITS(inst, 4, 0);
  if(op1 == 0)
  {
    if((op2 & 8) == 0)
    {
      //- rjf: logical (shifted register)
      U32 opc   = ARM64_DASM_BITS(inst, 30, 29);
      U32 shift = ARM64_DASM_BITS(inst, 23, 22);
      U32 n     = ARM64_DASM_BIT(inst, 21);
      U32 imm6  = ARM64_DASM_BITS(inst, 15, 10);
      if(sf || (imm6 & 0x20) == 0)
      {
        local_persist char *names[4][2] = {{"and", "bic"}, {"orr", "orn"}, {"eor", "eon"}, {"ands", "bics"}};
        if(opc == 1 && n == 0 && rn == 31 && shift == 0 && imm6 == 0)
        {
          arm64_dasm_mnem(ctx, "mov");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
        }
        else if(opc == 1 && n == 1 && rn == 31)
        {
          arm64_dasm_mnem(ctx, "mvn");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
          arm64_dasm_op_shift(ctx, shift, imm6);
        }
        else if(opc == 3 && n == 0 && rd == 31)
        {
          arm64_dasm_mnem(ctx, "tst");
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
          arm64_dasm_op_shift(ctx, shift, imm6);
        }
        else
        {
          arm64_dasm_mnem(ctx, names[opc][n]);
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
          arm64_dasm_op_shift(ctx, shift, imm6);
        }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
        result = 1;
      }
    }
    else if((op2 & 1) == 0)
    {
      //- rjf: add/subtract (shifted register)
      U32 op    = ARM64_DASM_BIT(inst, 30);
      U32 s     = ARM64_DASM_BIT(inst, 29);
      U32 shift = ARM64_DASM_BITS(inst, 23, 22);
      U32 imm6  = ARM64_DASM_BITS(inst, 15, 10);
      if(shift != 3 && (sf || (imm6 & 0x20) == 0))
      {
        local_persist char *names[2][2] = {{"add", "adds"}, {"sub", "subs"}};
        if(s && rd == 31)
        {
          arm64_dasm_mnem(ctx, op ? "cmp" : "cmn");
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
        }
        else if(op && rn == 31)
        {
          arm64_dasm_mnem(ctx, s ? "negs" : "neg");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
        }
        else
        {
          arm64_dasm_mnem(ctx, names[op][s]);
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_gpr(ctx, rm, sf, 0);
        }
        arm64_dasm_op_shift(ctx, shift, imm6);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
        result = 1;
      }
    }
    else
    {
      //- rjf: add/subtract (extended register)
      U32 op     = ARM64_DASM_BIT(inst, 30);
      U32 s      = ARM64_DASM_BIT(inst, 29);
      U32 opt    = ARM64_DASM_BITS(inst, 23, 22);
      U32 option = ARM64_DASM_BITS(inst, 15, 13);
      U32 imm3   = ARM64_DASM_BITS(inst, 12, 10);
      if(opt == 0 && imm3 <= 4)
      {
        local_persist char *names[2][2] = {{"add", "adds"}, {"sub", "subs"}};
        B32 rd_sp = !s;
        B32 rm_is64 = (sf && (option & 3) == 3);
        B32 lsl_form = (((rd == 31 && !s) || rn == 31) && option == (sf ? 3u : 2u));
        if(s && rd == 31)
        {
          arm64_dasm_mnem(ctx, op ? "cmp" : "cmn");
        }
        else
        {
          arm64_dasm_mnem(ctx, names[op][s]);
          arm64_dasm_op_gpr(ctx, rd, sf, rd_sp);
        }
        arm64_dasm_op_gpr(ctx, rn, sf, 1);
        arm64_dasm_op_gpr(ctx, rm, rm_is64, 0);
        if(lsl_form)
        {
          if(imm3 != 0) { arm64_dasm_op_f(ctx, "lsl #%u", imm3); }
        }
        else
        {
          if(imm3 != 0) { arm64_dasm_op_f(ctx, "%s #%u", arm64_dasm_extend_name(option), imm3); }
          else          { arm64_dasm_op_str(ctx, arm64_dasm_extend_name(option)); }
        }
        if(rd == 31 && rd_sp)
        {
          ctx->flags |= DASM_InstFlag_ChangesStackPointer|DASM_InstFlag_ChangesStackPointerVariably;
        }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, rd_sp), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 1), 0);
        result = 1;
      }
    }
  }
  else
  {
    switch(op2)
    {
      default:{}break;

      //- rjf: add/subtract with carry, rotate right into flags, evaluate into flags
      case 0:
      {
        U32 op = ARM64_DASM_BIT(inst, 30);
        U32 s  = ARM64_DASM_BIT(inst, 29);
        if(op3 == 0)
        {
          local_persist char *names[2][2] = {{"adc", "adcs"}, {"sbc", "sbcs"}};
          if(op && rn == 31)
          {
            arm64_dasm_mnem(ctx, s ? "ngcs" : "ngc");
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            arm64_dasm_op_gpr(ctx, rm, sf, 0);
          }
          else
          {
            arm64_dasm_mnem(ctx, names[op][s]);
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            arm64_dasm_op_gpr(ctx, rn, sf, 0);
            arm64_dasm_op_gpr(ctx, rm, sf, 0);
          }
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
          result = 1;
        }
        else if(sf == 1 && op == 0 && s == 1 && (op3 & 0x1f) == 1)
        {
          // RMIF
          U32 imm6 = ARM64_DASM_BITS(inst, 20, 15);
          U32 mask = ARM64_DASM_BITS(inst, 3, 0);
          if(ARM64_DASM_BIT(inst, 4) == 0)
          {
            arm64_dasm_mnem(ctx, "rmif");
            arm64_dasm_op_gpr(ctx, rn, 1, 0);
            arm64_dasm_op_dec(ctx, imm6);
            arm64_dasm_op_dec(ctx, mask);
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, 1, 0), 0);
            result = 1;
          }
        }
        else if(sf == 0 && op == 0 && s == 1 && (op3 & 0xf) == 2 && ARM64_DASM_BITS(inst, 4, 0) == 13)
        {
          // SETF8 / SETF16
          arm64_dasm_mnem(ctx, ARM64_DASM_BIT(inst, 14) ? "setf16" : "setf8");
          arm64_dasm_op_gpr(ctx, rn, 0, 0);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, 0, 0), 0);
          result = 1;
        }
      }break;

      //- rjf: conditional compare (register / immediate)
      case 2:
      {
        U32 op   = ARM64_DASM_BIT(inst, 30);
        U32 s    = ARM64_DASM_BIT(inst, 29);
        U32 cond = ARM64_DASM_BITS(inst, 15, 12);
        U32 is_imm = ARM64_DASM_BIT(inst, 11);
        U32 o2   = ARM64_DASM_BIT(inst, 10);
        U32 o3   = ARM64_DASM_BIT(inst, 4);
        U32 nzcv = ARM64_DASM_BITS(inst, 3, 0);
        if(s == 1 && o2 == 0 && o3 == 0)
        {
          arm64_dasm_mnem(ctx, op ? "ccmp" : "ccmn");
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          if(is_imm) { arm64_dasm_op_imm(ctx, rm); }
          else       { arm64_dasm_op_gpr(ctx, rm, sf, 0); }
          arm64_dasm_op_dec(ctx, nzcv);
          arm64_dasm_op_str(ctx, arm64_dasm_cond_name(cond));
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rn, sf, 0), 0);
          if(!is_imm) { arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rm, sf, 0), 0); }
          result = 1;
        }
      }break;

      //- rjf: conditional select
      case 4:
      {
        U32 op   = ARM64_DASM_BIT(inst, 30);
        U32 s    = ARM64_DASM_BIT(inst, 29);
        U32 cond = ARM64_DASM_BITS(inst, 15, 12);
        U32 o2   = ARM64_DASM_BITS(inst, 11, 10);
        if(s == 0 && (o2 & 2) == 0)
        {
          U32 kind = (op << 1) | o2; // 0 csel, 1 csinc, 2 csinv, 3 csneg
          B32 cond_invertible = ((cond & 0xe) != 0xe);
          if(kind != 0 && rm == rn && cond_invertible)
          {
            char *inv_cond = arm64_dasm_cond_name(cond ^ 1);
            if(rn == 31 && kind != 3)
            {
              arm64_dasm_mnem(ctx, kind == 1 ? "cset" : "csetm");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_str(ctx, inv_cond);
            }
            else
            {
              arm64_dasm_mnem(ctx, kind == 1 ? "cinc" : kind == 2 ? "cinv" : "cneg");
              arm64_dasm_op_gpr(ctx, rd, sf, 0);
              arm64_dasm_op_gpr(ctx, rn, sf, 0);
              arm64_dasm_op_str(ctx, inv_cond);
            }
          }
          else
          {
            local_persist char *names[4] = {"csel", "csinc", "csinv", "csneg"};
            arm64_dasm_mnem(ctx, names[kind]);
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            arm64_dasm_op_gpr(ctx, rn, sf, 0);
            arm64_dasm_op_gpr(ctx, rm, sf, 0);
            arm64_dasm_op_str(ctx, arm64_dasm_cond_name(cond));
          }
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
          result = 1;
        }
      }break;

      //- rjf: data-processing (2 source) / (1 source)
      case 6:
      {
        U32 s = ARM64_DASM_BIT(inst, 29);
        if(op0 == 0)
        {
          // 2 source
          U32 opcode = op3;
          char *name = 0;
          B32 rd_sp = 0;
          B32 rn_sp = 0;
          B32 rm_sp = 0;
          B32 rm_omit_zr = 0;
          B32 rm_is64 = sf;
          B32 rd_is64 = sf;
          B32 rn_is64 = sf;
          if(s == 0)
          {
            switch(opcode)
            {
              default:{}break;
              case 0x00:{if(sf) { name = "subp"; rn_sp = 1; rm_sp = 1; }}break;
              case 0x02:{name = "udiv";}break;
              case 0x03:{name = "sdiv";}break;
              case 0x04:{if(sf) { name = "irg"; rd_sp = 1; rn_sp = 1; rm_omit_zr = 1; }}break;
              case 0x05:{if(sf) { name = "gmi"; rn_sp = 1; }}break;
              case 0x08:{name = "lsl";}break;
              case 0x09:{name = "lsr";}break;
              case 0x0a:{name = "asr";}break;
              case 0x0b:{name = "ror";}break;
              case 0x0c:{if(sf) { name = "pacga"; rm_sp = 1; }}break;
              case 0x10:{if(!sf) { name = "crc32b"; }}break;
              case 0x11:{if(!sf) { name = "crc32h"; }}break;
              case 0x12:{if(!sf) { name = "crc32w"; }}break;
              case 0x13:{if(sf) { name = "crc32x"; rd_is64 = 0; rn_is64 = 0; }}break;
              case 0x14:{if(!sf) { name = "crc32cb"; }}break;
              case 0x15:{if(!sf) { name = "crc32ch"; }}break;
              case 0x16:{if(!sf) { name = "crc32cw"; }}break;
              case 0x17:{if(sf) { name = "crc32cx"; rd_is64 = 0; rn_is64 = 0; }}break;
            }
          }
          else if(sf && opcode == 0)
          {
            name = (rd == 31) ? "cmpp" : "subps";
            rn_sp = 1;
            rm_sp = 1;
          }
          if(name != 0)
          {
            arm64_dasm_mnem(ctx, name);
            if(!(s && rd == 31)) { arm64_dasm_op_gpr(ctx, rd, rd_is64, rd_sp); }
            arm64_dasm_op_gpr(ctx, rn, rn_is64, rn_sp);
            if(!(rm_omit_zr && rm == 31)) { arm64_dasm_op_gpr(ctx, rm, rm_is64, rm_sp); }
            if(rd_sp && rd == 31) { ctx->flags |= DASM_InstFlag_ChangesStackPointer|DASM_InstFlag_ChangesStackPointerVariably; }
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, rd_is64, rd_sp), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, rn_is64, rn_sp), 0);
            result = 1;
          }
        }
        else if(s == 0)
        {
          // 1 source
          U32 opcode2 = ARM64_DASM_BITS(inst, 20, 16);
          U32 opcode  = op3;
          char *name = 0;
          B32 one_operand = 0;
          B32 rn_sp = 0;
          if(opcode2 == 0)
          {
            switch(opcode)
            {
              default:{}break;
              case 0:{name = "rbit";}break;
              case 1:{name = "rev16";}break;
              case 2:{name = sf ? "rev32" : "rev";}break;
              case 3:{if(sf) { name = "rev"; }}break;
              case 4:{name = "clz";}break;
              case 5:{name = "cls";}break;
              case 6:{name = "ctz";}break;
              case 7:{name = "cnt";}break;
              case 8:{name = "abs";}break;
            }
          }
          else if(sf && opcode2 == 1)
          {
            local_persist char *names[18] =
            {
              "pacia", "pacib", "pacda", "pacdb", "autia", "autib", "autda", "autdb",
              "paciza", "pacizb", "pacdza", "pacdzb", "autiza", "autizb", "autdza", "autdzb",
              "xpaci", "xpacd",
            };
            if(opcode < 18)
            {
              name = names[opcode];
              one_operand = (opcode >= 8);
              rn_sp = (opcode < 8);
              if(one_operand && rn != 31) { name = 0; }
            }
          }
          if(name != 0)
          {
            arm64_dasm_mnem(ctx, name);
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            if(!one_operand) { arm64_dasm_op_gpr(ctx, rn, sf, rn_sp); }
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
            if(!one_operand) { arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, rn_sp), 0); }
            result = 1;
          }
        }
      }break;

      //- rjf: data-processing (3 source)
      case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
      {
        U32 op54 = ARM64_DASM_BITS(inst, 30, 29);
        U32 op31 = ARM64_DASM_BITS(inst, 23, 21);
        U32 o0   = ARM64_DASM_BIT(inst, 15);
        U32 ra   = ARM64_DASM_BITS(inst, 14, 10);
        if(op54 == 0)
        {
          char *name = 0;
          char *alias = 0; // when ra == 31
          B32 has_ra = 1;
          B32 srcs_is64 = sf;
          switch((op31 << 1) | o0)
          {
            default:{}break;
            case 0x0:{name = "madd";   alias = "mul";}break;
            case 0x1:{name = "msub";   alias = "mneg";}break;
            case 0x2:{if(sf) { name = "smaddl"; alias = "smull";  srcs_is64 = 0; }}break;
            case 0x3:{if(sf) { name = "smsubl"; alias = "smnegl"; srcs_is64 = 0; }}break;
            case 0x4:{if(sf) { name = "smulh"; has_ra = 0; }}break;
            case 0xa:{if(sf) { name = "umaddl"; alias = "umull";  srcs_is64 = 0; }}break;
            case 0xb:{if(sf) { name = "umsubl"; alias = "umnegl"; srcs_is64 = 0; }}break;
            case 0xc:{if(sf) { name = "umulh"; has_ra = 0; }}break;
          }
          if(name != 0 && (has_ra || ra == 31))
          {
            if(has_ra && ra == 31 && alias != 0)
            {
              arm64_dasm_mnem(ctx, alias);
              has_ra = 0;
            }
            else
            {
              arm64_dasm_mnem(ctx, name);
            }
            arm64_dasm_op_gpr(ctx, rd, sf, 0);
            arm64_dasm_op_gpr(ctx, rn, srcs_is64, 0);
            arm64_dasm_op_gpr(ctx, rm, srcs_is64, 0);
            if(has_ra) { arm64_dasm_op_gpr(ctx, ra, sf, 0); }
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, srcs_is64, 0), 0);
            result = 1;
          }
        }
      }break;
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Scalar Floating Point

// rjf: scalar FP register kind from the `ftype` field (00 S, 01 D, 11 H); 0 if reserved
internal char
arm64_dasm_fp_kind(U32 type)
{
  char result = 0;
  switch(type)
  {
    default:{}break;
    case 0:{result = 's';}break;
    case 1:{result = 'd';}break;
    case 3:{result = 'h';}break;
  }
  return result;
}

internal B32
arm64_dasm_decode_fp(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 m    = ARM64_DASM_BIT(inst, 31);
  U32 s    = ARM64_DASM_BIT(inst, 29);
  U32 type = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm   = ARM64_DASM_BITS(inst, 20, 16);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd   = ARM64_DASM_BITS(inst, 4, 0);
  char fk  = arm64_dasm_fp_kind(type);
  if(s != 0)
  {
    // unallocated
  }
  else if(ARM64_DASM_BIT(inst, 24))
  {
    //- rjf: data-processing (3 source)
    U32 o1 = ARM64_DASM_BIT(inst, 21);
    U32 o0 = ARM64_DASM_BIT(inst, 15);
    U32 ra = ARM64_DASM_BITS(inst, 14, 10);
    if(m == 0 && fk != 0)
    {
      local_persist char *names[2][2] = {{"fmadd", "fmsub"}, {"fnmadd", "fnmsub"}};
      arm64_dasm_mnem(ctx, names[o1][o0]);
      arm64_dasm_op_sreg(ctx, rd, fk);
      arm64_dasm_op_sreg(ctx, rn, fk);
      arm64_dasm_op_sreg(ctx, rm, fk);
      arm64_dasm_op_sreg(ctx, ra, fk);
      arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
      arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, fk), 0);
      result = 1;
    }
  }
  else if(ARM64_DASM_BIT(inst, 21) == 0)
  {
    //- rjf: conversion between floating-point and fixed-point
    U32 sf     = m;
    U32 rmode  = ARM64_DASM_BITS(inst, 20, 19);
    U32 opcode = ARM64_DASM_BITS(inst, 18, 16);
    U32 scale  = ARM64_DASM_BITS(inst, 15, 10);
    U32 fbits  = 64 - scale;
    if(fk != 0 && (sf || scale >= 32))
    {
      switch((rmode << 3) | opcode)
      {
        default:{}break;
        case 0x02:
        case 0x03:
        {
          arm64_dasm_mnem(ctx, opcode == 2 ? "scvtf" : "ucvtf");
          arm64_dasm_op_sreg(ctx, rd, fk);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_op_dec(ctx, fbits);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
          result = 1;
        }break;
        case 0x18:
        case 0x19:
        {
          arm64_dasm_mnem(ctx, opcode == 0 ? "fcvtzs" : "fcvtzu");
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_sreg(ctx, rn, fk);
          arm64_dasm_op_dec(ctx, fbits);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, fk), 0);
          result = 1;
        }break;
      }
    }
  }
  else
  {
    U32 lo = ARM64_DASM_BITS(inst, 15, 10);
    if(lo == 0)
    {
      //- rjf: conversion between floating-point and integer
      U32 sf     = m;
      U32 rmode  = ARM64_DASM_BITS(inst, 20, 19);
      U32 opcode = ARM64_DASM_BITS(inst, 18, 16);
      U32 key    = (rmode << 3) | opcode;
      char *name = 0;
      B32 to_int = 1; // Rd is integer
      switch(key)
      {
        default:{}break;
        case 0x00:{name = "fcvtns";}break;
        case 0x01:{name = "fcvtnu";}break;
        case 0x02:{name = "scvtf"; to_int = 0;}break;
        case 0x03:{name = "ucvtf"; to_int = 0;}break;
        case 0x04:{name = "fcvtas";}break;
        case 0x05:{name = "fcvtau";}break;
        case 0x06:
        case 0x07:
        {
          if((sf == 0 && type == 0) || (sf == 1 && type == 1) || type == 3)
          {
            name = "fmov";
            to_int = (key == 0x06);
          }
        }break;
        case 0x08:{name = "fcvtps";}break;
        case 0x09:{name = "fcvtpu";}break;
        case 0x10:{name = "fcvtms";}break;
        case 0x11:{name = "fcvtmu";}break;
        case 0x18:{name = "fcvtzs";}break;
        case 0x19:{name = "fcvtzu";}break;
      }
      if(name != 0 && fk != 0)
      {
        arm64_dasm_mnem(ctx, name);
        if(to_int)
        {
          arm64_dasm_op_gpr(ctx, rd, sf, 0);
          arm64_dasm_op_sreg(ctx, rn, fk);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, sf, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, fk), 0);
        }
        else
        {
          arm64_dasm_op_sreg(ctx, rd, fk);
          arm64_dasm_op_gpr(ctx, rn, sf, 0);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, sf, 0), 0);
        }
        result = 1;
      }
      else if(sf == 1 && type == 2 && (key == 0x0e || key == 0x0f))
      {
        // FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn
        arm64_dasm_mnem(ctx, "fmov");
        if(key == 0x0e)
        {
          arm64_dasm_op_gpr(ctx, rd, 1, 0);
          arm64_dasm_op_velem(ctx, rn, 'd', 1);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, 1, 0), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, 'v'), 0);
        }
        else
        {
          arm64_dasm_op_velem(ctx, rd, 'd', 1);
          arm64_dasm_op_gpr(ctx, rn, 1, 0);
          arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, 'v'), 0);
          arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, 1, 0), 0);
        }
        result = 1;
      }
      else if(sf == 0 && type == 1 && key == 0x1e)
      {
        arm64_dasm_mnem(ctx, "fjcvtzs");
        arm64_dasm_op_gpr(ctx, rd, 0, 0);
        arm64_dasm_op_sreg(ctx, rn, 'd');
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, 0, 0), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, 'd'), 0);
        result = 1;
      }
    }
    else if(m != 0)
    {
      // unallocated
    }
    else if((lo & 0x1f) == 0x10)
    {
      //- rjf: data-processing (1 source)
      U32 opcode = ARM64_DASM_BITS(inst, 20, 15);
      char *name = 0;
      char dk = fk;
      char nk = fk;
      switch(opcode)
      {
        default:{}break;
        case 0x00:{name = "fmov";}break;
        case 0x01:{name = "fabs";}break;
        case 0x02:{name = "fneg";}break;
        case 0x03:{name = "fsqrt";}break;
        case 0x04:{name = "fcvt"; dk = 's';}break;
        case 0x05:{name = "fcvt"; dk = 'd';}break;
        case 0x06:{if(type == 1) { name = "bfcvt"; dk = 'h'; nk = 's'; }}break;
        case 0x07:{name = "fcvt"; dk = 'h';}break;
        case 0x08:{name = "frintn";}break;
        case 0x09:{name = "frintp";}break;
        case 0x0a:{name = "frintm";}break;
        case 0x0b:{name = "frintz";}break;
        case 0x0c:{name = "frinta";}break;
        case 0x0e:{name = "frintx";}break;
        case 0x0f:{name = "frinti";}break;
        case 0x10:{name = "frint32z";}break;
        case 0x11:{name = "frint32x";}break;
        case 0x12:{name = "frint64z";}break;
        case 0x13:{name = "frint64x";}break;
      }
      if(name != 0 && fk != 0 && (opcode > 7 || opcode < 4 || dk != fk) && !(opcode >= 0x10 && fk == 'h'))
      {
        arm64_dasm_mnem(ctx, name);
        arm64_dasm_op_sreg(ctx, rd, dk);
        arm64_dasm_op_sreg(ctx, rn, nk);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, dk), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, nk), 0);
        result = 1;
      }
    }
    else if((lo & 0xf) == 0x8)
    {
      //- rjf: compare
      U32 op      = ARM64_DASM_BITS(inst, 15, 14);
      U32 opcode2 = ARM64_DASM_BITS(inst, 4, 0);
      if(op == 0 && fk != 0 && (opcode2 & 7) == 0)
      {
        arm64_dasm_mnem(ctx, (opcode2 & 0x10) ? "fcmpe" : "fcmp");
        arm64_dasm_op_sreg(ctx, rn, fk);
        if(opcode2 & 0x8) { arm64_dasm_op_fimm(ctx, 0.0); }
        else              { arm64_dasm_op_sreg(ctx, rm, fk); }
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rn, fk), 0);
        if(!(opcode2 & 0x8)) { arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rm, fk), 0); }
        result = 1;
      }
    }
    else if((lo & 0x7) == 0x4)
    {
      //- rjf: immediate
      U32 imm8 = ARM64_DASM_BITS(inst, 20, 13);
      U32 imm5 = ARM64_DASM_BITS(inst, 9, 5);
      if(imm5 == 0 && fk != 0)
      {
        arm64_dasm_mnem(ctx, "fmov");
        arm64_dasm_op_sreg(ctx, rd, fk);
        arm64_dasm_op_fimm(ctx, arm64_dasm_fp_imm8(imm8));
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
        result = 1;
      }
    }
    else if((lo & 0x3) == 0x1)
    {
      //- rjf: conditional compare
      U32 cond = ARM64_DASM_BITS(inst, 15, 12);
      U32 op   = ARM64_DASM_BIT(inst, 4);
      U32 nzcv = ARM64_DASM_BITS(inst, 3, 0);
      if(fk != 0)
      {
        arm64_dasm_mnem(ctx, op ? "fccmpe" : "fccmp");
        arm64_dasm_op_sreg(ctx, rn, fk);
        arm64_dasm_op_sreg(ctx, rm, fk);
        arm64_dasm_op_dec(ctx, nzcv);
        arm64_dasm_op_str(ctx, arm64_dasm_cond_name(cond));
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rn, fk), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rm, fk), 0);
        result = 1;
      }
    }
    else if((lo & 0x3) == 0x2)
    {
      //- rjf: data-processing (2 source)
      U32 opcode = ARM64_DASM_BITS(inst, 15, 12);
      local_persist char *names[9] = {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"};
      if(opcode < 9 && fk != 0)
      {
        arm64_dasm_mnem(ctx, names[opcode]);
        arm64_dasm_op_sreg(ctx, rd, fk);
        arm64_dasm_op_sreg(ctx, rn, fk);
        arm64_dasm_op_sreg(ctx, rm, fk);
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, fk), 0);
        result = 1;
      }
    }
    else if((lo & 0x3) == 0x3)
    {
      //- rjf: conditional select
      U32 cond = ARM64_DASM_BITS(inst, 15, 12);
      if(fk != 0)
      {
        arm64_dasm_mnem(ctx, "fcsel");
        arm64_dasm_op_sreg(ctx, rd, fk);
        arm64_dasm_op_sreg(ctx, rn, fk);
        arm64_dasm_op_sreg(ctx, rm, fk);
        arm64_dasm_op_str(ctx, arm64_dasm_cond_name(cond));
        arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, fk), 0);
        arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, fk), 0);
        result = 1;
      }
    }
  }
  return result;
}

////////////////////////////////
//~ rjf: Advanced SIMD

typedef enum ARM64_DasmSimdCat
{
  ARM64_DasmSimdCat_Int,       // arrangement from size:Q
  ARM64_DasmSimdCat_IntZero,   // integer compare against #0
  ARM64_DasmSimdCat_Fp,        // arrangement from sz:Q (2s/4s/2d); FP16 class uses 4h/8h
  ARM64_DasmSimdCat_FpZero,    // FP compare against #0.0
  ARM64_DasmSimdCat_Logic,     // always 8b/16b
  ARM64_DasmSimdCat_Narrow,    // dest size, source size+1 (XTN etc.)
  ARM64_DasmSimdCat_LongPair,  // dest size+1, source size (SADDLP etc.)
  ARM64_DasmSimdCat_Shll,      // SHLL
  ARM64_DasmSimdCat_FpNarrow,  // FCVTN / FCVTXN
  ARM64_DasmSimdCat_FpLong,    // FCVTL
}
ARM64_DasmSimdCat;

// rjf: vector register code helpers for SIMD results
internal void
arm64_dasm_simd_set_regs(ARM64_DasmCtx *ctx, U32 rd, char dk, U32 rn, char nk)
{
  arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, dk), 0);
  arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, nk), 0);
}

//- rjf: three same (vector & scalar, incl. FP16 forms)

// rjf: name + category + allowed element sizes (bit i => size i) for the vector
// and scalar forms; a zero size mask means the form does not exist
internal char *
arm64_dasm_simd_three_same_name(U32 u, U32 opcode, U32 size, ARM64_DasmSimdCat *cat_out, U32 *vec_sizes_out, U32 *scalar_sizes_out)
{
  char *n = 0;
  ARM64_DasmSimdCat cat = ARM64_DasmSimdCat_Int;
  U32 hi = (size >> 1) & 1;
  U32 vs = 0x7; // B/H/S
  U32 ss = 0x0; // no scalar form
  switch((u << 5) | opcode)
  {
    default:{}break;
    case 0x00:{n = "shadd";}break;
    case 0x01:{n = "sqadd";   vs = 0xf; ss = 0xf;}break;
    case 0x02:{n = "srhadd";}break;
    case 0x03:
    {
      local_persist char *names[4] = {"and", "bic", "orr", "orn"};
      n = names[size]; cat = ARM64_DasmSimdCat_Logic;
    }break;
    case 0x04:{n = "shsub";}break;
    case 0x05:{n = "sqsub";   vs = 0xf; ss = 0xf;}break;
    case 0x06:{n = "cmgt";    vs = 0xf; ss = 0x8;}break;
    case 0x07:{n = "cmge";    vs = 0xf; ss = 0x8;}break;
    case 0x08:{n = "sshl";    vs = 0xf; ss = 0x8;}break;
    case 0x09:{n = "sqshl";   vs = 0xf; ss = 0xf;}break;
    case 0x0a:{n = "srshl";   vs = 0xf; ss = 0x8;}break;
    case 0x0b:{n = "sqrshl";  vs = 0xf; ss = 0xf;}break;
    case 0x0c:{n = "smax";}break;
    case 0x0d:{n = "smin";}break;
    case 0x0e:{n = "sabd";}break;
    case 0x0f:{n = "saba";}break;
    case 0x10:{n = "add";     vs = 0xf; ss = 0x8;}break;
    case 0x11:{n = "cmtst";   vs = 0xf; ss = 0x8;}break;
    case 0x12:{n = "mla";}break;
    case 0x13:{n = "mul";}break;
    case 0x14:{n = "smaxp";}break;
    case 0x15:{n = "sminp";}break;
    case 0x16:{n = "sqdmulh"; vs = 0x6; ss = 0x6;}break;
    case 0x17:{n = "addp";    vs = 0xf;}break;
    case 0x18:{n = hi ? "fminnm" : "fmaxnm"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x19:{n = hi ? "fmls" : "fmla"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1a:{n = hi ? "fsub" : "fadd"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1b:{n = hi ? 0 : "fmulx"; cat = ARM64_DasmSimdCat_Fp; ss = 0xf;}break;
    case 0x1c:{n = hi ? 0 : "fcmeq"; cat = ARM64_DasmSimdCat_Fp; ss = 0xf;}break;
    case 0x1e:{n = hi ? "fmin" : "fmax"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1f:{n = hi ? "frsqrts" : "frecps"; cat = ARM64_DasmSimdCat_Fp; ss = 0xf;}break;
    case 0x20:{n = "uhadd";}break;
    case 0x21:{n = "uqadd";   vs = 0xf; ss = 0xf;}break;
    case 0x22:{n = "urhadd";}break;
    case 0x23:
    {
      local_persist char *names[4] = {"eor", "bsl", "bit", "bif"};
      n = names[size]; cat = ARM64_DasmSimdCat_Logic;
    }break;
    case 0x24:{n = "uhsub";}break;
    case 0x25:{n = "uqsub";   vs = 0xf; ss = 0xf;}break;
    case 0x26:{n = "cmhi";    vs = 0xf; ss = 0x8;}break;
    case 0x27:{n = "cmhs";    vs = 0xf; ss = 0x8;}break;
    case 0x28:{n = "ushl";    vs = 0xf; ss = 0x8;}break;
    case 0x29:{n = "uqshl";   vs = 0xf; ss = 0xf;}break;
    case 0x2a:{n = "urshl";   vs = 0xf; ss = 0x8;}break;
    case 0x2b:{n = "uqrshl";  vs = 0xf; ss = 0xf;}break;
    case 0x2c:{n = "umax";}break;
    case 0x2d:{n = "umin";}break;
    case 0x2e:{n = "uabd";}break;
    case 0x2f:{n = "uaba";}break;
    case 0x30:{n = "sub";     vs = 0xf; ss = 0x8;}break;
    case 0x31:{n = "cmeq";    vs = 0xf; ss = 0x8;}break;
    case 0x32:{n = "mls";}break;
    case 0x33:{n = "pmul";    vs = 0x1;}break;
    case 0x34:{n = "umaxp";}break;
    case 0x35:{n = "uminp";}break;
    case 0x36:{n = "sqrdmulh"; vs = 0x6; ss = 0x6;}break;
    case 0x38:{n = hi ? "fminnmp" : "fmaxnmp"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3a:{n = hi ? "fabd" : "faddp"; cat = ARM64_DasmSimdCat_Fp; ss = hi ? 0xf : 0;}break;
    case 0x3b:{n = hi ? 0 : "fmul"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3c:{n = hi ? "fcmgt" : "fcmge"; cat = ARM64_DasmSimdCat_Fp; ss = 0xf;}break;
    case 0x3d:{n = hi ? "facgt" : "facge"; cat = ARM64_DasmSimdCat_Fp; ss = 0xf;}break;
    case 0x3e:{n = hi ? "fminp" : "fmaxp"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3f:{n = hi ? 0 : "fdiv"; cat = ARM64_DasmSimdCat_Fp;}break;
  }
  if(cat != ARM64_DasmSimdCat_Int) { vs = 0xf; }
  if(cat_out != 0) { *cat_out = cat; }
  if(vec_sizes_out != 0) { *vec_sizes_out = vs; }
  if(scalar_sizes_out != 0) { *scalar_sizes_out = ss; }
  return n;
}

internal B32
arm64_dasm_decode_simd_three_same(ARM64_DasmCtx *ctx, B32 scalar, B32 fp16)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 11);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  if(fp16)
  {
    // FP16 class: opcode is bits [13:11], "a" (bit 23) selects the variant
    opcode = 0x18 | ARM64_DASM_BITS(inst, 13, 11);
    size   = ARM64_DASM_BIT(inst, 23) << 1;
  }
  ARM64_DasmSimdCat cat = ARM64_DasmSimdCat_Int;
  U32 vec_sizes = 0;
  U32 scalar_sizes = 0;
  char *name = arm64_dasm_simd_three_same_name(u, opcode, size, &cat, &vec_sizes, &scalar_sizes);
  if(name != 0 && cat == ARM64_DasmSimdCat_Int && ((vec_sizes >> size) & 1) == 0)
  {
    name = 0;
  }
  if(name != 0)
  {
    if(scalar)
    {
      char kind = 0;
      if(scalar_sizes != 0 && (cat != ARM64_DasmSimdCat_Int || ((scalar_sizes >> size) & 1)))
      {
        switch(cat)
        {
          default:{}break;
          case ARM64_DasmSimdCat_Int:{kind = arm64_dasm_kind_from_esz(size);}break;
          case ARM64_DasmSimdCat_Fp: {kind = fp16 ? 'h' : ((size & 1) ? 'd' : 's');}break;
        }
      }
      if(kind != 0)
      {
        arm64_dasm_mnem(ctx, name);
        arm64_dasm_op_sreg(ctx, rd, kind);
        arm64_dasm_op_sreg(ctx, rn, kind);
        arm64_dasm_op_sreg(ctx, rm, kind);
        arm64_dasm_simd_set_regs(ctx, rd, kind, rn, kind);
        result = 1;
      }
    }
    else
    {
      char *arr = 0;
      switch(cat)
      {
        default:{}break;
        case ARM64_DasmSimdCat_Int:  {if(size < 3 || q) { arr = arm64_dasm_arr(size, q); }}break;
        case ARM64_DasmSimdCat_Fp:   {if(fp16) { arr = arm64_dasm_arr(1, q); } else if(!(size & 1) || q) { arr = arm64_dasm_arr(2 + (size & 1), q); }}break;
        case ARM64_DasmSimdCat_Logic:{arr = arm64_dasm_arr(0, q);}break;
      }
      if(arr != 0)
      {
        if(cat == ARM64_DasmSimdCat_Logic && u == 0 && size == 2 && rm == rn)
        {
          arm64_dasm_mnem(ctx, "mov");
          arm64_dasm_op_vreg(ctx, rd, arr);
          arm64_dasm_op_vreg(ctx, rn, arr);
        }
        else
        {
          arm64_dasm_mnem(ctx, name);
          arm64_dasm_op_vreg(ctx, rd, arr);
          arm64_dasm_op_vreg(ctx, rn, arr);
          arm64_dasm_op_vreg(ctx, rm, arr);
        }
        arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
        result = 1;
      }
    }
  }
  return result;
}

//- rjf: two-register miscellaneous (vector & scalar, incl. FP16 forms)

internal char *
arm64_dasm_simd_two_reg_misc_name(U32 u, U32 opcode, U32 size, ARM64_DasmSimdCat *cat_out)
{
  char *n = 0;
  ARM64_DasmSimdCat cat = ARM64_DasmSimdCat_Int;
  U32 hi = (size >> 1) & 1;
  switch((u << 5) | opcode)
  {
    default:{}break;
    case 0x00:{n = "rev64";}break;
    case 0x01:{n = "rev16";}break;
    case 0x02:{n = "saddlp"; cat = ARM64_DasmSimdCat_LongPair;}break;
    case 0x03:{n = "suqadd";}break;
    case 0x04:{n = "cls";}break;
    case 0x05:{n = "cnt";}break;
    case 0x06:{n = "sadalp"; cat = ARM64_DasmSimdCat_LongPair;}break;
    case 0x07:{n = "sqabs";}break;
    case 0x08:{n = "cmgt"; cat = ARM64_DasmSimdCat_IntZero;}break;
    case 0x09:{n = "cmeq"; cat = ARM64_DasmSimdCat_IntZero;}break;
    case 0x0a:{n = "cmlt"; cat = ARM64_DasmSimdCat_IntZero;}break;
    case 0x0b:{n = "abs";}break;
    case 0x0c:{n = hi ? "fcmgt" : 0; cat = ARM64_DasmSimdCat_FpZero;}break;
    case 0x0d:{n = hi ? "fcmeq" : 0; cat = ARM64_DasmSimdCat_FpZero;}break;
    case 0x0e:{n = hi ? "fcmlt" : 0; cat = ARM64_DasmSimdCat_FpZero;}break;
    case 0x0f:{n = hi ? "fabs" : 0; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x12:{n = "xtn"; cat = ARM64_DasmSimdCat_Narrow;}break;
    case 0x14:{n = "sqxtn"; cat = ARM64_DasmSimdCat_Narrow;}break;
    case 0x16:{n = hi ? 0 : "fcvtn"; cat = ARM64_DasmSimdCat_FpNarrow;}break;
    case 0x17:{n = hi ? 0 : "fcvtl"; cat = ARM64_DasmSimdCat_FpLong;}break;
    case 0x18:{n = hi ? "frintp" : "frintn"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x19:{n = hi ? "frintz" : "frintm"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1a:{n = hi ? "fcvtps" : "fcvtns"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1b:{n = hi ? "fcvtzs" : "fcvtms"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1c:{n = hi ? "urecpe" : "fcvtas"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1d:{n = hi ? "frecpe" : "scvtf"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1e:{n = hi ? 0 : "frint32z"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x1f:{n = hi ? "frecpx" : "frint64z"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x20:{n = "rev32";}break;
    case 0x22:{n = "uaddlp"; cat = ARM64_DasmSimdCat_LongPair;}break;
    case 0x23:{n = "usqadd";}break;
    case 0x24:{n = "clz";}break;
    case 0x25:{n = (size == 0) ? "mvn" : (size == 1) ? "rbit" : 0; cat = ARM64_DasmSimdCat_Logic;}break;
    case 0x26:{n = "uadalp"; cat = ARM64_DasmSimdCat_LongPair;}break;
    case 0x27:{n = "sqneg";}break;
    case 0x28:{n = "cmge"; cat = ARM64_DasmSimdCat_IntZero;}break;
    case 0x29:{n = "cmle"; cat = ARM64_DasmSimdCat_IntZero;}break;
    case 0x2b:{n = "neg";}break;
    case 0x2c:{n = hi ? "fcmge" : 0; cat = ARM64_DasmSimdCat_FpZero;}break;
    case 0x2d:{n = hi ? "fcmle" : 0; cat = ARM64_DasmSimdCat_FpZero;}break;
    case 0x2f:{n = hi ? "fneg" : 0; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x32:{n = "sqxtun"; cat = ARM64_DasmSimdCat_Narrow;}break;
    case 0x33:{n = "shll"; cat = ARM64_DasmSimdCat_Shll;}break;
    case 0x34:{n = "uqxtn"; cat = ARM64_DasmSimdCat_Narrow;}break;
    case 0x36:{n = hi ? 0 : ((size & 1) ? "fcvtxn" : 0); cat = ARM64_DasmSimdCat_FpNarrow;}break;
    case 0x38:{n = hi ? 0 : "frinta"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x39:{n = hi ? "frinti" : "frintx"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3a:{n = hi ? "fcvtpu" : "fcvtnu"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3b:{n = hi ? "fcvtzu" : "fcvtmu"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3c:{n = hi ? "ursqrte" : "fcvtau"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3d:{n = hi ? "frsqrte" : "ucvtf"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3e:{n = hi ? 0 : "frint32x"; cat = ARM64_DasmSimdCat_Fp;}break;
    case 0x3f:{n = hi ? "fsqrt" : "frint64x"; cat = ARM64_DasmSimdCat_Fp;}break;
  }
  if(cat_out != 0) { *cat_out = cat; }
  return n;
}

internal B32
arm64_dasm_decode_simd_two_reg_misc(ARM64_DasmCtx *ctx, B32 scalar, B32 fp16)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 opcode = ARM64_DASM_BITS(inst, 16, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  if(fp16)
  {
    size = ARM64_DASM_BIT(inst, 23) << 1;
  }
  ARM64_DasmSimdCat cat = ARM64_DasmSimdCat_Int;
  char *name = arm64_dasm_simd_two_reg_misc_name(u, opcode, size, &cat);
  U32 sz = size & 1;
  if(fp16 && cat != ARM64_DasmSimdCat_Fp && cat != ARM64_DasmSimdCat_FpZero)
  {
    name = 0;
  }
  if(name != 0 && scalar)
  {
    char dk = 0;
    char nk = 0;
    B32 zero_int = 0;
    B32 zero_fp = 0;
    // scalar integer forms: ABS/NEG/CMxx are D only, SQABS/SQNEG/SUQADD/USQADD any size
    B32 int_any_size = (opcode == 0x03 || opcode == 0x07);
    switch(cat)
    {
      default:{}break;
      case ARM64_DasmSimdCat_Int:     {if(int_any_size || size == 3) { dk = nk = arm64_dasm_kind_from_esz(size); }}break;
      case ARM64_DasmSimdCat_IntZero: {if(size == 3) { dk = nk = arm64_dasm_kind_from_esz(size); zero_int = 1; }}break;
      case ARM64_DasmSimdCat_Fp:      {dk = nk = fp16 ? 'h' : (sz ? 'd' : 's');}break;
      case ARM64_DasmSimdCat_FpZero:  {dk = nk = fp16 ? 'h' : (sz ? 'd' : 's'); zero_fp = 1;}break;
      case ARM64_DasmSimdCat_Narrow:  {if(size < 3) { dk = arm64_dasm_kind_from_esz(size); nk = arm64_dasm_kind_from_esz(size + 1); }}break;
      case ARM64_DasmSimdCat_FpNarrow:{if(sz) { dk = 's'; nk = 'd'; }}break;
    }
    // the scalar two-register class has no REV/CLS/CLZ/CNT/NOT/XTN/SHLL/FCVTN/FCVTL forms
    if(cat == ARM64_DasmSimdCat_Int && !(opcode == 0x03 || opcode == 0x07 || opcode == 0x0b)) { dk = 0; }
    if(cat == ARM64_DasmSimdCat_Narrow && opcode == 0x12 && u == 0) { dk = 0; }
    if(cat == ARM64_DasmSimdCat_Fp && (opcode == 0x0f || opcode == 0x18 || opcode == 0x19 || opcode == 0x1e || (opcode == 0x1f && !((size >> 1) & 1)))) { dk = 0; }
    if(dk != 0 && nk != 0)
    {
      arm64_dasm_mnem(ctx, name);
      arm64_dasm_op_sreg(ctx, rd, dk);
      arm64_dasm_op_sreg(ctx, rn, nk);
      if(zero_int) { arm64_dasm_op_dec(ctx, 0); }
      if(zero_fp)  { arm64_dasm_op_fimm(ctx, 0.0); }
      arm64_dasm_simd_set_regs(ctx, rd, dk, rn, nk);
      result = 1;
    }
  }
  else if(name != 0)
  {
    char *darr = 0;
    char *narr = 0;
    char *suffix = "";
    B32 zero_int = 0;
    B32 zero_fp = 0;
    B32 shll = 0;
    // allowed element sizes for the integer forms (bit i => size i)
    U32 int_sizes = 0xf;
    switch(opcode)
    {
      default:{}break;
      case 0x00:{int_sizes = u ? 0x3 : 0x7;}break; // rev64 / rev32
      case 0x01:{int_sizes = 0x1;}break;           // rev16
      case 0x02: case 0x06:{int_sizes = 0x7;}break; // saddlp/uaddlp, sadalp/uadalp
      case 0x04:{int_sizes = 0x7;}break;           // cls / clz
      case 0x05:{int_sizes = 0x1;}break;           // cnt
    }
    B32 int_ok = (size < 3 || q) && ((int_sizes >> size) & 1);
    B32 fp_ok  = (fp16 || !sz || q);
    char *fp_arr = fp16 ? arm64_dasm_arr(1, q) : arm64_dasm_arr(2 + sz, q);
    switch(cat)
    {
      default:{}break;
      case ARM64_DasmSimdCat_Int:     {if(int_ok) { darr = narr = arm64_dasm_arr(size, q); }}break;
      case ARM64_DasmSimdCat_IntZero: {if(int_ok) { darr = narr = arm64_dasm_arr(size, q); zero_int = 1; }}break;
      case ARM64_DasmSimdCat_Fp:      {if(fp_ok) { darr = narr = fp_arr; }}break;
      case ARM64_DasmSimdCat_FpZero:  {if(fp_ok) { darr = narr = fp_arr; zero_fp = 1; }}break;
      case ARM64_DasmSimdCat_Logic:   {darr = narr = arm64_dasm_arr(0, q);}break;
      case ARM64_DasmSimdCat_Narrow:  {if(size < 3) { darr = arm64_dasm_arr(size, q); narr = arm64_dasm_arr(size + 1, 1); suffix = q ? "2" : ""; }}break;
      case ARM64_DasmSimdCat_LongPair:{if(size < 3) { darr = arm64_dasm_arr(size + 1, q); narr = arm64_dasm_arr(size, q); }}break;
      case ARM64_DasmSimdCat_Shll:    {if(size < 3) { darr = arm64_dasm_arr(size + 1, 1); narr = arm64_dasm_arr(size, q); suffix = q ? "2" : ""; shll = 1; }}break;
      case ARM64_DasmSimdCat_FpNarrow:{darr = arm64_dasm_arr(1 + sz, q); narr = arm64_dasm_arr(2 + sz, 1); suffix = q ? "2" : "";}break;
      case ARM64_DasmSimdCat_FpLong:  {darr = arm64_dasm_arr(2 + sz, 1); narr = arm64_dasm_arr(1 + sz, q); suffix = q ? "2" : "";}break;
    }
    if(darr != 0 && narr != 0)
    {
      arm64_dasm_mnemf(ctx, "%s%s", name, suffix);
      arm64_dasm_op_vreg(ctx, rd, darr);
      arm64_dasm_op_vreg(ctx, rn, narr);
      if(zero_int) { arm64_dasm_op_dec(ctx, 0); }
      if(zero_fp)  { arm64_dasm_op_fimm(ctx, 0.0); }
      if(shll)     { arm64_dasm_op_dec(ctx, 8u << size); }
      arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
      result = 1;
    }
  }
  return result;
}

//- rjf: across lanes

internal B32
arm64_dasm_decode_simd_across_lanes(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 opcode = ARM64_DASM_BITS(inst, 16, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  U32 hi     = (size >> 1) & 1;
  char *name = 0;
  char dk = 0;
  char *narr = 0;
  B32 ok = 0;
  B32 int_ok = (size < 3 && !(size == 2 && !q)); // B/H/S elements; 2S is reserved
  switch((u << 5) | opcode)
  {
    default:{}break;
    case 0x03:{name = "saddlv"; dk = arm64_dasm_kind_from_esz(size + 1); narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x0a:{name = "smaxv";  dk = arm64_dasm_kind_from_esz(size);     narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x1a:{name = "sminv";  dk = arm64_dasm_kind_from_esz(size);     narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x1b:{name = "addv";   dk = arm64_dasm_kind_from_esz(size);     narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x0c:{name = hi ? "fminnmv" : "fmaxnmv"; dk = 'h'; narr = arm64_dasm_arr(1, q); ok = ((size & 1) == 0);}break;
    case 0x0f:{name = hi ? "fminv"   : "fmaxv";   dk = 'h'; narr = arm64_dasm_arr(1, q); ok = ((size & 1) == 0);}break;
    case 0x23:{name = "uaddlv"; dk = arm64_dasm_kind_from_esz(size + 1); narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x2a:{name = "umaxv";  dk = arm64_dasm_kind_from_esz(size);     narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x3a:{name = "uminv";  dk = arm64_dasm_kind_from_esz(size);     narr = arm64_dasm_arr(size, q); ok = int_ok;}break;
    case 0x2c:{name = hi ? "fminnmv" : "fmaxnmv"; dk = 's'; narr = "4s"; ok = (q && (size & 1) == 0);}break;
    case 0x2f:{name = hi ? "fminv"   : "fmaxv";   dk = 's'; narr = "4s"; ok = (q && (size & 1) == 0);}break;
  }
  if(name != 0 && ok)
  {
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_sreg(ctx, rd, dk);
    arm64_dasm_op_vreg(ctx, rn, narr);
    arm64_dasm_simd_set_regs(ctx, rd, dk, rn, 'v');
    result = 1;
  }
  return result;
}

//- rjf: scalar pairwise

internal B32
arm64_dasm_decode_simd_scalar_pairwise(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 opcode = ARM64_DASM_BITS(inst, 16, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  U32 hi     = (size >> 1) & 1;
  U32 sz     = size & 1;
  char *name = 0;
  char dk = sz ? 'd' : 's';
  char *narr = sz ? "2d" : "2s";
  switch((u << 5) | opcode)
  {
    default:{}break;
    case 0x1b:{if(size == 3) { name = "addp"; dk = 'd'; narr = "2d"; }}break;
    case 0x0c:{name = (hi ? "fminnmp" : "fmaxnmp"); if(sz) { name = 0; } dk = 'h'; narr = "2h";}break;
    case 0x0d:{name = (hi ? 0 : "faddp");           if(sz) { name = 0; } dk = 'h'; narr = "2h";}break;
    case 0x0f:{name = (hi ? "fminp" : "fmaxp");     if(sz) { name = 0; } dk = 'h'; narr = "2h";}break;
    case 0x2c:{name = hi ? "fminnmp" : "fmaxnmp";}break;
    case 0x2d:{name = hi ? 0 : "faddp";}break;
    case 0x2f:{name = hi ? "fminp" : "fmaxp";}break;
  }
  if(name != 0)
  {
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_sreg(ctx, rd, dk);
    arm64_dasm_op_vreg(ctx, rn, narr);
    arm64_dasm_simd_set_regs(ctx, rd, dk, rn, 'v');
    result = 1;
  }
  return result;
}

//- rjf: three different (vector & scalar)

internal B32
arm64_dasm_decode_simd_three_diff(ARM64_DasmCtx *ctx, B32 scalar)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  local_persist char *names[2][16] =
  {
    {"saddl", "saddw", "ssubl", "ssubw", "addhn", "sabal", "subhn", "sabdl", "smlal", "sqdmlal", "smlsl", "sqdmlsl", "smull", "sqdmull", "pmull", 0},
    {"uaddl", "uaddw", "usubl", "usubw", "raddhn", "uabal", "rsubhn", "uabdl", "umlal", 0, "umlsl", 0, "umull", 0, 0, 0},
  };
  char *name = names[u][opcode];
  if(u == 0 && (opcode == 9 || opcode == 11 || opcode == 13) && !(size == 1 || size == 2))
  {
    name = 0; // SQDMLAL / SQDMLSL / SQDMULL: H and S elements only
  }
  if(scalar)
  {
    // SQDMLAL / SQDMLSL / SQDMULL only
    if(u == 0 && (opcode == 9 || opcode == 11 || opcode == 13) && (size == 1 || size == 2))
    {
      char dk = arm64_dasm_kind_from_esz(size + 1);
      char nk = arm64_dasm_kind_from_esz(size);
      arm64_dasm_mnem(ctx, name);
      arm64_dasm_op_sreg(ctx, rd, dk);
      arm64_dasm_op_sreg(ctx, rn, nk);
      arm64_dasm_op_sreg(ctx, rm, nk);
      arm64_dasm_simd_set_regs(ctx, rd, dk, rn, nk);
      result = 1;
    }
  }
  else if(name != 0)
  {
    B32 wide   = (opcode == 1 || opcode == 3);
    B32 narrow = (opcode == 4 || opcode == 6);
    char *darr = 0;
    char *narr = 0;
    char *marr = 0;
    if(opcode == 14)
    {
      // PMULL: 8b/16b -> 8h, 1d/2d -> 1q
      if(size == 0)      { darr = "8h"; narr = marr = arm64_dasm_arr(0, q); }
      else if(size == 3) { darr = "1q"; narr = marr = arm64_dasm_arr(3, q); }
    }
    else if(size < 3)
    {
      if(wide)        { darr = arm64_dasm_arr(size + 1, 1); narr = darr; marr = arm64_dasm_arr(size, q); }
      else if(narrow) { darr = arm64_dasm_arr(size, q); narr = marr = arm64_dasm_arr(size + 1, 1); }
      else            { darr = arm64_dasm_arr(size + 1, 1); narr = marr = arm64_dasm_arr(size, q); }
    }
    if(darr != 0)
    {
      arm64_dasm_mnemf(ctx, "%s%s", name, q ? "2" : "");
      arm64_dasm_op_vreg(ctx, rd, darr);
      arm64_dasm_op_vreg(ctx, rn, narr);
      arm64_dasm_op_vreg(ctx, rm, marr);
      arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
      result = 1;
    }
  }
  return result;
}

//- rjf: copy (DUP / INS / UMOV / SMOV) and scalar copy

// rjf: imm5 -> element size (0..3) and index; returns 0 when imm5 is reserved
internal B32
arm64_dasm_simd_elem_from_imm5(U32 imm5, U32 *esz_out, U32 *index_out)
{
  B32 result = 0;
  for(U32 esz = 0; esz < 4; esz += 1)
  {
    if((imm5 >> esz) & 1)
    {
      *esz_out = esz;
      *index_out = imm5 >> (esz + 1);
      result = 1;
      break;
    }
  }
  return result;
}

internal B32
arm64_dasm_decode_simd_copy(ARM64_DasmCtx *ctx, B32 scalar)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q    = ARM64_DASM_BIT(inst, 30);
  U32 op   = ARM64_DASM_BIT(inst, 29);
  U32 imm5 = ARM64_DASM_BITS(inst, 20, 16);
  U32 imm4 = ARM64_DASM_BITS(inst, 14, 11);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd   = ARM64_DASM_BITS(inst, 4, 0);
  U32 esz = 0;
  U32 index = 0;
  if(arm64_dasm_simd_elem_from_imm5(imm5, &esz, &index))
  {
    char ek = arm64_dasm_kind_from_esz(esz);
    if(scalar)
    {
      // DUP (element, scalar) -> MOV alias
      if(op == 0 && imm4 == 0)
      {
        arm64_dasm_mnem(ctx, "mov");
        arm64_dasm_op_sreg(ctx, rd, ek);
        arm64_dasm_op_velem(ctx, rn, ek, index);
        arm64_dasm_simd_set_regs(ctx, rd, ek, rn, 'v');
        result = 1;
      }
    }
    else if(op == 1)
    {
      // INS (element) -> MOV alias
      if(q == 1)
      {
        U32 index2 = imm4 >> esz;
        arm64_dasm_mnem(ctx, "mov");
        arm64_dasm_op_velem(ctx, rd, ek, index);
        arm64_dasm_op_velem(ctx, rn, ek, index2);
        arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
        result = 1;
      }
    }
    else
    {
      switch(imm4)
      {
        default:{}break;
        case 0x0: // DUP (element)
        {
          if(esz < 3 || q)
          {
            arm64_dasm_mnem(ctx, "dup");
            arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(esz, q));
            arm64_dasm_op_velem(ctx, rn, ek, index);
            arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
            result = 1;
          }
        }break;
        case 0x1: // DUP (general)
        {
          if(esz < 3 || q)
          {
            arm64_dasm_mnem(ctx, "dup");
            arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(esz, q));
            arm64_dasm_op_gpr(ctx, rn, esz == 3, 0);
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, 'v'), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, esz == 3, 0), 0);
            result = 1;
          }
        }break;
        case 0x3: // INS (general) -> MOV alias
        {
          if(q == 1)
          {
            arm64_dasm_mnem(ctx, "mov");
            arm64_dasm_op_velem(ctx, rd, ek, index);
            arm64_dasm_op_gpr(ctx, rn, esz == 3, 0);
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, 'v'), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_gpr_code(rn, esz == 3, 0), 0);
            result = 1;
          }
        }break;
        case 0x5: // SMOV
        {
          if((q == 0 && esz < 2) || (q == 1 && esz < 3))
          {
            arm64_dasm_mnem(ctx, "smov");
            arm64_dasm_op_gpr(ctx, rd, q, 0);
            arm64_dasm_op_velem(ctx, rn, ek, index);
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, q, 0), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, 'v'), 0);
            result = 1;
          }
        }break;
        case 0x7: // UMOV (MOV alias for 32/64-bit elements)
        {
          if((q == 0 && esz < 3) || (q == 1 && esz == 3))
          {
            arm64_dasm_mnem(ctx, (esz >= 2) ? "mov" : "umov");
            arm64_dasm_op_gpr(ctx, rd, q, 0);
            arm64_dasm_op_velem(ctx, rn, ek, index);
            arm64_dasm_set_reg(ctx, 0, arm64_dasm_gpr_code(rd, q, 0), 0);
            arm64_dasm_set_reg(ctx, 1, arm64_dasm_vreg_code(rn, 'v'), 0);
            result = 1;
          }
        }break;
      }
    }
  }
  return result;
}

//- rjf: modified immediate (MOVI / MVNI / ORR / BIC / FMOV)

internal B32
arm64_dasm_decode_simd_mod_imm(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q     = ARM64_DASM_BIT(inst, 30);
  U32 op    = ARM64_DASM_BIT(inst, 29);
  U32 abc   = ARM64_DASM_BITS(inst, 18, 16);
  U32 cmode = ARM64_DASM_BITS(inst, 15, 12);
  U32 o2    = ARM64_DASM_BIT(inst, 11);
  U32 defgh = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd    = ARM64_DASM_BITS(inst, 4, 0);
  U32 imm8  = (abc << 5) | defgh;
  if(o2 == 1)
  {
    // FMOV (vector, half precision immediate)
    if(op == 0 && cmode == 0xf)
    {
      arm64_dasm_mnem(ctx, "fmov");
      arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(1, q));
      arm64_dasm_op_fimm(ctx, arm64_dasm_fp_imm8(imm8));
      result = 1;
    }
  }
  else if(cmode < 8)
  {
    // 32-bit shifted immediate: MOVI/MVNI (cmode 0xx0) or ORR/BIC (cmode 0xx1)
    U32 shift = (cmode >> 1) * 8;
    char *name = (cmode & 1) ? (op ? "bic" : "orr") : (op ? "mvni" : "movi");
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(2, q));
    arm64_dasm_op_imm(ctx, imm8);
    if(shift != 0) { arm64_dasm_op_f(ctx, "lsl #%u", shift); }
    result = 1;
  }
  else if(cmode < 12)
  {
    // 16-bit shifted immediate
    U32 shift = ((cmode >> 1) & 1) * 8;
    char *name = (cmode & 1) ? (op ? "bic" : "orr") : (op ? "mvni" : "movi");
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(1, q));
    arm64_dasm_op_imm(ctx, imm8);
    if(shift != 0) { arm64_dasm_op_f(ctx, "lsl #%u", shift); }
    result = 1;
  }
  else if(cmode < 14)
  {
    // 32-bit shifting ones
    arm64_dasm_mnem(ctx, op ? "mvni" : "movi");
    arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(2, q));
    arm64_dasm_op_imm(ctx, imm8);
    arm64_dasm_op_f(ctx, "msl #%u", (cmode & 1) ? 16 : 8);
    result = 1;
  }
  else if(cmode == 14)
  {
    if(op == 0)
    {
      // MOVI 8-bit
      arm64_dasm_mnem(ctx, "movi");
      arm64_dasm_op_vreg(ctx, rd, arm64_dasm_arr(0, q));
      arm64_dasm_op_imm(ctx, imm8);
    }
    else
    {
      // MOVI 64-bit (scalar D or vector 2D)
      arm64_dasm_mnem(ctx, "movi");
      if(q) { arm64_dasm_op_vreg(ctx, rd, "2d"); }
      else  { arm64_dasm_op_sreg(ctx, rd, 'd'); }
      arm64_dasm_op_immu(ctx, arm64_dasm_simd_imm64(imm8));
    }
    result = 1;
  }
  else
  {
    // FMOV (vector, single / double precision immediate)
    if(op == 0 || q == 1)
    {
      arm64_dasm_mnem(ctx, "fmov");
      arm64_dasm_op_vreg(ctx, rd, op ? "2d" : arm64_dasm_arr(2, q));
      arm64_dasm_op_fimm(ctx, arm64_dasm_fp_imm8(imm8));
      result = 1;
    }
  }
  if(result)
  {
    arm64_dasm_set_reg(ctx, 0, arm64_dasm_vreg_code(rd, 'v'), 0);
  }
  return result;
}

//- rjf: shift by immediate (vector & scalar)

internal B32
arm64_dasm_decode_simd_shift_imm(ARM64_DasmCtx *ctx, B32 scalar)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 immh   = ARM64_DASM_BITS(inst, 22, 19);
  U32 immb   = ARM64_DASM_BITS(inst, 18, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 11);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  U32 esz    = (immh & 8) ? 3 : (immh & 4) ? 2 : (immh & 2) ? 1 : 0;
  U32 esize  = 8u << esz;
  U32 immhb  = (immh << 3) | immb;
  U32 rshift = 2 * esize - immhb;
  U32 lshift = immhb - esize;
  typedef enum ShiftForm { Form_Right, Form_Left, Form_Narrow, Form_Long, Form_Fixed } ShiftForm;
  char *name = 0;
  ShiftForm form = Form_Right;
  switch((u << 5) | opcode)
  {
    default:{}break;
    case 0x00:{name = "sshr";}break;
    case 0x02:{name = "ssra";}break;
    case 0x04:{name = "srshr";}break;
    case 0x06:{name = "srsra";}break;
    case 0x0a:{name = "shl";     form = Form_Left;}break;
    case 0x0e:{name = "sqshl";   form = Form_Left;}break;
    case 0x10:{name = "shrn";    form = Form_Narrow;}break;
    case 0x11:{name = "rshrn";   form = Form_Narrow;}break;
    case 0x12:{name = "sqshrn";  form = Form_Narrow;}break;
    case 0x13:{name = "sqrshrn"; form = Form_Narrow;}break;
    case 0x14:{name = "sshll";   form = Form_Long;}break;
    case 0x1c:{name = "scvtf";   form = Form_Fixed;}break;
    case 0x1f:{name = "fcvtzs";  form = Form_Fixed;}break;
    case 0x20:{name = "ushr";}break;
    case 0x22:{name = "usra";}break;
    case 0x24:{name = "urshr";}break;
    case 0x26:{name = "ursra";}break;
    case 0x28:{name = "sri";}break;
    case 0x2a:{name = "sli";     form = Form_Left;}break;
    case 0x2c:{name = "sqshlu";  form = Form_Left;}break;
    case 0x2e:{name = "uqshl";   form = Form_Left;}break;
    case 0x30:{name = "sqshrun"; form = Form_Narrow;}break;
    case 0x31:{name = "sqrshrun";form = Form_Narrow;}break;
    case 0x32:{name = "uqshrn";  form = Form_Narrow;}break;
    case 0x33:{name = "uqrshrn"; form = Form_Narrow;}break;
    case 0x34:{name = "ushll";   form = Form_Long;}break;
    case 0x3c:{name = "ucvtf";   form = Form_Fixed;}break;
    case 0x3f:{name = "fcvtzu";  form = Form_Fixed;}break;
  }
  if(name != 0 && immh != 0)
  {
    if(scalar)
    {
      char dk = arm64_dasm_kind_from_esz(esz);
      char nk = dk;
      U32 amount = rshift;
      B32 ok = 1;
      // saturating forms exist for all sizes; plain shifts are D only; SHRN/RSHRN have no scalar form
      B32 saturating = (opcode == 0x0e || opcode == 0x0c || (opcode >= 0x12 && opcode <= 0x13) || (u && opcode >= 0x10 && opcode <= 0x11));
      switch(form)
      {
        default:         {if(esz != 3) { ok = 0; }}break;
        case Form_Left:  {amount = lshift; if(!saturating && esz != 3) { ok = 0; }}break;
        case Form_Narrow:{if(esz < 3 && saturating) { nk = arm64_dasm_kind_from_esz(esz + 1); } else { ok = 0; }}break;
        case Form_Long:  {ok = 0;}break;
        case Form_Fixed: {if(esz == 0) { ok = 0; }}break;
      }
      if(ok)
      {
        arm64_dasm_mnem(ctx, name);
        arm64_dasm_op_sreg(ctx, rd, dk);
        arm64_dasm_op_sreg(ctx, rn, nk);
        arm64_dasm_op_dec(ctx, amount);
        arm64_dasm_simd_set_regs(ctx, rd, dk, rn, nk);
        result = 1;
      }
    }
    else
    {
      char *darr = arm64_dasm_arr(esz, q);
      char *narr = darr;
      char *suffix = "";
      U32 amount = rshift;
      B32 ok = (esz < 3 || q);
      B32 print_amount = 1;
      switch(form)
      {
        default:{}break;
        case Form_Left:{amount = lshift;}break;
        case Form_Narrow:
        {
          if(esz < 3) { darr = arm64_dasm_arr(esz, q); narr = arm64_dasm_arr(esz + 1, 1); suffix = q ? "2" : ""; ok = 1; }
          else        { ok = 0; }
        }break;
        case Form_Long:
        {
          if(esz < 3)
          {
            darr = arm64_dasm_arr(esz + 1, 1);
            narr = arm64_dasm_arr(esz, q);
            suffix = q ? "2" : "";
            amount = lshift;
            ok = 1;
            if(amount == 0)
            {
              name = u ? "uxtl" : "sxtl";
              print_amount = 0;
            }
          }
          else { ok = 0; }
        }break;
        case Form_Fixed:{if(esz == 0) { ok = 0; }}break;
      }
      if(ok)
      {
        arm64_dasm_mnemf(ctx, "%s%s", name, suffix);
        arm64_dasm_op_vreg(ctx, rd, darr);
        arm64_dasm_op_vreg(ctx, rn, narr);
        if(print_amount) { arm64_dasm_op_dec(ctx, amount); }
        arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
        result = 1;
      }
    }
  }
  return result;
}

//- rjf: vector/scalar x indexed element

internal B32
arm64_dasm_decode_simd_indexed(ARM64_DasmCtx *ctx, B32 scalar)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 l      = ARM64_DASM_BIT(inst, 21);
  U32 m      = ARM64_DASM_BIT(inst, 20);
  U32 rm     = ARM64_DASM_BITS(inst, 19, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 15, 12);
  U32 h      = ARM64_DASM_BIT(inst, 11);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  typedef enum IdxForm { Form_Same, Form_Long, Form_Fp, Form_Dot } IdxForm;
  char *name = 0;
  IdxForm form = Form_Same;
  switch((u << 4) | opcode)
  {
    default:{}break;
    case 0x01:{name = "fmla";     form = Form_Fp;}break;
    case 0x02:{name = "smlal";    form = Form_Long;}break;
    case 0x03:{name = "sqdmlal";  form = Form_Long;}break;
    case 0x05:{name = "fmls";     form = Form_Fp;}break;
    case 0x06:{name = "smlsl";    form = Form_Long;}break;
    case 0x07:{name = "sqdmlsl";  form = Form_Long;}break;
    case 0x08:{name = "mul";}break;
    case 0x09:{name = "fmul";     form = Form_Fp;}break;
    case 0x0a:{name = "smull";    form = Form_Long;}break;
    case 0x0b:{name = "sqdmull";  form = Form_Long;}break;
    case 0x0c:{name = "sqdmulh";}break;
    case 0x0d:{name = "sqrdmulh";}break;
    case 0x0e:{name = "sdot";     form = Form_Dot;}break;
    case 0x10:{name = "mla";}break;
    case 0x12:{name = "umlal";    form = Form_Long;}break;
    case 0x14:{name = "mls";}break;
    case 0x16:{name = "umlsl";    form = Form_Long;}break;
    case 0x19:{name = "fmulx";    form = Form_Fp;}break;
    case 0x1a:{name = "umull";    form = Form_Long;}break;
    case 0x1d:{name = "sqrdmlah";}break;
    case 0x1e:{name = "udot";     form = Form_Dot;}break;
    case 0x1f:{name = "sqrdmlsh";}break;
  }
  if(name != 0)
  {
    // element kind, index & Vm per element size
    U32 esz = 0;
    U32 index = 0;
    U32 vm = 0;
    B32 ok = 1;
    if(form == Form_Fp)
    {
      if(size == 0)      { esz = 1; index = (h << 2) | (l << 1) | m; vm = rm; }
      else if(size == 2) { esz = 2; index = (h << 1) | l; vm = (m << 4) | rm; }
      else if(size == 3) { esz = 3; index = h; vm = (m << 4) | rm; if(l) { ok = 0; } }
      else               { ok = 0; }
    }
    else if(form == Form_Dot)
    {
      if(size == 2) { esz = 2; index = (h << 1) | l; vm = (m << 4) | rm; }
      else          { ok = 0; }
    }
    else
    {
      if(size == 1)      { esz = 1; index = (h << 2) | (l << 1) | m; vm = rm; }
      else if(size == 2) { esz = 2; index = (h << 1) | l; vm = (m << 4) | rm; }
      else               { ok = 0; }
    }
    if(ok)
    {
      char ek = arm64_dasm_kind_from_esz(esz);
      if(scalar)
      {
        char dk = ek;
        char nk = ek;
        if(form == Form_Long) { dk = arm64_dasm_kind_from_esz(esz + 1); }
        if(form == Form_Dot)  { ok = 0; }
        // scalar forms: SQDMLAL/SQDMLSL/SQDMULL/SQDMULH/SQRDMULH/SQRDMLAH/SQRDMLSH + FP
        {
          U32 key = (u << 4) | opcode;
          if(!(key == 0x03 || key == 0x07 || key == 0x0b || key == 0x0c || key == 0x0d || key == 0x1d || key == 0x1f || form == Form_Fp))
          {
            ok = 0;
          }
        }
        if(ok)
        {
          arm64_dasm_mnem(ctx, name);
          arm64_dasm_op_sreg(ctx, rd, dk);
          arm64_dasm_op_sreg(ctx, rn, nk);
          arm64_dasm_op_velem(ctx, vm, ek, index);
          arm64_dasm_simd_set_regs(ctx, rd, dk, rn, nk);
          result = 1;
        }
      }
      else
      {
        char *darr = 0;
        char *narr = 0;
        char *suffix = "";
        switch(form)
        {
          default:
          case Form_Same:{darr = narr = arm64_dasm_arr(esz, q);}break;
          case Form_Fp:  {darr = narr = arm64_dasm_arr(esz, q);}break;
          case Form_Long:{darr = arm64_dasm_arr(esz + 1, 1); narr = arm64_dasm_arr(esz, q); suffix = q ? "2" : "";}break;
          case Form_Dot: {darr = arm64_dasm_arr(2, q); narr = arm64_dasm_arr(0, q); ek = 'b';}break;
        }
        if(form == Form_Fp && esz == 3 && q == 0)
        {
          // no 1d arrangement
        }
        else
        {
          arm64_dasm_mnemf(ctx, "%s%s", name, suffix);
          arm64_dasm_op_vreg(ctx, rd, darr);
          arm64_dasm_op_vreg(ctx, rn, narr);
          if(form == Form_Dot) { arm64_dasm_op(ctx); arm64_dasm_putf(ctx, "v%u.4b[%u]", vm, index); }
          else                 { arm64_dasm_op_velem(ctx, vm, ek, index); }
          arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
          result = 1;
        }
      }
    }
  }
  return result;
}

//- rjf: extract, permute, table lookup, three-register extension

internal B32
arm64_dasm_decode_simd_ext(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q    = ARM64_DASM_BIT(inst, 30);
  U32 rm   = ARM64_DASM_BITS(inst, 20, 16);
  U32 imm4 = ARM64_DASM_BITS(inst, 14, 11);
  U32 rn   = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd   = ARM64_DASM_BITS(inst, 4, 0);
  if(q || (imm4 & 8) == 0)
  {
    char *arr = arm64_dasm_arr(0, q);
    arm64_dasm_mnem(ctx, "ext");
    arm64_dasm_op_vreg(ctx, rd, arr);
    arm64_dasm_op_vreg(ctx, rn, arr);
    arm64_dasm_op_vreg(ctx, rm, arr);
    arm64_dasm_op_dec(ctx, imm4);
    arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
    result = 1;
  }
  return result;
}

internal B32
arm64_dasm_decode_simd_permute(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 14, 12);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  local_persist char *names[8] = {0, "uzp1", "trn1", "zip1", 0, "uzp2", "trn2", "zip2"};
  if(names[opcode] != 0 && (size < 3 || q))
  {
    char *arr = arm64_dasm_arr(size, q);
    arm64_dasm_mnem(ctx, names[opcode]);
    arm64_dasm_op_vreg(ctx, rd, arr);
    arm64_dasm_op_vreg(ctx, rn, arr);
    arm64_dasm_op_vreg(ctx, rm, arr);
    arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
    result = 1;
  }
  return result;
}

internal B32
arm64_dasm_decode_simd_tbl(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q   = ARM64_DASM_BIT(inst, 30);
  U32 op2 = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm  = ARM64_DASM_BITS(inst, 20, 16);
  U32 len = ARM64_DASM_BITS(inst, 14, 13);
  U32 op  = ARM64_DASM_BIT(inst, 12);
  U32 rn  = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd  = ARM64_DASM_BITS(inst, 4, 0);
  if(op2 == 0)
  {
    char *arr = arm64_dasm_arr(0, q);
    arm64_dasm_mnem(ctx, op ? "tbx" : "tbl");
    arm64_dasm_op_vreg(ctx, rd, arr);
    arm64_dasm_op_vlist(ctx, rn, len + 1, "16b");
    arm64_dasm_op_vreg(ctx, rm, arr);
    arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
    result = 1;
  }
  return result;
}

internal B32
arm64_dasm_decode_simd_three_reg_ext(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 q      = ARM64_DASM_BIT(inst, 30);
  U32 u      = ARM64_DASM_BIT(inst, 29);
  U32 size   = ARM64_DASM_BITS(inst, 23, 22);
  U32 rm     = ARM64_DASM_BITS(inst, 20, 16);
  U32 opcode = ARM64_DASM_BITS(inst, 14, 11);
  U32 rn     = ARM64_DASM_BITS(inst, 9, 5);
  U32 rd     = ARM64_DASM_BITS(inst, 4, 0);
  char *name = 0;
  char *darr = 0;
  char *narr = 0;
  U32 rot = 0;
  B32 has_rot = 0;
  if(u == 0 && (opcode == 2 || opcode == 3) && size == 2)
  {
    name = (opcode == 2) ? "sdot" : "usdot";
    darr = arm64_dasm_arr(2, q);
    narr = arm64_dasm_arr(0, q);
  }
  else if(u == 1 && opcode == 2 && size == 2)
  {
    name = "udot";
    darr = arm64_dasm_arr(2, q);
    narr = arm64_dasm_arr(0, q);
  }
  else if(u == 1 && (opcode == 0 || opcode == 1) && (size == 1 || size == 2))
  {
    name = (opcode == 0) ? "sqrdmlah" : "sqrdmlsh";
    darr = narr = arm64_dasm_arr(size, q);
  }
  else if(u == 1 && (opcode & 0xc) == 0x8 && size != 0 && (size < 3 || q))
  {
    name = "fcmla";
    darr = narr = arm64_dasm_arr(size, q);
    rot = (opcode & 3) * 90;
    has_rot = 1;
  }
  else if(u == 1 && (opcode & 0xd) == 0xc && size != 0 && (size < 3 || q))
  {
    name = "fcadd";
    darr = narr = arm64_dasm_arr(size, q);
    rot = (opcode & 2) ? 270 : 90;
    has_rot = 1;
  }
  if(name != 0)
  {
    arm64_dasm_mnem(ctx, name);
    arm64_dasm_op_vreg(ctx, rd, darr);
    arm64_dasm_op_vreg(ctx, rn, narr);
    arm64_dasm_op_vreg(ctx, rm, narr);
    if(has_rot) { arm64_dasm_op_dec(ctx, rot); }
    arm64_dasm_simd_set_regs(ctx, rd, 'v', rn, 'v');
    result = 1;
  }
  return result;
}

//- rjf: SIMD top-level (vector when bit 28 == 0, scalar when bits 30 & 28 are set)

internal B32
arm64_dasm_decode_simd(ARM64_DasmCtx *ctx, B32 scalar)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 b_24 = ARM64_DASM_BIT(inst, 24);
  U32 b_23 = ARM64_DASM_BIT(inst, 23);
  U32 b_21 = ARM64_DASM_BIT(inst, 21);
  U32 b_15 = ARM64_DASM_BIT(inst, 15);
  U32 b_10 = ARM64_DASM_BIT(inst, 10);
  U32 b1110 = ARM64_DASM_BITS(inst, 11, 10);
  U32 u     = ARM64_DASM_BIT(inst, 29);
  if(b_24)
  {
    if(b_10 == 0)
    {
      result = arm64_dasm_decode_simd_indexed(ctx, scalar);
    }
    else if(b_23 == 0)
    {
      if(ARM64_DASM_BITS(inst, 22, 19) == 0)
      {
        if(!scalar) { result = arm64_dasm_decode_simd_mod_imm(ctx); }
      }
      else
      {
        result = arm64_dasm_decode_simd_shift_imm(ctx, scalar);
      }
    }
  }
  else if(b_21)
  {
    switch(b1110)
    {
      case 0:{result = arm64_dasm_decode_simd_three_diff(ctx, scalar);}break;
      case 1:
      case 3:{result = arm64_dasm_decode_simd_three_same(ctx, scalar, 0);}break;
      case 2:
      {
        // bits [21:17]: 10000 two-register misc, 11000 across lanes / scalar pairwise,
        // 1111 00 (bits [22:17]) two-register misc (FP16)
        U32 sel = ARM64_DASM_BITS(inst, 20, 17);
        if(sel == 0x0)      { result = arm64_dasm_decode_simd_two_reg_misc(ctx, scalar, 0); }
        else if(sel == 0x8) { result = scalar ? arm64_dasm_decode_simd_scalar_pairwise(ctx) : arm64_dasm_decode_simd_across_lanes(ctx); }
        else if(sel == 0xc && ARM64_DASM_BIT(inst, 22) == 1) { result = arm64_dasm_decode_simd_two_reg_misc(ctx, scalar, 1); }
      }break;
    }
  }
  else
  {
    if(b_23 == 0 && ARM64_DASM_BITS(inst, 22, 21) == 0 && b_15 == 0 && b_10 == 1)
    {
      result = arm64_dasm_decode_simd_copy(ctx, scalar);
    }
    else if(ARM64_DASM_BITS(inst, 22, 21) == 2 && ARM64_DASM_BITS(inst, 15, 14) == 0 && b_10 == 1)
    {
      result = arm64_dasm_decode_simd_three_same(ctx, scalar, 1);
    }
    else if(b_15 == 1 && b_10 == 1)
    {
      if(!scalar) { result = arm64_dasm_decode_simd_three_reg_ext(ctx); }
    }
    else if(!scalar)
    {
      if(u == 0 && b_15 == 0 && b1110 == 0)      { result = arm64_dasm_decode_simd_tbl(ctx); }
      else if(u == 0 && b_15 == 0 && b1110 == 2) { result = arm64_dasm_decode_simd_permute(ctx); }
      else if(u == 1 && b_15 == 0 && b_10 == 0 && ARM64_DASM_BITS(inst, 23, 22) == 0) { result = arm64_dasm_decode_simd_ext(ctx); }
    }
  }
  return result;
}

//- rjf: scalar FP & advanced SIMD top-level

internal B32
arm64_dasm_decode_fp_simd(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 b_31 = ARM64_DASM_BIT(inst, 31);
  U32 b_30 = ARM64_DASM_BIT(inst, 30);
  U32 b_28 = ARM64_DASM_BIT(inst, 28);
  if(b_30 == 0 && b_28 == 1)
  {
    result = arm64_dasm_decode_fp(ctx);
  }
  else if(b_31 == 0 && b_28 == 0)
  {
    result = arm64_dasm_decode_simd(ctx, 0);
  }
  else if(b_31 == 0 && b_30 == 1 && b_28 == 1)
  {
    result = arm64_dasm_decode_simd(ctx, 1);
  }
  return result;
}

////////////////////////////////
//~ rjf: Top-Level Decode

internal B32
arm64_dasm_decode(ARM64_DasmCtx *ctx)
{
  B32 result = 0;
  U32 inst = ctx->inst;
  U32 op0 = ARM64_DASM_BITS(inst, 28, 25);
  switch(op0)
  {
    default:{}break;

    //- rjf: reserved (UDF)
    case 0x0:
    {
      if(ARM64_DASM_BITS(inst, 31, 16) == 0)
      {
        arm64_dasm_mnem(ctx, "udf");
        arm64_dasm_op_immu(ctx, ARM64_DASM_BITS(inst, 15, 0));
        result = 1;
      }
    }break;

    //- rjf: SME / SVE / unallocated
    case 0x1:
    case 0x2:
    case 0x3:
    {
    }break;

    //- rjf: data processing (immediate)
    case 0x8:
    case 0x9:
    {
      result = arm64_dasm_decode_dp_imm(ctx);
    }break;

    //- rjf: branches, exception generating & system instructions
    case 0xa:
    case 0xb:
    {
      result = arm64_dasm_decode_branch_sys(ctx);
    }break;

    //- rjf: loads & stores
    case 0x4:
    case 0x6:
    case 0xc:
    case 0xe:
    {
      result = arm64_dasm_decode_ldst(ctx);
    }break;

    //- rjf: data processing (register)
    case 0x5:
    case 0xd:
    {
      result = arm64_dasm_decode_dp_reg(ctx);
    }break;

    //- rjf: data processing (scalar floating-point & advanced SIMD)
    case 0x7:
    case 0xf:
    {
      result = arm64_dasm_decode_fp_simd(ctx);
    }break;
  }
  return result;
}

////////////////////////////////
//~ rjf: Entry Point

internal DASM_Inst
arm64_dasm_inst_from_code(Arena *arena, U64 vaddr, String8 code, DASM_Syntax syntax)
{
  DASM_Inst inst = {0};
  (void)syntax; // NOTE(rjf): only ARM syntax is produced

  //////////////////////////////
  //- rjf: not enough bytes for a full instruction
  //
  if(code.size < 4)
  {
    inst.size = (U32)(code.size != 0 ? code.size : 1);
    inst.string = push_str8_copy(arena, str8_lit("???"));
    return inst;
  }

  //////////////////////////////
  //- rjf: fetch the little-endian instruction word
  //
  U32 word = ((U32)code.str[0] |
              ((U32)code.str[1] << 8) |
              ((U32)code.str[2] << 16) |
              ((U32)code.str[3] << 24));

  //////////////////////////////
  //- rjf: decode
  //
  ARM64_DasmCtx ctx = {0};
  ctx.inst = word;
  ctx.vaddr = vaddr;
  B32 decoded = arm64_dasm_decode(&ctx);
  if(!decoded)
  {
    ctx.flags = 0;
    ctx.dst_vaddr = 0;
    ctx.reg_codes[0] = ctx.reg_codes[1] = 0;
    ctx.reg_offs[0] = ctx.reg_offs[1] = 0;
    ctx.len = 0;
    ctx.buf[0] = 0;
    arm64_dasm_putf(&ctx, ".inst 0x%08x", word);
  }
  else if(!(ctx.flags & (DASM_InstFlag_Call|DASM_InstFlag_Branch|DASM_InstFlag_UnconditionalJump|DASM_InstFlag_Return)))
  {
    ctx.flags |= DASM_InstFlag_NonFlow;
  }

  //////////////////////////////
  //- rjf: bundle
  //
  inst.flags        = ctx.flags;
  inst.size         = 4;
  inst.string       = push_str8_copy(arena, str8((U8 *)ctx.buf, ctx.len));
  inst.dst_vaddr    = ctx.dst_vaddr;
  inst.dst_reg_code = ctx.reg_codes[0];
  inst.dst_reg_off  = ctx.reg_offs[0];
  inst.src_reg_code = ctx.reg_codes[1];
  inst.src_reg_off  = ctx.reg_offs[1];
  return inst;
}
