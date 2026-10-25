// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_SOCKET_H
#define MAC_SOCKET_H

////////////////////////////////
//~ rjf: Includes

#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

////////////////////////////////
//~ rjf: Implementation Types

typedef struct MAC_SOCK_Connection MAC_SOCK_Connection;
struct MAC_SOCK_Connection
{
  MAC_SOCK_Connection *next;
  MAC_SOCK_Connection *prev;
  SOCK_Endpoint endpoint;
  SOCK_Protocol protocol;
  int socket;
};

typedef struct MAC_SOCK_ConnectionSlot MAC_SOCK_ConnectionSlot;
struct MAC_SOCK_ConnectionSlot
{
  MAC_SOCK_Connection *first;
  MAC_SOCK_Connection *last;
};

typedef struct MAC_SOCK_Session MAC_SOCK_Session;
struct MAC_SOCK_Session
{
  MAC_SOCK_Session *next;
  MAC_SOCK_Session *prev;
  Arena *arena;
  GuardedRing *u2s_ring;
  GuardedRing *s2u_ring;
  int kqueue_fd;
  SOCK_WakeupFunctionType *wakeup_hook;
  int tcp_listen_socket;
  U64 connection_slots_count;
  StripeArray connection_stripes;
  MAC_SOCK_ConnectionSlot *connection_slots;
  Thread listener_thread;
};

typedef struct MAC_SOCK_State MAC_SOCK_State;
struct MAC_SOCK_State
{
  Arena *arena;
  RWMutex session_rw_mutex;
  MAC_SOCK_Session *first_session;
  MAC_SOCK_Session *last_session;
};

////////////////////////////////
//~ rjf: Globals

global MAC_SOCK_State *mac_sock_state = 0;

////////////////////////////////
//~ rjf: Helpers

internal void mac_sock_socket_setup(int fd);
internal void mac_sock_kqueue_add(int kqueue_fd, int fd, void *udata);
internal void mac_sock_kqueue_del(int kqueue_fd, int fd);

////////////////////////////////
//~ rjf: Listener Thread

internal void mac_sock_listener_thread_entry_point(void *p);

#endif // MAC_SOCKET_H
