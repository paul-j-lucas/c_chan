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

/**
 * @file
 * Defines types and functions to implement Go-like channels in C.
 */

// local
#include "c_chan.h"
#include "util.h"

// standard
#include <assert.h>
#include <attribute.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>                     /* for memcpy(3) */
#include <sys/time.h>                   /* for gettimeofday(2) */
#include <unistd.h>

/**
 * TODO
 * @{
 */
#define CHAN_BUF_NOT_EMPTY    CHAN_RECV
#define CHAN_BUF_NOT_FULL     CHAN_SEND
/** @} */

/**
 * TODO
 * @{
 */
#define CHAN_UNBUF_SEND_DONE  CHAN_RECV
#define CHAN_UNBUF_RECV_WAIT  CHAN_SEND
/** @} */

////////// enumerations ///////////////////////////////////////////////////////

/**
 * Channel direction.
 */
enum chan_dir {
  CHAN_RECV,                            ///< Receive direction.
  CHAN_SEND                             ///< Send direction.
};

////////// typedefs ///////////////////////////////////////////////////////////

typedef enum    chan_dir chan_dir;
typedef struct  chan_select_ref     chan_select_ref;

/**
 * The signature for a function passed to **qsort**(3).
 *
 * @param i_data A pointer to data.
 * @param j_data A pointer to data.
 * @return Returns an integer less than, equal to, or greater than 0, according
 * to whether the data pointed to by \a i_data is less than, equal to, or
 * greater than the data pointed to by \a j_data.
 */
typedef int (*qsort_cmp_fn)( void const *i_data, void const *j_data );

////////// structs ////////////////////////////////////////////////////////////

/**
 * TODO
 */
struct chan_select_ref {
  struct channel *chan;                 ///< The \ref channel referred to.

  /**
   * Index into either the \p recv_chan and \p recv_buf, or \p send_chan and \p
   * send_buf parameters of chan_select().
   */
  unsigned short  param_idx;

  /**
   * Indicates whether param_idx refers to either the \p recv_chan and \p
   * recv_buf, or or \p send_chan and \p send_buf parameters of chan_select().
   */
  chan_dir        dir;

  bool            maybe_ready;          ///< Is \ref chan maybe ready?
};

////////// local functions ////////////////////////////////////////////////////

static void     chan_notify( struct channel*, chan_dir,
                             int (*)( pthread_cond_t* ) );

NODISCARD
static bool     chan_unbuf_recv( struct channel*, void*,
                                 struct timespec const* );

NODISCARD
static bool     chan_unbuf_send( struct channel*, void const*,
                                 struct timespec const* );

NODISCARD
static chan_rv  chan_wait( struct channel*, chan_dir, struct timespec const* );

NODISCARD
static int      pthread_cond_relwait( pthread_cond_t*, pthread_mutex_t*,
                                      struct timespec const* );

// local variables
static struct timespec const  CHAN_NO_TIMEOUT_TIMESPEC;
static pthread_once_t         srand_once = PTHREAD_ONCE_INIT;

// extern variables
struct timespec const *const CHAN_NO_TIMEOUT = &CHAN_NO_TIMEOUT_TIMESPEC;

////////// inline functions ///////////////////////////////////////////////////

/**
 * Gets a pointer to the ith message in the buffered \a chan.
 *
 * @param chan The buffered channel to get a pointer to the ith message of.
 * @param i The index of the message.
 * @return Returns a pointer to the ith message in buffered \a chan.
 */
NODISCARD
static inline void* chan_buf_at( struct channel *chan, unsigned i ) {
  return (char*)chan->buf.ring_buf + i * chan->msg_size;
}

/**
 * Gets whether \a chan is buffered.
 *
 * @param chan The \ref channel to check.
 * @return Returns `true` only if \a chan is buffered.
 */
NODISCARD
static inline bool chan_is_buffered( struct channel const *chan ) {
  return chan->buf_cap > 0;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Receives data from a buffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.
 * @param timeout How long to wait. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
NODISCARD
static chan_rv chan_buf_recv( struct channel *chan, void *recv_buf,
                              struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed )
      rv = CHAN_CLOSED;
    else if ( chan->buf.ring_len > 0 )
      break;
    else
      rv = chan_wait( chan, CHAN_BUF_NOT_EMPTY, timeout );
  } // while

  if ( rv == CHAN_OK ) {
    memcpy( recv_buf, chan_buf_at( chan, chan->buf.recv_idx ), chan->msg_size );
    chan->buf.recv_idx = (chan->buf.recv_idx + 1) % chan->buf_cap;
    if ( chan->buf.ring_len-- == chan->buf_cap )
      chan_notify( chan, CHAN_BUF_NOT_FULL, &pthread_cond_signal );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends data to a buffered \ref channel.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.
 * @param timeout How long to wait. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
NODISCARD
static chan_rv chan_buf_send( struct channel *chan, void const *send_buf,
                              struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed )
      rv = CHAN_CLOSED;
    else if ( chan->buf.ring_len < chan->buf_cap )
      break;
    else
      rv = chan_wait( chan, CHAN_BUF_NOT_FULL, timeout );
  } // while

  if ( rv == CHAN_OK ) {
    memcpy( chan_buf_at( chan, chan->buf.send_idx ), send_buf, chan->msg_size );
    chan->buf.send_idx = (chan->buf.send_idx + 1) % chan->buf_cap;
    if ( chan->buf.ring_len++ == 0 )
      chan_notify( chan, CHAN_BUF_NOT_EMPTY, &pthread_cond_signal );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Notifies \a chan and all its observers that a condition has been signaled.
 *
 * @param chan The \ref channel to notify about.
 * @param dir Whether to notify about a receive or send condition.
 * @param pthread_cond_fn The `pthread_cond_t` function to call, either
 * `pthread_cond_signal` or `pthread_cond_broadcast`.
 */
static void chan_notify( struct channel *chan, chan_dir dir,
                         int (*pthread_cond_fn)( pthread_cond_t* ) ) {
  for ( chan_obs_impl *obs = &chan->observer[ dir ]; obs != NULL;
        obs = obs->next ) {
    obs->chan = chan;
    PERROR_EXIT_IF( (*pthread_cond_fn)( &obs->cond ) != 0, EX_IOERR );
  } // for
}

/**
 * Removes \a remove_obs as an observer from all \a chan.
 *
 * @param remove_obs TODO.
 * @param chan_len The length of \a chan.
 * @param chan The array of channels to remove \a remove_obs from.
 * @param dir The common direction of \a chan.
 */
static void chan_obs_remove( chan_obs_impl *remove_obs, unsigned chan_len,
                             struct channel *chan[chan_len], chan_dir dir ) {
  assert( remove_obs != NULL );
  assert( chan != NULL );

  for ( unsigned i = 0; i < chan_len; ++i ) {
    pthread_mutex_t *pmtx = &chan[i]->mtx;
    PTHREAD_MUTEX_LOCK( pmtx );
    --chan[i]->wait_cnt[ dir ];

    for ( chan_obs_impl *obs = chan[i]->observer[ dir ].next; obs != NULL; ) {
      if ( obs->next == remove_obs ) {
        PTHREAD_MUTEX_LOCK( obs->next->pmtx );
        obs->next = obs->next->next;
        PTHREAD_MUTEX_UNLOCK( obs->next->pmtx );
        break;
      }

      PTHREAD_MUTEX_UNLOCK( pmtx );
      pmtx = obs->pmtx;
      PTHREAD_MUTEX_LOCK( pmtx );
      obs = obs->next;
    } // for
    PTHREAD_MUTEX_UNLOCK( pmtx );
  } // for
}

/**
 * Attempts to receive data from a \ref channel, but does not wait if either
 * the channel is empty (buffered) or no sender is waiting (unbuffered).
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_recv()
 */
NODISCARD
static chan_rv chan_recv_nowait( struct channel *chan, void *recv_buf ) {
  assert( chan != NULL );

  if ( chan_is_buffered( chan ) ) {
    assert( recv_buf != NULL );
    return chan_buf_recv( chan, recv_buf, /*timeout=*/NULL );
  }

  return chan_unbuf_recv( chan, recv_buf, /*timeout=*/NULL );
}

/**
 * Initializes \a ref for chan_select().
 *
 * @param ref The array of channel references to initialize.
 * @param pref_len Must be 0 to start; updated to be the number of references.
 * @param chan_len The length of \a chan.
 * @param chan The channels to initialize from.
 * @param dir The common direction of \a chan.
 * @param add_obs The observer to add to each channel.  If `NULL`, the select
 * is non-blocking.
 * @return Returns the number of channels that may be ready.
 */
NODISCARD
static unsigned chan_select_init( chan_select_ref ref[], unsigned *pref_len,
                                  unsigned chan_len,
                                  struct channel *chan[chan_len], chan_dir dir,
                                  chan_obs_impl *add_obs ) {
  assert( ref != NULL );
  assert( pref_len != NULL );
  assert( chan != NULL );

  unsigned maybe_ready_len = 0;

  for ( unsigned i = 0; i < chan_len; ++i ) {
    bool is_ready = false;
    PTHREAD_MUTEX_LOCK( &chan[i]->mtx );

    if ( !chan[i]->is_closed ) {
      is_ready = chan_is_buffered( chan[i] ) ?
        (dir == CHAN_RECV ?
          chan[i]->buf.ring_len > 0 :
          chan[i]->buf.ring_len < chan[i]->buf_cap) :
        chan[i]->wait_cnt[ !dir ] > 0;

      if ( add_obs != NULL ) {
        add_obs->next = chan[i]->observer[ dir ].next;
        chan[i]->observer[ dir ].next = add_obs;
        ++chan[i]->wait_cnt[ dir ];
      }

      if ( is_ready || add_obs != NULL ) {
        ref[ (*pref_len)++ ] = (chan_select_ref){
          .chan = chan[i],
          .dir = dir,
          .param_idx = (unsigned short)i,
          .maybe_ready = is_ready
        };
      }
    }

    PTHREAD_MUTEX_UNLOCK( &chan[i]->mtx );

    if ( is_ready )
      ++maybe_ready_len;
  } // for

  return maybe_ready_len;
}

/**
 * Compares two \ref chan_select_ref objects.
 *
 * @param i_csr The first \ref chan_select_ref.
 * @param j_csr The second \ref chan_select_ref.
 * @return Returns a number less than 0, 0, or greater than 0 if \a i_csr is
 * less than, equal to, or greater than \a j_csr, respectively.
 */
static int chan_select_ref_cmp( chan_select_ref const *i_csr,
                                chan_select_ref const *j_csr ) {
  // sort maybe_ready (true, aka, 1) before !maybe_ready (false, aka, 0)
  return (int)j_csr->maybe_ready - (int)i_csr->maybe_ready;
}

/**
 * Attempts to send data to a \ref channel, but does not wait if either the
 * channel is full (buffered) or no receiver is waiting (unbuffered).
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_send()
 */
NODISCARD
static chan_rv chan_send_nowait( struct channel *chan, void const *send_buf ) {
  assert( chan != NULL );
  assert( send_buf != NULL );

  return chan_is_buffered( chan ) ?
    chan_buf_send( chan, send_buf, /*timeout=*/NULL ) :
    chan_unbuf_send( chan, send_buf, /*timeout=*/NULL );
}

/**
 * Receives data from an unbuffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.
 * @param timeout How long to wait. If `NULL`, does not wait.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
NODISCARD
static bool chan_unbuf_recv( struct channel *chan, void *recv_buf,
                             struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed ) {
      rv = CHAN_CLOSED;
    }
    else if ( chan->wait_cnt[ CHAN_SEND ] == 0 && timeout == NULL ) {
      rv = CHAN_TIMEDOUT;               // no sender and shouldn't wait
    }
    else if ( chan->unbuf.recv_buf != NULL ) {
      // Some other thread has called chan_unbuf_recv() that has already set
      // recv_buf and is waiting for a sender.  We must wait for that other
      // thread to reset recv_buf.
      PTHREAD_COND_WAIT( &chan->unbuf.recv_buf_is_null, &chan->mtx );
    }
    else {
      chan->unbuf.recv_buf = recv_buf;
      chan_notify( chan, CHAN_UNBUF_RECV_WAIT, &pthread_cond_signal );

      // Wait for a sender to copy the data.
      rv = chan_wait( chan, CHAN_UNBUF_SEND_DONE, timeout );

      chan->unbuf.recv_buf = NULL;
      PTHREAD_COND_SIGNAL( &chan->unbuf.recv_buf_is_null );
    }
  } // while

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends data to an unbuffered \ref channel.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.
 * @param timeout How long to wait. If `NULL`, does not wait.
 * @return Returns a \ref chan_rv.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
NODISCARD
static bool chan_unbuf_send( struct channel *chan, void const *send_buf,
                             struct timespec const *timeout ) {
  chan_rv rv = CHAN_OK;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  while ( rv == CHAN_OK ) {
    if ( chan->is_closed )
      rv = CHAN_CLOSED;
    else if ( chan->wait_cnt[ CHAN_RECV ] == 0 && timeout == NULL )
      rv = CHAN_TIMEDOUT;               // no receiver and shouldn't wait
    else if ( chan->unbuf.recv_buf != NULL )
      break;                            // there is a receiver
    else
      rv = chan_wait( chan, CHAN_UNBUF_RECV_WAIT, timeout );
  } // while

  if ( rv == CHAN_OK ) {
    if ( chan->msg_size > 0 )
      memcpy( chan->unbuf.recv_buf, send_buf, chan->msg_size );
    chan_notify( chan, CHAN_UNBUF_SEND_DONE, &pthread_cond_broadcast );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Like `pthread_cond_wait()` and `pthread_cond_timedwait()` except:
 *  + If \a timeout is #CHAN_TIMEOUT waits indefinitely;
 *  + Otherwise \a timeout specifies how long to wait, not an absolute time.
 *
 * @param chan The \ref channel to wait for.
 * @param dir Whether to wait to receive or send.
 * @param timeout The duration of time to wait. If `NULL`, returns
 * #CHAN_TIMEOUT; if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns a \ref chan_rv.
 *
 * @warning \ref channel::mtx must be locked before calling this function.
 */
NODISCARD
static chan_rv chan_wait( struct channel *chan, chan_dir dir,
                          struct timespec const *timeout ) {
  assert( chan != NULL );

  chan_rv rv;

  if ( timeout == NULL ) {
    rv = CHAN_TIMEDOUT;
  }
  else {
    rv = CHAN_OK;
    ++chan->wait_cnt[ dir ];
    if ( timeout == CHAN_NO_TIMEOUT ) {
      PTHREAD_COND_WAIT( &chan->observer[ dir ].cond, &chan->mtx );
    }
    else {
      int const pcr_rv = pthread_cond_relwait( 
        &chan->observer[ dir ].cond, &chan->mtx, timeout
      );
      switch ( pcr_rv ) {
        case 0:
          break;
        case ETIMEDOUT:
          rv = CHAN_TIMEDOUT;
          break;
        default:
          unreachable();
      } // switch
    }
    --chan->wait_cnt[ dir ];
  }

  return chan->is_closed ? CHAN_CLOSED : rv;
}

/**
 * Like pthread_cond_timedwait(3), except \a timeout specifies a duration of
 * time rather than an absolute time.
 *
 * @param cond The condition to wait for.
 * @param mtx The mutex to unlock temporarily.
 * @param timeout The duration of time to wait.
 * @return Returns either 0 only if \a cond was signaled or `ETIMEDOUT` only if
 * \a timeout expired.
 */
NODISCARD
static int pthread_cond_relwait( pthread_cond_t *cond, pthread_mutex_t *mtx,
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

  int const pct_rv = pthread_cond_timedwait( cond, mtx, &abs_time );
  switch ( pct_rv ) {
    case 0:
    case ETIMEDOUT:
      return pct_rv;
    default:
      errno = pct_rv;
      perror_exit( EX_IOERR );
  } // switch
}

/**
 * Calls **srand**(3) using the time-of-day as the seed.
 */
static void srand_init( void ) {
  struct timeval now;
  (void)gettimeofday( &now, /*tzp=*/NULL );
  srand( (unsigned)now.tv_usec );
}

////////// extern functions ///////////////////////////////////////////////////

void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) ) {
  if ( chan == NULL )
    return;
  if ( chan_is_buffered( chan ) ) {
    if ( chan->buf.ring_len > 0 && free_fn != NULL ) {
      unsigned idx = chan->buf.recv_idx;
      for ( unsigned i = 0; i < chan->buf.ring_len; ++i ) {
        (*free_fn)( chan_buf_at( chan, idx ) );
        idx = (idx + 1) % chan->buf_cap;
      }
    }
    free( chan->buf.ring_buf );
  }
  else {
    PTHREAD_COND_DESTROY( &chan->unbuf.recv_buf_is_null );
  }

  PTHREAD_COND_DESTROY( &chan->observer[ CHAN_RECV ].cond );
  PTHREAD_COND_DESTROY( &chan->observer[ CHAN_SEND ].cond );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct channel *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool const was_already_closed = chan->is_closed;
  chan->is_closed = true;
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( !was_already_closed ) {          // wake up all waiting threads, if any
    chan_notify( chan, CHAN_RECV, &pthread_cond_broadcast );
    chan_notify( chan, CHAN_SEND, &pthread_cond_broadcast );
    if ( !chan_is_buffered( chan ) )
      PTHREAD_COND_BROADCAST( &chan->unbuf.recv_buf_is_null );
  }
}

bool chan_init( struct channel *chan, unsigned buf_cap, size_t msg_size ) {
  assert( chan != NULL );

  chan->buf_cap = buf_cap;

  if ( chan_is_buffered( chan ) ) {
    assert( msg_size > 0 );
    chan->buf.ring_buf = malloc( buf_cap * msg_size );
    if ( chan->buf.ring_buf == NULL )
      return false;
    chan->buf.recv_idx = chan->buf.send_idx = 0;
    chan->buf.ring_len = 0;
  }
  else {
    chan->unbuf.recv_buf = NULL;
    PTHREAD_COND_INIT( &chan->unbuf.recv_buf_is_null, /*attr=*/0 );
  }

  chan->msg_size = msg_size;
  chan->is_closed = false;

  PTHREAD_MUTEX_INIT( &chan->mtx, /*attr=*/0 );
  chan->wait_cnt[ CHAN_RECV ] = chan->wait_cnt[ CHAN_SEND ] = 0;

  chan_obs_impl *obs = &chan->observer[ CHAN_RECV ];
  PTHREAD_COND_INIT( &obs->cond, /*attr=*/0 );
  obs->next = NULL;
  obs->pmtx = &chan->mtx;
  obs->chan = NULL;

  obs = &chan->observer[ CHAN_RECV ];
  PTHREAD_COND_INIT( &obs->cond, /*attr=*/0 );
  obs->next = NULL;
  obs->pmtx = &chan->mtx;
  obs->chan = NULL;

  return true;
}

chan_rv chan_recv( struct channel *chan, void *recv_buf,
                   struct timespec const *timeout ) {
  assert( chan != NULL );
  assert( recv_buf != NULL );

  if ( timeout == NULL )
    timeout = CHAN_NO_TIMEOUT;

  return chan_is_buffered( chan ) ?
    chan_buf_recv( chan, recv_buf, timeout ) :
    chan_unbuf_recv( chan, recv_buf, timeout );
}

int chan_select( unsigned recv_len, struct channel *recv_chan[recv_len],
                 void *recv_buf[recv_len],
                 unsigned send_len, struct channel *send_chan[send_len],
                 void const *send_buf[send_len],
                 struct timespec const *timeout ) {
  assert( recv_len == 0 || (recv_chan != NULL && recv_buf != NULL) );
  assert( send_len == 0 || (send_chan != NULL && send_buf != NULL) );

  unsigned const total_len = recv_len + send_len;
  bool const wait = timeout != NULL;

  chan_select_ref stack_ref[16];
  chan_select_ref *const ref = total_len <= ARRAY_SIZE( stack_ref ) ?
    stack_ref : malloc( total_len * sizeof( chan_select_ref ) );

  chan_obs_impl select_obs;
  pthread_mutex_t select_mtx;

  if ( wait ) {
    PTHREAD_COND_INIT( &select_obs.cond, /*attr=*/0 );
    select_obs.next = NULL;
    select_obs.pmtx = &select_mtx;
  }

  PTHREAD_MUTEX_INIT( &select_mtx, /*attr=*/0 );

retry:;

  unsigned ref_len = 0;                 // number of channels to select from
  unsigned const maybe_ready_len =      // number of those that may be ready
    chan_select_init(
      ref, &ref_len, recv_len, recv_chan, CHAN_RECV, wait ? &select_obs : NULL
    ) +
    chan_select_init(
      ref, &ref_len, send_len, send_chan, CHAN_SEND, wait ? &select_obs : NULL
    );

  if ( maybe_ready_len > 0 && maybe_ready_len < ref_len ) {
    qsort(                              // sort maybe ready channels first ...
      ref, ref_len, sizeof( chan_select_ref ),
      (qsort_cmp_fn)&chan_select_ref_cmp
    );
    ref_len = maybe_ready_len;          // ... and select only from those
  }

  chan_select_ref const *selected_ref = NULL;

  if ( ref_len > 0 ) {
    chan_rv rv = CHAN_OK;

    if ( maybe_ready_len == 0 && wait ) {
      PTHREAD_MUTEX_LOCK( &select_mtx );
      if ( timeout == CHAN_NO_TIMEOUT ) {
        PTHREAD_COND_WAIT( &select_obs.cond, &select_mtx );
      }
      else if ( pthread_cond_relwait( &select_obs.cond, &select_mtx,
                                      timeout ) == ETIMEDOUT ) {
        rv = CHAN_TIMEDOUT;
      }
      PTHREAD_MUTEX_UNLOCK( &select_mtx );

      if ( rv == CHAN_OK ) {
        for ( unsigned i = 0; i < ref_len; ++i ) {
          if ( ref[i].chan == select_obs.chan ) {
            selected_ref = &ref[i];
            break;
          }
        } // for
        assert( selected_ref != NULL );
      }
    }
    else {
      PTHREAD_ONCE( &srand_once, &srand_init );
      selected_ref = &ref[ rand() % (int)ref_len ];
    }

    if ( rv == CHAN_OK ) {
      rv = selected_ref->dir == CHAN_RECV ?
        chan_recv_nowait(
          recv_chan[ selected_ref->param_idx ],
          recv_buf[ selected_ref->param_idx ]
        ) :
        chan_send_nowait(
          send_chan[ selected_ref->param_idx ],
          send_buf[ selected_ref->param_idx ]
        );
    }

    switch ( rv ) {
      case CHAN_OK:
        break;
      case CHAN_CLOSED:
        if ( ref_len > 1 )              // at least 1 potentially open channel
          goto retry;
        break;                          // only channel is closed
      case CHAN_TIMEDOUT:
        goto retry;
    } // switch
  }

  if ( ref != stack_ref )
    free( ref );

  if ( wait ) {
    chan_obs_remove( &select_obs, recv_len, recv_chan, CHAN_RECV );
    chan_obs_remove( &select_obs, send_len, send_chan, CHAN_SEND );
    PTHREAD_COND_DESTROY( &select_obs.cond );
  }

  PTHREAD_MUTEX_DESTROY( &select_mtx );

  if ( selected_ref == NULL )
    return -1;
  return selected_ref->dir == CHAN_RECV ?
    CHAN_SELECT_RECV( selected_ref->param_idx ) :
    CHAN_SELECT_SEND( selected_ref->param_idx );
}

chan_rv chan_send( struct channel *chan, void const *send_buf,
                   struct timespec const *timeout ) {
  assert( chan != NULL );

  if ( timeout == NULL )
    timeout = CHAN_NO_TIMEOUT;

  if ( chan_is_buffered( chan ) ) {
    assert( send_buf != NULL );
    return chan_buf_send( chan, send_buf, timeout );
  }

  return chan_unbuf_send( chan, send_buf, timeout );
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
