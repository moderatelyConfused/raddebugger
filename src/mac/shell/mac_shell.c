// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal void *
mac_sh_arena_alloc(void *user, size_t size)
{
  Arena *arena = (Arena *)user;
  void *result = push_array_no_zero(arena, U8, size);
  return result;
}

internal String8
mac_sh_install_symlink_path(Arena *arena)
{
  // rjf: "installed" on macOS means: ~/.local/bin/raddbg is a symlink to this executable.
  String8 result = {0};
  char *home = getenv("HOME");
  if(home != 0 && home[0] != 0)
  {
    result = push_str8f(arena, "%s/.local/bin/raddbg", home);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Shell Operations

internal void
sh_message(B32 error, String8 title, String8 message)
{
  Temp scratch = scratch_begin(0, 0);
  B32 done = 0;

  //- rjf: try a native alert (main thread only)
  {
    String8 title_copy = push_str8_copy(scratch.arena, title);
    String8 message_copy = push_str8_copy(scratch.arena, message);
    done = mac_plat_message_box(!!error, (char *)title_copy.str, (char *)message_copy.str);
  }

  //- rjf: fall back to stderr
  if(!done)
  {
    if(error)
    {
      fprintf(stderr, "[X] ");
    }
    fprintf(stderr, "%.*s\n", str8_varg(title));
    fprintf(stderr, "%.*s\n\n", str8_varg(message));
  }

  scratch_end(scratch);
}

internal String8
sh_pick_file(Arena *arena, String8 title, String8 initial_path)
{
  String8 result = {0};
  Temp scratch = scratch_begin(&arena, 1);
  String8 title_copy = push_str8_copy(scratch.arena, title);
  String8 initial_path_copy = push_str8_copy(scratch.arena, initial_path);
  size_t size = 0;
  char *path = mac_plat_pick_file((char *)title_copy.str, (char *)initial_path_copy.str, mac_sh_arena_alloc, arena, &size);
  if(path != 0 && size != 0)
  {
    result = str8((U8 *)path, (U64)size);
  }
  scratch_end(scratch);
  return result;
}

internal void
sh_show_in_file_browser(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  String8 path_copy = push_str8_copy(scratch.arena, path);
  mac_plat_show_in_file_browser((char *)path_copy.str);
  scratch_end(scratch);
}

internal void
sh_open_in_browser(String8 url)
{
  Temp scratch = scratch_begin(0, 0);
  String8 url_copy = push_str8_copy(scratch.arena, url);
  B32 done = mac_plat_open_url((char *)url_copy.str);
  if(!done)
  {
    sh_message(1, s("Error"), str8f(scratch.arena, "Could not find a way to open %S in a locally installed browser.", url));
  }
  scratch_end(scratch);
}

internal B32
sh_install_or_uninstall_self(B32 write, B32 install)
{
  B32 install_state = 0;
  Temp scratch = scratch_begin(0, 0);

  //- rjf: unpack context
  String8 binary_path = push_str8_copy(scratch.arena, get_process_info()->binary_file_path);
  String8 symlink_path = mac_sh_install_symlink_path(scratch.arena);

  if(symlink_path.size != 0 && binary_path.size != 0)
  {
    //- rjf: if writing -> do mutation
    if(write)
    {
      //- rjf: install -> make symlink in local binary path to this exe
      if(install)
      {
        make_directory(str8_chop_last_slash(symlink_path));
        unlink((char *)symlink_path.str);
        int result = 0;
        do
        {
          result = symlink((char *)binary_path.str, (char *)symlink_path.str);
        }
        while(result == -1 && errno == EINTR);
      }

      //- rjf: uninstall -> delete symlink
      else
      {
        unlink((char *)symlink_path.str);
      }

      install_state = install;
    }

    //- rjf: do reading -> check if the symlink exists & points at this exe
    else
    {
      char target[4096] = {0};
      ssize_t target_size = readlink((char *)symlink_path.str, target, sizeof(target)-1);
      if(target_size > 0)
      {
        String8 target_string = str8((U8 *)target, (U64)target_size);
        install_state = str8_match(target_string, binary_path, 0);
      }
    }
  }

  scratch_end(scratch);
  return install_state;
}
