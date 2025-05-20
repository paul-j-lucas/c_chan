/*
**      c_chan -- Channels Library for C
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
#include "c_chan.h"
#include "util.h"

// standard
#include <assert.h>
#include <errno.h>
#include <sys/time.h>                   /* for gettimeofday(2) */
#include <stdlib.h>
#include <string.h>                     /* for memcpy(3) */
#include <unistd.h>

static bool cond_reltimedwait( pthread_cond_t*, pthread_mutex_t*,
                               struct timespec const* );

////////// inline functions ///////////////////////////////////////////////////

/**
 * Gets whether \a chan is buffered.
 *
 * @param chan The \ref channel to check.
 * @return Returns `true` only if \a chan is buffered.
 */
static inline bool chan_is_buffered( struct channel const *chan ) {
  return chan->buf_cap > 0;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Receives data from a buffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
static chan_rv chan_buf_recv( struct channel *chan, void *recv_buf,
                              struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( chan->buf.ring_len == 0 && rv == CHAN_OK ) {
    if ( chan->is_closed )
      rv = CHAN_CLOSED;
    else if ( timeout == NULL )
      PTHREAD_COND_WAIT( &chan->not_empty, &chan->mtx );
    else if ( !cond_reltimedwait( &chan->not_empty, &chan->mtx, timeout ) )
      rv = CHAN_TIMEDOUT;
  } // while

  if ( rv == CHAN_OK ) {
    memcpy( recv_buf, chan->buf.ring_buf + chan->buf.recv_idx * chan->msg_size,
            chan->msg_size );
    chan->buf.recv_idx = (chan->buf.recv_idx + 1) % chan->buf_cap;
    --chan->buf.ring_len;
    PTHREAD_COND_SIGNAL( &chan->not_full );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends data to a buffered \ref channel.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
static chan_rv chan_buf_send( struct channel *chan, void const *send_buf,
                              struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed )
      rv = CHAN_CLOSED;
    else if ( chan->buf.ring_len < chan->buf_cap )
      break;
    else if ( timeout == NULL )
      PTHREAD_COND_WAIT( &chan->not_full, &chan->mtx );
    else if ( !cond_reltimedwait( &chan->not_full, &chan->mtx, timeout ) )
      rv = CHAN_TIMEDOUT;
  } // while

  if ( rv == CHAN_OK ) {
    memcpy( chan->buf.ring_buf + chan->buf.send_idx * chan->msg_size, send_buf,
            chan->msg_size );
    chan->buf.send_idx = (chan->buf.send_idx + 1) % chan->buf_cap;
    ++chan->buf.ring_len;
    PTHREAD_COND_SIGNAL( &chan->not_empty );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * TODO
 */
static bool chan_can_recv( struct channel const *chan ) {
  if ( chan_is_buffered( chan ) )
    return chan->buf.ring_len > 0;
}

/**
 * Receives data from an unbuffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
static bool chan_unbuf_recv( struct channel *chan, void *recv_buf,
                             struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  if ( chan->is_closed ) {
    rv = CHAN_CLOSED;
  }
  else {
    PTHREAD_MUTEX_LOCK( &chan->unbuf.recv_buf_mtx );
    chan->unbuf.recv_buf = recv_buf;
    PTHREAD_COND_BROADCAST( &chan->not_full );

    // Wait for a sender to copy the data.
    ++chan->recv_wait_cnt;
    if ( timeout == NULL )
      PTHREAD_COND_WAIT( &chan->not_empty, &chan->mtx );
    else if ( !cond_reltimedwait( &chan->not_empty, &chan->mtx, timeout ) )
      rv = CHAN_TIMEDOUT;
    --chan->recv_wait_cnt;

    chan->unbuf.recv_buf = NULL;
    PTHREAD_MUTEX_UNLOCK( &chan->unbuf.recv_buf_mtx );
    if ( rv == CHAN_OK && chan->is_closed )
      rv = CHAN_CLOSED;
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends data to an unbuffered \ref channel.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
static bool chan_unbuf_send( struct channel *chan, void const *send_buf,
                             struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed ) {
      rv = CHAN_CLOSED;
    }
    else if ( chan->unbuf.recv_buf == NULL ) { // there is no reader: wait
      ++chan->send_wait_cnt;
      if ( timeout == NULL )
        PTHREAD_COND_WAIT( &chan->not_full, &chan->mtx );
      else if ( !cond_reltimedwait( &chan->not_full, &chan->mtx, timeout ) )
        rv = CHAN_TIMEDOUT;
      --chan->send_wait_cnt;
    }
    else {
      break;
    }
  } // while

  if ( rv == CHAN_OK ) {
    memcpy( chan->unbuf.recv_buf, send_buf, chan->msg_size );
    chan->unbuf.recv_buf = NULL;
    PTHREAD_COND_SIGNAL( &chan->not_empty );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Like `pthread_cond_timedwait()` except \a timeout specifies how long to
 * wait, not an absolute time.
 *
 * @param cond The condition to wait for.
 * @param mtx The mutex to unlock.
 * @param timeout How long to wait.
 * @return Returns `true` only if the wait succeeded; `false` if the timeout
 * expired.
 */
static bool cond_reltimedwait( pthread_cond_t *cond, pthread_mutex_t *mtx,
                               struct timespec const *timeout ) {
  assert( cond != NULL );
  assert( mtx != NULL );
  assert( timeout != NULL );

  struct timeval now;
  (void)gettimeofday( &now, /*tzp=*/NULL );

  struct timespec const abs_time = {
    .tv_sec  = timeout->tv_sec  + now.tv_sec,
    .tv_nsec = timeout->tv_nsec + now.tv_usec * 1000
  };

  int const rv = pthread_cond_timedwait( cond, mtx, &abs_time );
  switch ( rv ) {
    case 0:
      return true;
    case ETIMEDOUT:
      return false;
    default:
      errno = rv;
      perror_exit( EX_IOERR );
  } // switch
}

////////// extern functions ///////////////////////////////////////////////////

void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) ) {
  if ( chan == NULL )
    return;
  if ( chan_is_buffered( chan ) ) {
    if ( free_fn != NULL ) {
      unsigned idx = chan->buf.recv_idx;
      for ( ; chan->buf.ring_len > 0; --chan->buf.ring_len ) {
        (*free_fn)( &chan->buf.ring_buf[idx] );
        idx = (idx + 1) % chan->buf_cap;
      }
    }
    chan->buf_cap = 0;
    free( chan->buf.ring_buf );
    chan->buf.ring_buf = NULL;
    chan->buf.recv_idx = chan->buf.send_idx = 0;
  }
  else {
    chan->unbuf.recv_buf = NULL;
    PTHREAD_MUTEX_DESTROY( &chan->unbuf.recv_buf_mtx );
  }

  PTHREAD_COND_DESTROY( &chan->not_empty );
  PTHREAD_COND_DESTROY( &chan->not_full );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct channel *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  chan->is_closed = true;
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  PTHREAD_COND_BROADCAST( &chan->not_empty );
  PTHREAD_COND_BROADCAST( &chan->not_full );
}

bool chan_init( struct channel *chan, unsigned buf_cap, size_t msg_size ) {
  assert( chan != NULL );
  assert( msg_size > 0 );

  if ( buf_cap > 0 ) {                  // buffered init
    chan->buf.ring_buf = malloc( buf_cap * msg_size );
    if ( chan->buf.ring_buf == NULL ) {
      errno = ENOMEM;
      return false;
    }
    chan->buf.recv_idx = chan->buf.send_idx = 0;
    chan->buf.ring_len = 0;
  }
  else {                                // unbuffered init
    chan->unbuf.recv_buf = NULL;
    PTHREAD_MUTEX_INIT( &chan->unbuf.recv_buf_mtx, 0 );
  }

  chan->buf_cap = buf_cap;
  chan->is_closed = false;
  chan->msg_size = msg_size;
  chan->recv_wait_cnt = chan->send_wait_cnt = 0;

  PTHREAD_MUTEX_INIT( &chan->mtx, 0 );
  PTHREAD_COND_INIT( &chan->not_empty, 0 );
  PTHREAD_COND_INIT( &chan->not_full, 0 );
  return true;
}

chan_rv chan_recv( struct channel *chan, void *recv_buf,
                   struct timespec const *timeout ) {
  assert( chan != NULL );
  assert( recv_buf != NULL );

  return chan_is_buffered( chan ) ?
    chan_buf_recv( chan, recv_buf, timeout ) :
    chan_unbuf_recv( chan, recv_buf, timeout );
}

chan_rv chan_send( struct channel *chan, void const *send_buf,
                   struct timespec const *timeout ) {
  assert( chan != NULL );
  assert( send_buf != NULL );

  return chan_is_buffered( chan ) ?
    chan_buf_send( chan, send_buf, timeout ) :
    chan_unbuf_send( chan, send_buf, timeout );
}

int chan_select( unsigned recv_n, struct channel *recv_chan[recv_n],
                 void *recv_buf[recv_n],
                 unsigned send_n, struct channel *send_chan[send_n],
                 void const *send_buf[send_n] ) {
  (void)recv_n;
  (void)recv_chan;
  (void)recv_buf;
  (void)send_n;
  (void)send_chan;
  (void)send_buf;

  int rv = -1;
  // TODO
  return rv;
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
