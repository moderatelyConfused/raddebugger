// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_SHELL_H
#define MAC_SHELL_H

////////////////////////////////
//~ rjf: Includes
//
// Dialogs, Finder and URL opening go through the plain C API of the separately
// compiled src/mac/mac_platform.m (Cocoa); installation is plain POSIX.

#include "mac/mac_platform.h"
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

////////////////////////////////
//~ rjf: Helpers

internal void *mac_sh_arena_alloc(void *user, size_t size);
internal String8 mac_sh_install_symlink_path(Arena *arena);

#endif // MAC_SHELL_H
