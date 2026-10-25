// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

//- GENERATED CODE

#ifndef ARM64_META_H
#define ARM64_META_H

typedef enum ARM64_RegCode
{
ARM64_RegCode_nil,
ARM64_RegCode_x0,
ARM64_RegCode_x1,
ARM64_RegCode_x2,
ARM64_RegCode_x3,
ARM64_RegCode_x4,
ARM64_RegCode_x5,
ARM64_RegCode_x6,
ARM64_RegCode_x7,
ARM64_RegCode_x8,
ARM64_RegCode_x9,
ARM64_RegCode_x10,
ARM64_RegCode_x11,
ARM64_RegCode_x12,
ARM64_RegCode_x13,
ARM64_RegCode_x14,
ARM64_RegCode_x15,
ARM64_RegCode_x16,
ARM64_RegCode_x17,
ARM64_RegCode_x18,
ARM64_RegCode_x19,
ARM64_RegCode_x20,
ARM64_RegCode_x21,
ARM64_RegCode_x22,
ARM64_RegCode_x23,
ARM64_RegCode_x24,
ARM64_RegCode_x25,
ARM64_RegCode_x26,
ARM64_RegCode_x27,
ARM64_RegCode_x28,
ARM64_RegCode_fp,
ARM64_RegCode_lr,
ARM64_RegCode_sp,
ARM64_RegCode_pc,
ARM64_RegCode_cpsr,
ARM64_RegCode_fpsr,
ARM64_RegCode_fpcr,
ARM64_RegCode_tpidrro_el0,
ARM64_RegCode_tpidr_el0,
ARM64_RegCode_v0,
ARM64_RegCode_v1,
ARM64_RegCode_v2,
ARM64_RegCode_v3,
ARM64_RegCode_v4,
ARM64_RegCode_v5,
ARM64_RegCode_v6,
ARM64_RegCode_v7,
ARM64_RegCode_v8,
ARM64_RegCode_v9,
ARM64_RegCode_v10,
ARM64_RegCode_v11,
ARM64_RegCode_v12,
ARM64_RegCode_v13,
ARM64_RegCode_v14,
ARM64_RegCode_v15,
ARM64_RegCode_v16,
ARM64_RegCode_v17,
ARM64_RegCode_v18,
ARM64_RegCode_v19,
ARM64_RegCode_v20,
ARM64_RegCode_v21,
ARM64_RegCode_v22,
ARM64_RegCode_v23,
ARM64_RegCode_v24,
ARM64_RegCode_v25,
ARM64_RegCode_v26,
ARM64_RegCode_v27,
ARM64_RegCode_v28,
ARM64_RegCode_v29,
ARM64_RegCode_v30,
ARM64_RegCode_v31,
ARM64_RegCode_x29,
ARM64_RegCode_x30,
ARM64_RegCode_w0,
ARM64_RegCode_w1,
ARM64_RegCode_w2,
ARM64_RegCode_w3,
ARM64_RegCode_w4,
ARM64_RegCode_w5,
ARM64_RegCode_w6,
ARM64_RegCode_w7,
ARM64_RegCode_w8,
ARM64_RegCode_w9,
ARM64_RegCode_w10,
ARM64_RegCode_w11,
ARM64_RegCode_w12,
ARM64_RegCode_w13,
ARM64_RegCode_w14,
ARM64_RegCode_w15,
ARM64_RegCode_w16,
ARM64_RegCode_w17,
ARM64_RegCode_w18,
ARM64_RegCode_w19,
ARM64_RegCode_w20,
ARM64_RegCode_w21,
ARM64_RegCode_w22,
ARM64_RegCode_w23,
ARM64_RegCode_w24,
ARM64_RegCode_w25,
ARM64_RegCode_w26,
ARM64_RegCode_w27,
ARM64_RegCode_w28,
ARM64_RegCode_w29,
ARM64_RegCode_w30,
ARM64_RegCode_wsp,
ARM64_RegCode_q0,
ARM64_RegCode_q1,
ARM64_RegCode_q2,
ARM64_RegCode_q3,
ARM64_RegCode_q4,
ARM64_RegCode_q5,
ARM64_RegCode_q6,
ARM64_RegCode_q7,
ARM64_RegCode_q8,
ARM64_RegCode_q9,
ARM64_RegCode_q10,
ARM64_RegCode_q11,
ARM64_RegCode_q12,
ARM64_RegCode_q13,
ARM64_RegCode_q14,
ARM64_RegCode_q15,
ARM64_RegCode_q16,
ARM64_RegCode_q17,
ARM64_RegCode_q18,
ARM64_RegCode_q19,
ARM64_RegCode_q20,
ARM64_RegCode_q21,
ARM64_RegCode_q22,
ARM64_RegCode_q23,
ARM64_RegCode_q24,
ARM64_RegCode_q25,
ARM64_RegCode_q26,
ARM64_RegCode_q27,
ARM64_RegCode_q28,
ARM64_RegCode_q29,
ARM64_RegCode_q30,
ARM64_RegCode_q31,
ARM64_RegCode_d0,
ARM64_RegCode_d1,
ARM64_RegCode_d2,
ARM64_RegCode_d3,
ARM64_RegCode_d4,
ARM64_RegCode_d5,
ARM64_RegCode_d6,
ARM64_RegCode_d7,
ARM64_RegCode_d8,
ARM64_RegCode_d9,
ARM64_RegCode_d10,
ARM64_RegCode_d11,
ARM64_RegCode_d12,
ARM64_RegCode_d13,
ARM64_RegCode_d14,
ARM64_RegCode_d15,
ARM64_RegCode_d16,
ARM64_RegCode_d17,
ARM64_RegCode_d18,
ARM64_RegCode_d19,
ARM64_RegCode_d20,
ARM64_RegCode_d21,
ARM64_RegCode_d22,
ARM64_RegCode_d23,
ARM64_RegCode_d24,
ARM64_RegCode_d25,
ARM64_RegCode_d26,
ARM64_RegCode_d27,
ARM64_RegCode_d28,
ARM64_RegCode_d29,
ARM64_RegCode_d30,
ARM64_RegCode_d31,
ARM64_RegCode_s0,
ARM64_RegCode_s1,
ARM64_RegCode_s2,
ARM64_RegCode_s3,
ARM64_RegCode_s4,
ARM64_RegCode_s5,
ARM64_RegCode_s6,
ARM64_RegCode_s7,
ARM64_RegCode_s8,
ARM64_RegCode_s9,
ARM64_RegCode_s10,
ARM64_RegCode_s11,
ARM64_RegCode_s12,
ARM64_RegCode_s13,
ARM64_RegCode_s14,
ARM64_RegCode_s15,
ARM64_RegCode_s16,
ARM64_RegCode_s17,
ARM64_RegCode_s18,
ARM64_RegCode_s19,
ARM64_RegCode_s20,
ARM64_RegCode_s21,
ARM64_RegCode_s22,
ARM64_RegCode_s23,
ARM64_RegCode_s24,
ARM64_RegCode_s25,
ARM64_RegCode_s26,
ARM64_RegCode_s27,
ARM64_RegCode_s28,
ARM64_RegCode_s29,
ARM64_RegCode_s30,
ARM64_RegCode_s31,
ARM64_RegCode_h0,
ARM64_RegCode_h1,
ARM64_RegCode_h2,
ARM64_RegCode_h3,
ARM64_RegCode_h4,
ARM64_RegCode_h5,
ARM64_RegCode_h6,
ARM64_RegCode_h7,
ARM64_RegCode_h8,
ARM64_RegCode_h9,
ARM64_RegCode_h10,
ARM64_RegCode_h11,
ARM64_RegCode_h12,
ARM64_RegCode_h13,
ARM64_RegCode_h14,
ARM64_RegCode_h15,
ARM64_RegCode_h16,
ARM64_RegCode_h17,
ARM64_RegCode_h18,
ARM64_RegCode_h19,
ARM64_RegCode_h20,
ARM64_RegCode_h21,
ARM64_RegCode_h22,
ARM64_RegCode_h23,
ARM64_RegCode_h24,
ARM64_RegCode_h25,
ARM64_RegCode_h26,
ARM64_RegCode_h27,
ARM64_RegCode_h28,
ARM64_RegCode_h29,
ARM64_RegCode_h30,
ARM64_RegCode_h31,
ARM64_RegCode_b0,
ARM64_RegCode_b1,
ARM64_RegCode_b2,
ARM64_RegCode_b3,
ARM64_RegCode_b4,
ARM64_RegCode_b5,
ARM64_RegCode_b6,
ARM64_RegCode_b7,
ARM64_RegCode_b8,
ARM64_RegCode_b9,
ARM64_RegCode_b10,
ARM64_RegCode_b11,
ARM64_RegCode_b12,
ARM64_RegCode_b13,
ARM64_RegCode_b14,
ARM64_RegCode_b15,
ARM64_RegCode_b16,
ARM64_RegCode_b17,
ARM64_RegCode_b18,
ARM64_RegCode_b19,
ARM64_RegCode_b20,
ARM64_RegCode_b21,
ARM64_RegCode_b22,
ARM64_RegCode_b23,
ARM64_RegCode_b24,
ARM64_RegCode_b25,
ARM64_RegCode_b26,
ARM64_RegCode_b27,
ARM64_RegCode_b28,
ARM64_RegCode_b29,
ARM64_RegCode_b30,
ARM64_RegCode_b31,
ARM64_RegCode_COUNT,
} ARM64_RegCode;

typedef struct ARM64_RegBlock ARM64_RegBlock;
struct ARM64_RegBlock
{
U64 x0;
U64 x1;
U64 x2;
U64 x3;
U64 x4;
U64 x5;
U64 x6;
U64 x7;
U64 x8;
U64 x9;
U64 x10;
U64 x11;
U64 x12;
U64 x13;
U64 x14;
U64 x15;
U64 x16;
U64 x17;
U64 x18;
U64 x19;
U64 x20;
U64 x21;
U64 x22;
U64 x23;
U64 x24;
U64 x25;
U64 x26;
U64 x27;
U64 x28;
U64 fp;
U64 lr;
U64 sp;
U64 pc;
U32 cpsr;
U32 fpsr;
U32 fpcr;
U64 tpidrro_el0;
U64 tpidr_el0;
U128 v0;
U128 v1;
U128 v2;
U128 v3;
U128 v4;
U128 v5;
U128 v6;
U128 v7;
U128 v8;
U128 v9;
U128 v10;
U128 v11;
U128 v12;
U128 v13;
U128 v14;
U128 v15;
U128 v16;
U128 v17;
U128 v18;
U128 v19;
U128 v20;
U128 v21;
U128 v22;
U128 v23;
U128 v24;
U128 v25;
U128 v26;
U128 v27;
U128 v28;
U128 v29;
U128 v30;
U128 v31;
};

C_LINKAGE_BEGIN
extern B8 arm64_reg_code_is_vector_table[265];
extern String8 arm64_reg_code_name_table[265];
extern U8 arm64_reg_code_base_table[265];
extern Rng1U16 arm64_reg_code_rng_table[265];

C_LINKAGE_END

#endif // ARM64_META_H
