// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal MAC_PLAT_Font *
fp_ct_font_from_handle(FP_Handle handle)
{
  MAC_PLAT_Font *result = (MAC_PLAT_Font *)handle.u64[0];
  return result;
}

internal FP_Handle
fp_ct_handle_from_font(MAC_PLAT_Font *font)
{
  FP_Handle result = {0};
  result.u64[0] = (U64)font;
  return result;
}

internal void *
fp_ct_arena_alloc(void *user, size_t size)
{
  Arena *arena = (Arena *)user;
  void *result = push_array_no_zero(arena, U8, size);
  return result;
}

////////////////////////////////
//~ rjf: Backend Implementations

fp_hook void
fp_init(void)
{
  Arena *arena = arena_alloc();
  fp_ct_state = push_array(arena, FP_CT_State, 1);
  fp_ct_state->arena = arena;
}

fp_hook FP_Handle
fp_font_open(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8 path_copy = push_str8_copy(scratch.arena, path);
  MAC_PLAT_Font *font = mac_plat_font_open_from_path((char *)path_copy.str);
  FP_Handle handle = fp_ct_handle_from_font(font);
  scratch_end(scratch);
  return handle;
}

fp_hook FP_Handle
fp_font_open_from_static_data_string(String8 *data_ptr)
{
  // rjf: the data is static, so the platform layer wraps it without copying.
  MAC_PLAT_Font *font = mac_plat_font_open_from_memory(data_ptr->str, (size_t)data_ptr->size);
  FP_Handle handle = fp_ct_handle_from_font(font);
  return handle;
}

fp_hook void
fp_font_close(FP_Handle handle)
{
  MAC_PLAT_Font *font = fp_ct_font_from_handle(handle);
  if(font != 0)
  {
    mac_plat_font_close(font);
  }
}

fp_hook FP_Metrics
fp_metrics_from_font(FP_Handle handle)
{
  MAC_PLAT_Font *font = fp_ct_font_from_handle(handle);
  FP_Metrics result = {0};
  if(font != 0)
  {
    MAC_PLAT_FontMetrics metrics = {0};
    if(mac_plat_font_get_metrics(font, &metrics))
    {
      result.design_units_per_em = metrics.units_per_em;
      result.ascent              = metrics.ascent;
      result.descent             = metrics.descent;
      result.line_gap            = metrics.line_gap;
      result.capital_height      = metrics.cap_height;
    }
  }
  return result;
}

fp_hook FP_RasterResult
fp_raster(Arena *arena, FP_Handle handle, F32 size, FP_RasterFlags flags, String8 string)
{
  ProfBeginFunction();
  MAC_PLAT_Font *font = fp_ct_font_from_handle(handle);
  FP_RasterResult result = {0};
  if(font != 0 && size > 0)
  {
    Temp scratch = scratch_begin(&arena, 1);

    //- rjf: unpack string
    String32 string32 = str32_from_8(scratch.arena, string);

    //- rjf: map flags
    MAC_PLAT_RasterFlags plat_flags = 0;
    if(flags & FP_RasterFlag_Smooth) { plat_flags |= MAC_PLAT_RasterFlag_Smooth; }
    if(flags & FP_RasterFlag_Hinted) { plat_flags |= MAC_PLAT_RasterFlag_Hinted; }

    //- rjf: rasterize (`size` is in points; pixel size matches the other backends)
    MAC_PLAT_RasterResult raster = {0};
    F32 size_px = (96.f/72.f) * size;
    if(mac_plat_font_raster(font, size_px, plat_flags, string32.str, (size_t)string32.size, fp_ct_arena_alloc, arena, &raster))
    {
      result.atlas_dim.x = (S16)Min(raster.width, 0x7fff);
      result.atlas_dim.y = (S16)Min(raster.height, 0x7fff);
      result.atlas       = raster.pixels;
      result.advance     = raster.advance;
    }

    scratch_end(scratch);
  }
  ProfEnd();
  return result;
}
