/*
**      C Channels -- Channels Library for C
**      src/c_chan.c
**
**      Copyright (C) 2025  Paul J. Lucas
**
**      This program is free software: you can redistribute it and/or modify
**      it under the terms of the GNU General Public License as published by
**      the Free Software Foundation, either version 3 of the License, or
**      (at your option) any later version.
**
**      This program is distributed in the hope that it will be useful,
**      but WITHOUT ANY WARRANTY; without even the implied warranty of
**      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**      GNU General Public License for more details.
**
**      You should have received a copy of the GNU General Public License
**      along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// local
#include "chan.h"
#include "util.h"

// standard
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

////////// inline functions ///////////////////////////////////////////////////

static inline bool chan_empty( channel_t const *chan ) {
  return chan->buf.len == 0;
}

static inline bool chan_full( struct channel *chan ) {
  return chan->buf.len == chan->buf_cap;
}

////////// local functions ////////////////////////////////////////////////////

static bool chan_buf_recv( struct channel *chan, void *data,
                           struct timespec const *timeout ) {
  bool is_closed = false;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( chan_empty( chan ) ) {
    if ( (is_closed = chan->is_closed) )
      goto done;
    if ( timeout == NULL )
      PTHREAD_COND_WAIT( &chan->not_empty, &chan->mtx );
    else
      PTHREAD_COND_TIMEDWAIT( &chan->not_empty, &chan->mtx, timeout );
  }

  memcpy( data, chan->buf.buf + chan->buf.idx[0] * chan->msg_size,
          chan->msg_size );
  chan->buf.idx[0] = (chan->buf.idx[0] + 1) % chan->buf_cap;
  --chan->buf.len;

  PTHREAD_COND_SIGNAL( &chan->not_full );

done:
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( is_closed )
    errno = EPIPE;
  return !is_closed;
}

static bool chan_buf_send( struct channel *chan, void *data,
                           struct timespec const *timeout ) {
  bool is_closed;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( true ) {
    if ( (is_closed = chan->is_closed) )
      goto done;
    if ( !chan_full( chan ) )
      break;
    if ( timeout == NULL )
      PTHREAD_COND_WAIT( &chan->not_full, &chan->mtx );
    else
      PTHREAD_COND_TIMEDWAIT( &chan->not_full, &chan->mtx, timeout );
  }

  memcpy( chan->buf.buf + chan->buf.idx[1] * chan->msg_size, data,
          chan->msg_size );
  chan->chan->buf.idx[1] = (chan->buf.idx[1] + 1) % chan->buf_cap;
  ++chan->buf.len;

  PTHREAD_COND_SIGNAL( &chan->not_empty );

done:
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( is_closed )
    errno = EPIPE;
  return !is_closed;
}

static bool chan_can_recv( struct channel const *chan ) {
}

static bool chan_unbuf_recv( struct channel *chan, void *data,
                             struct timespec const *timeout ) {
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool const is_closed = chan->is_closed;
  if ( !is_closed ) {
    PTHREAD_MUTEX_LOCK( &chan->unbuf.mtx[1] );
    chan->unbuf.recv_buf = data;
    PTHREAD_COND_SIGNAL( &chan->not_empty );
    PTHREAD_COND_WAIT( &chan->not_full, &chan->mtx );
    PTHREAD_MUTEX_UNLOCK( &chan->unbuf.mtx[1] );
  }
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return !is_closed;
}

static bool chan_unbuf_send( struct channel *chan, void *data,
                             struct timespec const *timeout ) {
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool is_closed = chan->is_closed;
  if ( !is_closed ) {
    PTHREAD_MUTEX_LOCK( &chan->unbuf.mtx[1] );
    PTHREAD_COND_WAIT( &chan->not_empty, &chan->mtx );
    if ( !(is_closed = chan->is_closed) )
      memcpy( chan->unbuf.recv_buf, data, chan->msg_size );
    PTHREAD_COND_SIGNAL( &chan->not_full );
    PTHREAD_MUTEX_UNLOCK( &chan->unbuf.mtx[1] );
  }
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( is_closed )
    errno = EPIPE;
  return !is_closed;
}

////////// extern functions ///////////////////////////////////////////////////

void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) ) {
  if ( chan == nullptr )
    return;
  if ( chan->buf_cap == 0 ) {
    PTHREAD_MUTEX_DESTROY( &chan->unbuf.mtx[0] );
    PTHREAD_MUTEX_DESTROY( &chan->unbuf.mtx[1] );
  }
  else {
    for ( unsigned i = 0; i < chan->buf.len; ++i )
      (*free_fn)( chan->buf.buf[i] );
    free( chan->buf.buf );
  }

  PTHREAD_COND_DESTROY( &chan->not_empty );
  PTHREAD_COND_DESTROY( &chan->not_full );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct channel *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( chan->mtx );
  chan->is_closed = true;
  PTHREAD_MUTEX_UNLOCK( chan->mtx );
  PTHREAD_COND_BROADCAST( chan->not_empty );
  PTHREAD_COND_BROADCAST( chan->not_full );
}

bool chan_init( struct channel *chan, size_t buf_cap, size_t msg_size ) {
  assert( chan != NULL );

  if ( msg_size == 0 )
    msg_size = 1;

  if ( buf_cap == 0 ) {                 // unbuffered init
    chan->recv_buf = nullptr;
    PTHREAD_MUTEX_INIT( &chan->unbuf.mtx[0], 0 );
    PTHREAD_MUTEX_INIT( &chan->unbuf.mtx[1], 0 );
  }
  else {                                // buffered init
    chan->buf = malloc( buf_cap * msg_size );
    if ( chan->buf == NULL ) {
      errno = ENOMEM;
      return false;
    }
    chan->buf_idx[0] = chan->buf_idx[1] = 0;
    chan->buf.len = 0;
  }

  chan->buf_cap = buf_cap;
  chan->msg_size = msg_size;
  chan->is_closed = false;

  PTHREAD_MUTEX_INIT( &chan->mtx, 0 );
  PTHREAD_COND_INIT( &chan->not_empty, 0 );
  PTHREAD_COND_INIT( &chan->not_full, 0 );
  return true;
}

bool chan_recv( struct channel *chan, void *data,
                struct timespec const *timeout ) {
  assert( chan != NULL );
  assert( data != NULL );

  return chan->buf_cap == 0 ?
    chan_unbuf_recv( chan, data, timeout ) :
    chan_buf_recv( chan, data, timeout );
}

bool chan_send( struct channel *chan, void *data,
                struct timespec const *timeout ) {
  assert( chan != NULL );
  assert( data != NULL );

  return chan->buf_cap == 0 ?
    chan_unbuf_send( chan, data, timeout ) :
    chan_buf_send( chan, data, timeout );
}

int chan_select( size_t recv_n, struct channel *recv_chan[recv_n],
                 void *recv_data[recv_n],
                 size_t send_n, struct channel *send_chan[send_n],
                 void const *send_data[send_n] ) {
  int rv = -1;
  // TODO
  return rv;
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
