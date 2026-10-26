// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal void
mac_sock_socket_setup(int fd)
{
  // NOTE(rjf): macOS has no MSG_NOSIGNAL / SOCK_CLOEXEC / SOCK_NONBLOCK /
  // accept4; SIGPIPE suppression is a per-socket option, and close-on-exec is
  // set via fcntl.
  if(fd >= 0)
  {
    int no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
}

internal void
mac_sock_kqueue_add(int kqueue_fd, int fd, void *udata)
{
  struct kevent evt = {0};
  EV_SET(&evt, fd, EVFILT_READ, EV_ADD, 0, 0, udata);
  MAC_RETRY_ON_EINTR(kevent(kqueue_fd, &evt, 1, 0, 0, 0));
}

internal void
mac_sock_kqueue_del(int kqueue_fd, int fd)
{
  struct kevent evt = {0};
  EV_SET(&evt, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
  MAC_RETRY_ON_EINTR(kevent(kqueue_fd, &evt, 1, 0, 0, 0));
}

////////////////////////////////
//~ rjf: Listener Thread

internal void
mac_sock_listener_thread_entry_point(void *p)
{
  ThreadNameF("mac_sock_listener_thread_%I64x", p);
  MAC_SOCK_Session *session = (MAC_SOCK_Session *)p;
  for(;;)
  {
    //- rjf: wait for next event
    struct kevent evts[1] = {0};
    int wait_result = MAC_RETRY_ON_EINTR(kevent(session->kqueue_fd, 0, 0, evts, ArrayCount(evts), 0));
    if(wait_result < 0 && errno == EBADF)
    {
      // rjf: kqueue was closed -> session is gone
      break;
    }
    if(wait_result <= 0)
    {
      continue;
    }

    //- rjf: call wakeup hook
    if(session->wakeup_hook)
    {
      session->wakeup_hook();
    }

    //- rjf: user data is 0 -> listener is ready for accept
    if(evts[0].udata == 0)
    {
      // rjf: accept new connection
      struct sockaddr_storage addr = {0};
      socklen_t addr_size = sizeof(addr);
      int new_socket = MAC_RETRY_ON_EINTR(accept(session->tcp_listen_socket, (struct sockaddr *)&addr, &addr_size));
      if(new_socket < 0)
      {
        continue;
      }
      mac_sock_socket_setup(new_socket);

      // rjf: unpack socket's endpoint info
      SOCK_Endpoint endpoint = {0};
      {
        switch(addr.ss_family)
        {
          default:{}break;
          case AF_INET:
          {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)&addr;
            endpoint.kind = SOCK_EndpointKind_IPv4;
            endpoint.port = ntohs(ipv4->sin_port);
            endpoint.address_u32[0] = ipv4->sin_addr.s_addr;
          }break;
          case AF_INET6:
          {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&addr;
            endpoint.kind = SOCK_EndpointKind_IPv6;
            endpoint.port = ntohs(ipv6->sin6_port);
            MemoryCopy(endpoint.address_u8, ipv6->sin6_addr.s6_addr, sizeof(endpoint.address_u8));
          }break;
        }
      }

      // rjf: unpack endpoint
      U64 hash = u64_hash_from_str8(str8_struct(&endpoint));
      U64 slot_idx = hash%session->connection_slots_count;
      MAC_SOCK_ConnectionSlot *slot = &session->connection_slots[slot_idx];
      Stripe *stripe = stripe_from_slot_idx(&session->connection_stripes, slot_idx);

      // rjf: store new connection
      MAC_SOCK_Connection *con = 0;
      RWMutexScope(stripe->rw_mutex, 1)
      {
        con = (MAC_SOCK_Connection *)stripe->free;
        if(con != 0)
        {
          stripe->free = con->next;
        }
        else
        {
          con = push_array(stripe->arena, MAC_SOCK_Connection, 1);
        }
        con->endpoint = endpoint;
        con->protocol = SOCK_Protocol_TCP;
        con->socket = new_socket;
        DLLPushBack(slot->first, slot->last, con);
      }

      // rjf: add socket to kqueue
      mac_sock_kqueue_add(session->kqueue_fd, new_socket, con);
    }

    //- rjf: user data is nonzero -> ready to read on a connection.
    else
    {
      // rjf: unpack associated connection
      MAC_SOCK_Connection *con = (MAC_SOCK_Connection *)evts[0].udata;
      SOCK_Endpoint endpoint = con->endpoint;
      U64 hash = u64_hash_from_str8(str8_struct(&endpoint));
      U64 slot_idx = hash%session->connection_slots_count;
      MAC_SOCK_ConnectionSlot *slot = &session->connection_slots[slot_idx];
      Stripe *stripe = stripe_from_slot_idx(&session->connection_stripes, slot_idx);

      // rjf: do read
      U8 buffer[4096] = {0};
      ssize_t recv_result = MAC_RETRY_ON_EINTR(recv(con->socket, buffer, sizeof(buffer), MSG_DONTWAIT));

      // rjf: bytes received -> push result to user.
      if(recv_result > 0)
      {
        RingGuard g = guarded_ring_open(session->s2u_ring);
        // NOTE: the endpoint kind rides in the high 32 bits of the protocol word, so
        // the endpoint reconstructed in sock_recv is byte-identical to the one stored
        // on accept (otherwise the connection lookup in sock_async_tick, which hashes
        // and MemoryMatchStructs the whole endpoint, would miss and open a stray
        // connection instead of replying on the accepted socket).
        U64 header[5] =
        {
          (U64)SOCK_Protocol_TCP | ((U64)endpoint.kind << 32),
          (U64)endpoint.port,
          endpoint.address_u64[0],
          endpoint.address_u64[1],
          (U64)recv_result,
        };
        guarded_ring_write_or_wait(&g, sizeof(header), header, max_U64);
        guarded_ring_write_or_wait(&g, recv_result, buffer, max_U64);
        guarded_ring_close(&g);
      }

      // rjf: error -> socket was disconnected
      else RWMutexScope(stripe->rw_mutex, 1)
      {
        mac_sock_kqueue_del(session->kqueue_fd, con->socket);
        close(con->socket);
        DLLRemove(slot->first, slot->last, con);
        con->next = stripe->free;
        stripe->free = con;
      }
    }
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Top-Level Layer Calls

internal void
sock_init(void)
{
  Arena *arena = arena_alloc();
  mac_sock_state = push_array(arena, MAC_SOCK_State, 1);
  mac_sock_state->arena = arena;
  mac_sock_state->session_rw_mutex = rw_mutex_alloc();
}

internal void
sock_async_tick(void)
{
  Temp scratch = scratch_begin(0, 0);

  //////////////////////////////
  //- rjf: gather send tasks
  //
  typedef struct SendTask SendTask;
  struct SendTask
  {
    SendTask *next;
    MAC_SOCK_Session *session;
    SOCK_Endpoint endpoint;
    String8 data;
  };
  SendTask *first_tcp_send = 0;
  SendTask *last_tcp_send = 0;
  if(lane_idx() == 0)
  {
    for(;;)
    {
      B32 got_more = 0;
      RWMutexScope(mac_sock_state->session_rw_mutex, 0)
      {
        for EachNode(s, MAC_SOCK_Session, mac_sock_state->first_session)
        {
          RingGuard g = guarded_ring_open(s->u2s_ring);
          {
            U64 header[5] = {0};
            if(guarded_ring_try_read(&g, sizeof(header), header))
            {
              got_more = 1;
              SOCK_Protocol protocol = (SOCK_Protocol)(header[0] & 0xffffffff);
              U16 port = (U16)header[1];
              SOCK_Endpoint endpoint = {0};
              endpoint.kind = (SOCK_EndpointKind)(header[0] >> 32);
              endpoint.address_u64[0] = header[2];
              endpoint.address_u64[1] = header[3];
              endpoint.port = port;
              U64 data_size = header[4];
              U8 *data = push_array(scratch.arena, U8, data_size);
              guarded_ring_read_or_wait(&g, data_size, data, max_U64);
              SendTask *t = push_array(scratch.arena, SendTask, 1);
              t->session = s;
              t->endpoint = endpoint;
              t->data = str8(data, data_size);
              SLLQueuePush(first_tcp_send, last_tcp_send, t);
            }
          }
          guarded_ring_close(&g);
        }
      }
      if(!got_more)
      {
        break;
      }
    }
  }
  lane_sync();

  //////////////////////////////
  //- rjf: do TCP sends
  //
  for(SendTask *t = first_tcp_send; t != 0; t = t->next)
  {
    MAC_SOCK_Session *session = t->session;

    // rjf: unpack endpoint
    U64 hash = u64_hash_from_str8(str8_struct(&t->endpoint));
    U64 slot_idx = hash%session->connection_slots_count;
    MAC_SOCK_ConnectionSlot *slot = &session->connection_slots[slot_idx];
    Stripe *stripe = stripe_from_slot_idx(&session->connection_stripes, slot_idx);

    // rjf: get existing socket for this endpoint
    int ep_socket = -1;
    RWMutexScope(stripe->rw_mutex, 0)
    {
      for(MAC_SOCK_Connection *c = slot->first; c != 0; c = c->next)
      {
        if(MemoryMatchStruct(&c->endpoint, &t->endpoint))
        {
          ep_socket = c->socket;
          break;
        }
      }
    }

    // rjf: didn't get a socket? -> open socket
    if(ep_socket == -1) RWMutexScope(stripe->rw_mutex, 1)
    {
      // rjf: try to get socket again, now that we have the write lock
      MAC_SOCK_Connection *con = 0;
      for(MAC_SOCK_Connection *c = slot->first; c != 0; c = c->next)
      {
        if(MemoryMatchStruct(&c->endpoint, &t->endpoint))
        {
          con = c;
          break;
        }
      }

      // rjf: no socket still? -> create
      if(con == 0)
      {
        // rjf: convert endpoint -> sockaddr
        struct sockaddr_storage endpoint_sockaddr = {0};
        int endpoint_sockaddr_size = 0;
        {
          switch(t->endpoint.kind)
          {
            default:{}break;
            case SOCK_EndpointKind_IPv4:
            {
              struct sockaddr_in *dst = (struct sockaddr_in *)(&endpoint_sockaddr);
              endpoint_sockaddr_size = sizeof(*dst);
              dst->sin_len = sizeof(*dst);
              dst->sin_family = AF_INET;
              dst->sin_port = htons(t->endpoint.port);
              MemoryCopy(&dst->sin_addr, &t->endpoint.address_u32[0], sizeof(U32));
            }break;
            case SOCK_EndpointKind_IPv6:
            {
              struct sockaddr_in6 *dst = (struct sockaddr_in6 *)(&endpoint_sockaddr);
              endpoint_sockaddr_size = sizeof(*dst);
              dst->sin6_len = sizeof(*dst);
              dst->sin6_family = AF_INET6;
              dst->sin6_port = htons(t->endpoint.port);
              MemoryCopy(&dst->sin6_addr, &t->endpoint.address_u128[0], sizeof(U128));
            }break;
          }
        }

        // rjf: create
        int new_socket = socket(AF_INET, SOCK_STREAM, 0);
        mac_sock_socket_setup(new_socket);
        connect(new_socket, (struct sockaddr *)&endpoint_sockaddr, endpoint_sockaddr_size);

        // rjf: store in cache
        con = (MAC_SOCK_Connection *)stripe->free;
        if(con != 0)
        {
          stripe->free = con->next;
        }
        else
        {
          con = push_array(stripe->arena, MAC_SOCK_Connection, 1);
        }
        con->endpoint = t->endpoint;
        con->protocol = SOCK_Protocol_TCP;
        con->socket = new_socket;
        DLLPushBack(slot->first, slot->last, con);

        // rjf: hook up to session kqueue
        mac_sock_kqueue_add(session->kqueue_fd, new_socket, con);
      }

      // rjf: get socket from cache
      ep_socket = con->socket;
    }

    // rjf: got socket? -> send (SIGPIPE is suppressed per-socket via SO_NOSIGPIPE). a
    // send may take only part of a large packet (a full socket buffer); the rest follows.
    B32 send_failed = 0;
    if(ep_socket != -1)
    {
      U64 wait_count = 0;
      for(U64 sent = 0; sent < t->data.size;)
      {
        ssize_t n = MAC_RETRY_ON_EINTR(send(ep_socket, t->data.str + sent, t->data.size - sent, 0));
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_count < 10)
        {
          // rjf: the peer is not reading yet: wait for room (10 x 1 s at most)
          struct pollfd pfd = {ep_socket, POLLOUT, 0};
          poll(&pfd, 1, 1000);
          wait_count += 1;
          continue;
        }
        if(n <= 0)
        {
          send_failed = 1;
          break;
        }
        sent += (U64)n;
      }
    }

    // rjf: got a socket, but send failed? -> connection closed
    if(ep_socket != -1 && send_failed)
    {
      RWMutexScope(stripe->rw_mutex, 1)
      {
        for(MAC_SOCK_Connection *c = slot->first, *next = 0; c != 0; c = next)
        {
          next = c->next;
          if(MemoryMatchStruct(&c->endpoint, &t->endpoint))
          {
            mac_sock_kqueue_del(session->kqueue_fd, c->socket);
            close(c->socket);
            DLLRemove(slot->first, slot->last, c);
            c->next = stripe->free;
            stripe->free = c;
          }
        }
      }
    }
  }

  scratch_end(scratch);
}

////////////////////////////////
//~ rjf: @per_os_impl Session Creation/Closing

internal SOCK_Session
sock_session_open(U16 listener_port, SOCK_WakeupFunctionType *wakeup_hook)
{
  //- rjf: set up state
  Arena *arena = arena_alloc();
  MAC_SOCK_Session *session = push_array(arena, MAC_SOCK_Session, 1);
  session->arena = arena;
  session->u2s_ring = guarded_ring_alloc(arena, KB(256));
  session->s2u_ring = guarded_ring_alloc(arena, KB(256));
  session->kqueue_fd = kqueue();
  session->wakeup_hook = wakeup_hook;
  session->tcp_listen_socket = -1;

  //- rjf: set up listener
  if(listener_port != 0)
  {
    session->tcp_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    mac_sock_socket_setup(session->tcp_listen_socket);
    U32 ipv6only = 0;
    setsockopt(session->tcp_listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&ipv6only, sizeof(ipv6only));
    struct sockaddr_in server_addr = {0};
    server_addr.sin_len = sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listener_port);
    // NOTE: without SO_REUSEADDR the bind fails while connections of a previous
    // instance (e.g. `--ipc` clients) linger in TIME_WAIT on the port, and the
    // listener is then silently unreachable for seconds; a restarted debugger
    // did not answer IPC for ~10s.
    int reuse_addr = 1;
    setsockopt(session->tcp_listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    if(bind(session->tcp_listen_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)
    {
      // NOTE: a listener which could not bind its port must not be used: `listen` on the
      // unbound socket would succeed on a kernel-chosen port no client knows, and the
      // session would look alive. the caller gets a zero session (and the log the reason).
      log_infof("mac socket: could not bind the IPC listener to port %u (errno %d)\n", (unsigned)listener_port, errno);
      close(session->tcp_listen_socket);
      close(session->kqueue_fd);
      arena_release(arena);
      SOCK_Session zero = {0};
      return zero;
    }
    listen(session->tcp_listen_socket, SOMAXCONN);
    mac_sock_kqueue_add(session->kqueue_fd, session->tcp_listen_socket, 0);
  }

  //- rjf: set up connection cache
  session->connection_slots_count = 8;
  session->connection_stripes = stripe_array_alloc(arena);
  session->connection_slots = push_array(arena, MAC_SOCK_ConnectionSlot, session->connection_slots_count);

  //- rjf: launch listener thread
  session->listener_thread = thread_launch(mac_sock_listener_thread_entry_point, session);

  //- rjf: link into top-level storage
  RWMutexScope(mac_sock_state->session_rw_mutex, 1)
  {
    DLLPushBack(mac_sock_state->first_session, mac_sock_state->last_session, session);
  }

  //- rjf: bundle as handle
  SOCK_Session s = {(U64)session};
  return s;
}

internal void
sock_session_close(SOCK_Session session)
{
  // TODO(rjf)
}

////////////////////////////////
//~ rjf: @per_os_impl Sends

internal B32
sock_send(SOCK_Session session, SOCK_Protocol protocol, SOCK_Endpoint endpoint, String8 data, U64 endt_us)
{
  B32 result = 0;
  MAC_SOCK_Session *s = (MAC_SOCK_Session *)session.u64[0];
  if(s == 0)
  {
    return result;
  }
  RingGuard guard = guarded_ring_open(s->u2s_ring);
  {
    // NOTE: the user -> socket ring holds 256 KB; a message larger than that used to be
    // cut to the ring (`Min(data.size, size_cap)`), silently: an `--ipc state` reply
    // with ~1500 modules announced its full size in its header and arrived truncated.
    // a large message now goes into the ring as several packets of at most 64 KB, which
    // the async tick sends in order on the same connection (a TCP stream carries no
    // packet boundaries; the receiver reassembles by the message's own header).
    U64 header[5] = {0};
    U64 chunk_size_cap = KB(64);
    U64 sent_size = 0;
    for(;;)
    {
      U64 size = Min(data.size - sent_size, chunk_size_cap);
      {
        // NOTE: the endpoint kind rides in the high 32 bits of the protocol word (see
        // the listener thread); sock_async_tick reconstructs the full endpoint from it
        // so the connection-cache lookup matches what was stored on accept.
        header[0] = (U64)protocol | ((U64)endpoint.kind << 32);
        header[1] = (U64)endpoint.port;
        header[2] = endpoint.address_u64[0];
        header[3] = endpoint.address_u64[1];
        header[4] = size;
      }
      if(!guarded_ring_write_or_wait(&guard, sizeof(header), header, endt_us))
      {
        break;
      }
      guarded_ring_write_or_wait(&guard, size, data.str + sent_size, max_U64);
      sent_size += size;
      result = 1;
      if(sent_size >= data.size)
      {
        break;
      }
    }
  }
  guarded_ring_close(&guard);
  if(result)
  {
    ins_atomic_u32_eval_assign(&async_loop_again, 1);
    cond_var_broadcast(async_tick_start_cond_var);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Receives

internal B32
sock_recv(Arena *arena, SOCK_Session session, SOCK_Protocol *protocol_out, SOCK_Endpoint *endpoint_out, String8 *data_out, U64 endt_us)
{
  B32 result = 0;
  U64 header_size = sizeof(*protocol_out) + sizeof(*endpoint_out) + sizeof(U64);
  MAC_SOCK_Session *s = (MAC_SOCK_Session *)session.u64[0];
  if(s != 0)
  {
    RingGuard guard = guarded_ring_open(s->s2u_ring);
    {
      U64 header[5] = {0};
      if(guarded_ring_read_or_wait(&guard, sizeof(header), header, endt_us))
      {
        U64 size_cap = s->s2u_ring->ring->size - sizeof(header);
        if(protocol_out != 0)
        {
          protocol_out[0] = (SOCK_Protocol)(header[0] & 0xffffffff);
        }
        endpoint_out->kind = (SOCK_EndpointKind)(header[0] >> 32);
        endpoint_out->port = (U16)header[1];
        endpoint_out->address_u64[0] = header[2];
        endpoint_out->address_u64[1] = header[3];
        data_out->size = Min(size_cap, header[4]);
        data_out->str = push_array(arena, U8, data_out->size);
        guarded_ring_read_or_wait(&guard, data_out->size, data_out->str, max_U64);
        result = 1;
      }
    }
    guarded_ring_close(&guard);
  }
  return result;
}
