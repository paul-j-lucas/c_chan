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

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <attribute.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>                     /* for memcpy(3) */
#include <sys/time.h>                   /* for clock_gettime(3) */
#include <unistd.h>

/// @endcond

/**
 * @ingroup c-chan-implementation-group
 * @{
 */

/**
 * For buffered channels, a more mnemonic name for specifying which observer
 * (receive or send) is meant.
 *
 * @sa chan_dir
 */
#define CHAN_BUF_NOT_EMPTY    CHAN_RECV

/**
 * @copydoc CHAN_BUF_NOT_EMPTY
 */
#define CHAN_BUF_NOT_FULL     CHAN_SEND

////////// enumerations ///////////////////////////////////////////////////////

/**
 * Channel direction.
 */
enum chan_dir {
  CHAN_RECV,                            ///< Receive direction.
  CHAN_SEND                             ///< Send direction.
};

////////// typedefs ///////////////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
typedef enum    chan_dir        chan_dir;
typedef struct  chan_select_ref chan_select_ref;
/// @endcond

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
 * In chan_select(), a single array of references to both receive and send
 * channels is created.
 */
struct chan_select_ref {
  struct chan    *chan;                 ///< The \ref chan referred to.

  /**
   * Index into either \a recv_chan and \a recv_buf, or \a send_chan and \a
   * send_buf parameters of chan_select().
   */
  unsigned short  param_idx;

  /**
   * Indicates whether param_idx refers to either the \a recv_chan and \a
   * recv_buf, or \a send_chan and \a send_buf parameters of chan_select().
   */
  chan_dir        dir;

  bool            maybe_ready;          ///< Is \ref chan maybe ready?
};

////////// local functions ////////////////////////////////////////////////////

static void chan_signal_all_obs( struct chan*, chan_dir,
                                 int (*)( pthread_cond_t* ) );

static void chan_unbuf_release( struct chan*, chan_dir );

NODISCARD
static int  chan_wait( struct chan*, chan_dir, struct timespec const* );

NODISCARD
static int  pthread_cond_wait_wrapper( pthread_cond_t*, pthread_mutex_t*,
                                       struct timespec const* );

// local variables

/// A variable to which \ref CHAN_NO_TIMEOUT can point.
static struct timespec const  CHAN_NO_TIMEOUT_TS;

/// @cond DOXYGEN_IGNORE

// extern variables
struct timespec const *const CHAN_NO_TIMEOUT = &CHAN_NO_TIMEOUT_TS;

/// @endcond

////////// inline functions ///////////////////////////////////////////////////

/**
 * Gets a pointer to the ith message in the buffered \a chan.
 *
 * @param chan The buffered channel to get a pointer to the ith message of.
 * @param abs_idx The absolute index of the message within the ring buffer.
 * @return Returns a pointer to the ith message in buffered \a chan.
 */
NODISCARD
static inline void* chan_buf_at( struct chan *chan, unsigned abs_idx ) {
  return (char*)chan->buf.ring_buf + abs_idx * chan->msg_size;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Adds \a add_obs as an observer of \a chan.
 *
 * @param chan Then \ref chan to add \a add_obs to.
 * @param dir The direction of \a chan.
 * @param add_obs The observer to add.
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 *
 * @sa chan_remove_obs()
 * @sa chan_select_init()
 */
static void chan_add_obs( struct chan *chan, chan_dir dir,
                          chan_impl_obs *add_obs ) {
  assert( chan != NULL );
  assert( add_obs != NULL );

#ifndef NDEBUG
  int debug_locked_cnt = 0;
#endif /* NDEBUG */
  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  for ( chan_impl_obs *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    next_obs = obs->next;
    if ( next_obs == NULL ) {           // At end of list.
      obs->next = add_obs;
    }
    else {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ... (1)
      DEBUG_BLOCK( ++debug_locked_cnt; );
      if ( add_obs->key < next_obs->key ) {
        obs->next = add_obs;
        add_obs->next = next_obs;
        next_obs = NULL;                // Will cause loop to exit ... (2)
        pmtx = next_pmtx;               // Will unlock next_mptx.
      }
      else {
        assert( add_obs->key != next_obs->key );
      }
    }
    if ( pmtx != NULL ) {               // (2) ... yet still runs this code.
      PTHREAD_MUTEX_UNLOCK( pmtx );     // (1) ... before unlocking previous.
      DEBUG_BLOCK( --debug_locked_cnt; );
    }
  } // for

  assert( debug_locked_cnt == 0 );
  ++chan->wait_cnt[ dir ];
}

/**
 * Receives a message from a buffered \ref chan.
 *
 * @param chan The \ref chan to receive from.
 * @param recv_buf The buffer to receive into.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message is available and \a abs_time is `NULL`; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
NODISCARD
static int chan_buf_recv( struct chan *chan, void *recv_buf,
                          struct timespec const *abs_time ) {
  int rv = 0;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  do {
    // Since we can still read from a closed, non-empty, buffered channel,
    // there's no check for is_closed first.
    if ( chan->buf.ring_len > 0 ) {
      memcpy( recv_buf, chan_buf_at( chan, chan->buf.recv_idx ),
              chan->msg_size );
      chan->buf.recv_idx = (chan->buf.recv_idx + 1) % chan->buf_cap;
      if ( chan->buf.ring_len-- == chan->buf_cap )
        chan_signal_all_obs( chan, CHAN_BUF_NOT_FULL, &pthread_cond_signal );
      break;
    }
    rv = chan_wait( chan, CHAN_BUF_NOT_EMPTY, abs_time );
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends a message to a buffered \ref chan.
 *
 * @param chan The \ref chan to send to.
 * @param send_buf The buffer to send from.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message can be sent and \a abs_time is `NULL`; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
NODISCARD
static int chan_buf_send( struct chan *chan, void const *send_buf,
                          struct timespec const *abs_time ) {
  int rv = 0;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  do {
    if ( chan->is_closed ) {
      rv = EPIPE;
    }
    else if ( chan->buf.ring_len < chan->buf_cap ) {
      memcpy( chan_buf_at( chan, chan->buf.send_idx ), send_buf,
              chan->msg_size );
      chan->buf.send_idx = (chan->buf.send_idx + 1) % chan->buf_cap;
      if ( ++chan->buf.ring_len == 1 )
        chan_signal_all_obs( chan, CHAN_BUF_NOT_EMPTY, &pthread_cond_signal );
      break;
    }
    else {
      rv = chan_wait( chan, CHAN_BUF_NOT_FULL, abs_time );
    }
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Checks whether \a chan is "hard closed."
 *
 * @remarks A non-empty buffered channel can still be received from even if
 * it's closed, so \a chan shouldn't be considered "hard closed" if it's such a
 * channel.
 *
 * @param dir The direction of \a chan to check.
 * @return Returns `true` only if \a chan is "hard closed."
 */
NODISCARD
static bool chan_is_hard_closed( struct chan const *chan, chan_dir dir ) {
  return  chan->is_closed &&
          (dir == CHAN_SEND || chan->buf_cap == 0 || chan->buf.ring_len == 0);
}

/**
 * Cleans-up a \ref chan_impl_obs.
 *
 * @param obs The \ref chan_impl_obs to clean up.
 *
 * @sa chan_obs_init()
 */
static void chan_obs_cleanup( chan_impl_obs *obs ) {
  assert( obs != NULL );
  PTHREAD_COND_DESTROY( &obs->chan_ready );
}

/**
 * Initializes a \ref chan_impl_obs.
 *
 * @param obs The \ref chan_impl_obs to initialize.
 * @param pmtx The mutex to use, if any.
 *
 * @sa chan_obs_cleanup()
 * @sa chan_obs_init_key()
 */
static void chan_obs_init( chan_impl_obs *obs, pthread_mutex_t *pmtx ) {
  assert( obs != NULL );

  obs->chan = NULL;
  PTHREAD_COND_INIT( &obs->chan_ready, /*attr=*/NULL );
  obs->key  = 0;
  obs->next = NULL;
  obs->pmtx = pmtx;
}

/**
 * Initializes a \ref chan_impl_obs key.
 *
 * @remarks An observer has an arbitrary, comparable key so the linked list of
 * a channel's observers can be in ascending key order.  The linked list is
 * traversed using hand-over-hand locking.  Since an observer can be in
 * multiple channels' lists, the ordering ensures that pairs of mutexes are
 * always locked in the same order on every list to avoid deadlocks.
 *
 * @param obs The \ref chan_impl_obs to initialize the key of.
 *
 * @sa chan_obs_init()
 * @sa chan_select_init()
 */
static void chan_obs_init_key( chan_impl_obs *obs ) {
  assert( obs != NULL );

  static unsigned         next_key      = 1;
  static pthread_mutex_t  next_key_mtx  = PTHREAD_MUTEX_INITIALIZER;

  PTHREAD_MUTEX_LOCK( &next_key_mtx );
  obs->key = next_key++;
  if ( next_key == 0 )                  // Reserved for channel itself.
    ++next_key;
  PTHREAD_MUTEX_UNLOCK( &next_key_mtx );
}

/**
 * Removes \a remove_obs as an observer from \a chan.
 *
 * @param chan Then \ref chan to remove \a remove_obs from.
 * @param dir The direction of \a chan.
 * @param remove_obs The observer to remove.
 *
 * @sa chan_add_obs()
 * @sa obs_remove_all_chan()
 */
static void chan_remove_obs( struct chan *chan, chan_dir dir,
                             chan_impl_obs *remove_obs ) {
  assert( chan != NULL );
  assert( remove_obs != NULL );

  // Whether the channel is closed now doesn't matter since it may have been
  // open when the observer was added.

#ifndef NDEBUG
  int   debug_locked_cnt = 0;
  bool  debug_removed = false;
#endif /* NDEBUG */
  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  PTHREAD_MUTEX_LOCK( &chan->mtx );

  for ( chan_impl_obs *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    next_obs = obs->next;
    if ( next_obs == remove_obs ) {
      // remove_obs is an observer in our caller's stack frame, i.e., this
      // thread, so there's no need to lock next_obs->pmtx.
      obs->next = next_obs->next;
      next_obs = NULL;                  // Will cause loop to exit ... (1)
      DEBUG_BLOCK( debug_removed = true; );
    }
    else if ( next_obs != NULL ) {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ... (2)
      DEBUG_BLOCK( ++debug_locked_cnt; );
    }
    if ( pmtx != NULL ) {               // (1) ... yet still runs this code.
      PTHREAD_MUTEX_UNLOCK( pmtx );     // (2) ... before unlocking previous.
      DEBUG_BLOCK( --debug_locked_cnt; );
    }
  } // for

  --chan->wait_cnt[ dir ];
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  assert( debug_locked_cnt == 0 );
  assert( debug_removed );
}

/**
 * Initializes \a ref for chan_select().
 *
 * @param ref The array of channel references to initialize.
 * @param pref_len Must be 0 to start; updated to be the number of references.
 * @param chan_len The length of \a chan.
 * @param chan The channels to initialize from.  May be `NULL` only if \a
 * chan_len is 0.
 * @param dir The common direction of \a chan.
 * @param add_obs The observer to add to each channel.  If `NULL`, the select
 * is non-blocking.
 * @return Returns the number of channels that may be ready.
 */
NODISCARD
static unsigned chan_select_init( chan_select_ref ref[], unsigned *pref_len,
                                  unsigned chan_len,
                                  struct chan *chan[chan_len], chan_dir dir,
                                  chan_impl_obs *add_obs ) {
  assert( ref != NULL );
  assert( pref_len != NULL );
  assert( chan_len == 0 || chan != NULL );

  unsigned maybe_ready_len = 0;

  if ( chan != NULL ) {
    for ( unsigned i = 0; i < chan_len; ++i ) {
      bool is_ready = false;
      PTHREAD_MUTEX_LOCK( &chan[i]->mtx );

      bool const is_hard_closed = chan_is_hard_closed( chan[i], dir );
      if ( !is_hard_closed ) {
        is_ready = chan[i]->buf_cap > 0 ?
          (dir == CHAN_RECV ?
            chan[i]->buf.ring_len > 0 :
            chan[i]->buf.ring_len < chan[i]->buf_cap) :
          chan[i]->wait_cnt[ !dir ] > 0;

        if ( add_obs != NULL )
          chan_add_obs( chan[i], dir, add_obs );
      }

      PTHREAD_MUTEX_UNLOCK( &chan[i]->mtx );

      if ( is_hard_closed )
        continue;
      if ( is_ready || add_obs != NULL ) {
        ref[ (*pref_len)++ ] = (chan_select_ref){
          .chan = chan[i],
          .dir = dir,
          .param_idx = (unsigned short)i,
          .maybe_ready = is_ready
        };
      }
      if ( is_ready )
        ++maybe_ready_len;
    } // for
  }

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
 * Signals all of a channel's observers' conditions.
 *
 * @param chan The \ref chan to notify about.
 * @param dir Whether to notify about a receive or send condition.
 * @param pthread_cond_fn The `pthread_cond_t` function to call, either
 * **pthread_cond_signal**(3) or **pthread_cond_broadcast**(3).
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 */
static void chan_signal_all_obs( struct chan *chan, chan_dir dir,
                                 int (*pthread_cond_fn)( pthread_cond_t* ) ) {
  if ( chan->wait_cnt[ dir ] == 0 )     // Nobody is waiting.
    return;

#ifndef NDEBUG
  int debug_locked_cnt = 0;
#endif /* NDEBUG */
  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  for ( chan_impl_obs *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    obs->chan = chan;
    PERROR_EXIT_IF( (*pthread_cond_fn)( &obs->chan_ready ) != 0, EX_IOERR );
    next_obs = obs->next;
    if ( next_obs != NULL ) {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ...
      DEBUG_BLOCK( ++debug_locked_cnt; );
    }
    if ( pmtx != NULL ) {
      PTHREAD_MUTEX_UNLOCK( pmtx );     // ... before unlocking previous.
      DEBUG_BLOCK( --debug_locked_cnt; );
    }
  } // for

  assert( debug_locked_cnt == 0 );
}

/**
 * Acquires exclusive \a dir access of an unbuffered channel.
 *
 * @param chan The unbuffered \ref chan to acquire.
 * @param dir The direction of \a chan.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 *
 * @sa chan_unbuf_release()
 */
NODISCARD
static int chan_unbuf_acquire( struct chan *chan, chan_dir dir,
                               struct timespec const *abs_time ) {
  int rv = 0;
  while ( rv == 0 && chan->unbuf.is_busy[ dir ] ) {
    rv = chan->is_closed ? EPIPE :
      pthread_cond_wait_wrapper( &chan->unbuf.not_busy[ dir ], &chan->mtx,
                                 abs_time );
  } // while
  if ( rv == 0 )
    chan->unbuf.is_busy[ dir ] = true;
  return rv;
}

/**
 * Receives a message from an unbuffered \ref chan.
 *
 * @param chan The \ref chan to receive from.
 * @param recv_buf The buffer to receive into.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message is available and \a abs_time is `NULL`; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @sa chan_buf_recv()
 * @sa chan_unbuf_send()
 */
NODISCARD
static int chan_unbuf_recv( struct chan *chan, void *recv_buf,
                            struct timespec const *abs_time ) {
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  int rv = chan_unbuf_acquire( chan, CHAN_RECV, abs_time );
  if ( rv == 0 ) {
    chan->unbuf.recv_buf = recv_buf;
    chan_signal_all_obs( chan, CHAN_SEND, &pthread_cond_signal );

    do {
      if ( chan->unbuf.is_busy[ CHAN_SEND ] ) {
        PTHREAD_COND_SIGNAL( &chan->unbuf.cpy_done[ CHAN_SEND ] );
        PTHREAD_COND_WAIT( &chan->unbuf.cpy_done[ CHAN_RECV ], &chan->mtx );
        PTHREAD_COND_SIGNAL( &chan->unbuf.cpy_done[ CHAN_SEND ] );
        break;
      }
      rv = chan_wait( chan, CHAN_RECV, abs_time );
    } while ( rv == 0 );

    chan->unbuf.recv_buf = NULL;
    chan_unbuf_release( chan, CHAN_RECV );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Releases exclusive \a dir access of an unbuffered channel.
 *
 * @param chan The unbuffered \ref chan to release.
 * @param dir The direction of \a chan.
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 *
 * @sa chan_unbuf_acquire()
 */
static void chan_unbuf_release( struct chan *chan, chan_dir dir ) {
  chan->unbuf.is_busy[ dir ] = false;
  PTHREAD_COND_SIGNAL( &chan->unbuf.not_busy[ dir ] );
}

/**
 * Sends a message to an unbuffered \ref chan.
 *
 * @param chan The \ref chan to send to.
 * @param send_buf The buffer to send from.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message can be sent and \a abs_time is `NULL`; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @sa chan_buf_send()
 * @sa chan_unbuf_recv()
 */
NODISCARD
static int chan_unbuf_send( struct chan *chan, void const *send_buf,
                            struct timespec const *abs_time ) {
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  int rv = chan_unbuf_acquire( chan, CHAN_SEND, abs_time );
  if ( rv == 0 ) {
    chan_signal_all_obs( chan, CHAN_RECV, &pthread_cond_signal );

    do {
      if ( chan->unbuf.is_busy[ CHAN_RECV ] ) {
        if ( chan->msg_size > 0 )
          memcpy( chan->unbuf.recv_buf, send_buf, chan->msg_size );
        PTHREAD_COND_SIGNAL( &chan->unbuf.cpy_done[ CHAN_RECV ] );
        PTHREAD_COND_WAIT( &chan->unbuf.cpy_done[ CHAN_SEND ], &chan->mtx );
        PTHREAD_COND_SIGNAL( &chan->unbuf.cpy_done[ CHAN_RECV ] );
        break;
      }
      rv = chan_wait( chan, CHAN_SEND, abs_time );
    } while ( rv == 0 );

    chan_unbuf_release( chan, CHAN_SEND );
  }

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Like **pthread_cond_wait**(3)** and **pthread_cond_timedwait**(3) except:
 *  + If \a abs_time is `NULL`, does not wait.
 *  + If \a abs_time is \ref CHAN_NO_TIMEOUT, waits indefinitely.
 *
 * @param chan The \ref chan to wait for.
 * @param dir Whether to wait to receive or send.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed or:
 *  + `EAGAIN` if \a abs_time is `NULL`; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 */
NODISCARD
static int chan_wait( struct chan *chan, chan_dir dir,
                      struct timespec const *abs_time ) {
  assert( chan != NULL );

  if ( chan->is_closed )
    return EPIPE;

  ++chan->wait_cnt[ dir ];
  int const rv = pthread_cond_wait_wrapper( &chan->observer[ dir ].chan_ready,
                                            &chan->mtx, abs_time );
  --chan->wait_cnt[ dir ];

  // The channel could have been closed while waiting, so check again.
  return chan_is_hard_closed( chan, dir ) ? EPIPE : rv;
}

/**
 * Like **pthread_cond_wait**(3)** and **pthread_cond_timedwait**(3) except:
 *  + If \a abs_time is `NULL`, returns immediatly.
 *  + If \a abs_time is \ref CHAN_NO_TIMEOUT, waits indefinitely.
 *  + Checks the return value of **pthread_cond_timedwait**(3) for errors other
 *    than `ETIMEDOUT`.
 *
 * @param cond The condition to wait for.
 * @param mtx The mutex to unlock temporarily.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns either 0 only if \a cond was signaled or `ETIMEDOUT` only if
 * it's now \a abs_time or later.
 */
NODISCARD
static int pthread_cond_wait_wrapper( pthread_cond_t *cond,
                                      pthread_mutex_t *mtx,
                                      struct timespec const *abs_time ) {
  assert( cond != NULL );
  assert( mtx != NULL );

  if ( abs_time == NULL )
    return EAGAIN;

  int const pcw_rv = abs_time == CHAN_NO_TIMEOUT ?
    pthread_cond_wait( cond, mtx ) :
    pthread_cond_timedwait( cond, mtx, abs_time );

  switch ( pcw_rv ) {
    case 0:
    case ETIMEDOUT:
      return pcw_rv;
    default:
      errno = pcw_rv;
      perror_exit( EX_IOERR );
  } // switch
}

/**
 * Gets a seed for **rand_r**(3) or **srand**(3).
 *
 * @param abs_time A pointer to an absolute time.  If not `NULL` nor \ref
 * CHAN_NO_TIMEOUT, uses it as the basis for a seed; otherwise, uses the
 * current time.
 * @return Returns a pseudo-random unsigned integer in the range 0 to
 * `RAND_MAX`.
 */
static unsigned rand_seed( struct timespec const *abs_time ) {
  if ( abs_time != NULL && abs_time != CHAN_NO_TIMEOUT )
    return (unsigned)abs_time->tv_nsec;
  struct timespec now;
  CLOCK_GETTIME( CLOCK_REALTIME, &now );
  return (unsigned)now.tv_nsec;
}

/**
 * Converts a duration to an absolute time in the future relative to now.
 *
 * @param duration The duration to convert.  May be `NULL` to mean do not wait
 * or \ref CHAN_NO_TIMEOUT to mean wait indefinitely.
 * @param abs_time A pointer to receive the absolute time, but only if \a
 * duration is neither `NULL` nor \ref CHAN_NO_TIMEOUT.
 * @return Returns \a duration if either `NULL` or \ref CHAN_NO_TIMEOUT, or \a
 * abs_time otherwise.
 */
NODISCARD
static struct timespec const* ts_dur_to_abs( struct timespec const *duration,
                                             struct timespec *abs_time ) {
  assert( abs_time != NULL );

  if ( duration == NULL || duration == CHAN_NO_TIMEOUT )
    return duration;

  struct timespec now;
  CLOCK_GETTIME( CLOCK_REALTIME, &now );

  *abs_time = (struct timespec){
    .tv_sec  = now.tv_sec  + duration->tv_sec,
    .tv_nsec = now.tv_nsec + duration->tv_nsec
  };

  return abs_time;
}

/** @} */

////////// extern functions ///////////////////////////////////////////////////

void chan_cleanup( struct chan *chan, void (*msg_cleanup_fn)( void* ) ) {
  if ( chan == NULL )
    return;

  if ( chan->buf_cap > 0 ) {
    if ( chan->buf.ring_len > 0 && msg_cleanup_fn != NULL ) {
      unsigned idx = chan->buf.recv_idx;
      for ( unsigned i = 0; i < chan->buf.ring_len; ++i ) {
        (*msg_cleanup_fn)( chan_buf_at( chan, idx ) );
        idx = (idx + 1) % chan->buf_cap;
      }
    }
    free( chan->buf.ring_buf );
  }
  else {
    PTHREAD_COND_DESTROY( &chan->unbuf.cpy_done[ CHAN_RECV ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.cpy_done[ CHAN_SEND ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.not_busy[ CHAN_RECV ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.not_busy[ CHAN_SEND ] );
  }

  chan_obs_cleanup( &chan->observer[ CHAN_RECV ] );
  chan_obs_cleanup( &chan->observer[ CHAN_SEND ] );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct chan *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool const was_already_closed = chan->is_closed;
  chan->is_closed = true;
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( was_already_closed )
    return;
  chan_signal_all_obs( chan, CHAN_RECV, &pthread_cond_broadcast );
  chan_signal_all_obs( chan, CHAN_SEND, &pthread_cond_broadcast );
  if ( chan->buf_cap == 0 ) {
    PTHREAD_COND_BROADCAST( &chan->unbuf.cpy_done[ CHAN_RECV ] );
    PTHREAD_COND_BROADCAST( &chan->unbuf.cpy_done[ CHAN_SEND ] );
    PTHREAD_COND_BROADCAST( &chan->unbuf.not_busy[ CHAN_RECV ] );
    PTHREAD_COND_BROADCAST( &chan->unbuf.not_busy[ CHAN_SEND ] );
  }
}

int chan_init( struct chan *chan, unsigned buf_cap, size_t msg_size ) {
  assert( chan != NULL );

  if ( buf_cap > 0 ) {
    if ( unlikely( msg_size == 0 ) )
      return EINVAL;
    chan->buf.ring_buf = malloc( buf_cap * msg_size );
    if ( unlikely( chan->buf.ring_buf == NULL ) )
      return ENOMEM;
    chan->buf.recv_idx = chan->buf.send_idx = 0;
    chan->buf.ring_len = 0;
  }
  else {
    chan->unbuf.recv_buf = NULL;
    PTHREAD_COND_INIT( &chan->unbuf.cpy_done[ CHAN_RECV ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.cpy_done[ CHAN_SEND ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.not_busy[ CHAN_RECV ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.not_busy[ CHAN_SEND ], /*attr=*/NULL );
    chan->unbuf.is_busy[ CHAN_RECV ] = false;
    chan->unbuf.is_busy[ CHAN_SEND ] = false;
  }

  chan->buf_cap = buf_cap;
  chan->msg_size = msg_size;
  chan->is_closed = false;

  PTHREAD_MUTEX_INIT( &chan->mtx, /*attr=*/NULL );
  chan->wait_cnt[ CHAN_RECV ] = chan->wait_cnt[ CHAN_SEND ] = 0;

  chan_obs_init( &chan->observer[ CHAN_RECV ], /*pmtx=*/NULL );
  chan_obs_init( &chan->observer[ CHAN_SEND ], /*pmtx=*/NULL );

  return 0;
}

int chan_recv( struct chan *chan, void *recv_buf,
               struct timespec const *duration ) {
  assert( chan != NULL );

  struct timespec abs_ts;
  struct timespec const *const abs_time = ts_dur_to_abs( duration, &abs_ts );

  if ( chan->buf_cap > 0 ) {
    if ( unlikely( recv_buf == NULL ) )
      return EINVAL;
    return chan_buf_recv( chan, recv_buf, abs_time );
  }

  if ( unlikely( chan->msg_size > 0 && recv_buf == NULL ) )
    return EINVAL;
  return chan_unbuf_recv( chan, recv_buf, abs_time );
}

int chan_select( unsigned recv_len, struct chan *recv_chan[recv_len],
                 void *recv_buf[recv_len],
                 unsigned send_len, struct chan *send_chan[send_len],
                 void const *send_buf[send_len],
                 struct timespec const *duration ) {
  if ( unlikely( recv_len > 0 && (recv_chan == NULL || recv_buf == NULL) ) ||
       unlikely( send_len > 0 && (send_chan == NULL || send_buf == NULL) ) ) {
    errno = EINVAL;
    return -1;
  }

  chan_select_ref  *ref, stack_ref[16];
  unsigned const    total_channels = recv_len + send_len;

  if ( total_channels <= ARRAY_SIZE( stack_ref ) ) {
    ref = stack_ref;
  }
  else {
    ref = malloc( total_channels * sizeof( chan_select_ref ) );
    if ( unlikely( ref == NULL ) )
      return -1;                        // malloc sets errno
  }

  struct timespec               abs_ts;
  struct timespec const *const  abs_time = ts_dur_to_abs( duration, &abs_ts );
  unsigned                      chans_open;   // number of open channels
  bool const                    is_blocking = duration != NULL;
  unsigned                      seed = 0;     // random number seed
  chan_impl_obs                 select_obs;   // observer for this select
  pthread_mutex_t               select_mtx;   // mutex for select_obs
  chan_select_ref const        *selected_ref; // reference to selected channel
  int                           rv;

  if ( is_blocking ) {
    PTHREAD_MUTEX_INIT( &select_mtx, /*attr=*/NULL );
    chan_obs_init( &select_obs, &select_mtx );
    chan_obs_init_key( &select_obs );
  }

  do {
    chans_open = 0;
    rv = 0;
    selected_ref = NULL;

    unsigned const maybe_ready_len =    // number of channels that may be ready
      chan_select_init(
        ref, &chans_open, recv_len, recv_chan, CHAN_RECV,
        is_blocking ? &select_obs : NULL
      ) +
      chan_select_init(
        ref, &chans_open, send_len, send_chan, CHAN_SEND,
        is_blocking ? &select_obs : NULL
      );

    if ( chans_open == 0 ) {
      rv = EPIPE;
      break;
    }

    unsigned select_len = chans_open;   // number of channels to select from

    if ( maybe_ready_len > 0 && maybe_ready_len < chans_open ) {
      qsort(                            // Sort maybe ready channels first ...
        ref, chans_open, sizeof( chan_select_ref ),
        (qsort_cmp_fn)&chan_select_ref_cmp
      );
      select_len = maybe_ready_len;     // ... and select only from those.
    }
    else {
      // Otherwise, either no or all channels are ready, so there's no need to
      // sort them.
    }

    struct timespec const *might_as_well_wait_time = NULL;
    if ( chans_open == 1 ) {            // Degenerate case.
      selected_ref = ref;
      might_as_well_wait_time = abs_time;
    }
    else if ( maybe_ready_len == 0 && is_blocking ) {
      // None of the channels may be ready and we should wait -- so wait.
      PTHREAD_MUTEX_LOCK( &select_mtx );
      if ( pthread_cond_wait_wrapper( &select_obs.chan_ready, &select_mtx,
                                      abs_time ) == ETIMEDOUT ) {
        rv = ETIMEDOUT;
      }
      PTHREAD_MUTEX_UNLOCK( &select_mtx );
      if ( rv == 0 ) {                  // A channel became ready: find it.
        for ( unsigned i = 0; i < select_len; ++i ) {
          if ( select_obs.chan == ref[i].chan ) {
            selected_ref = &ref[i];
            break;
          }
        } // for
        assert( selected_ref != NULL );
      }
    }
    else {                              // Some or all may be ready: pick one.
      if ( seed == 0 )
        seed = rand_seed( abs_time );
      selected_ref = &ref[ rand_r( &seed ) % (int)select_len ];
    }

    if ( selected_ref != NULL ) {
      struct chan *const selected_chan = selected_ref->chan;
      rv = selected_ref->dir == CHAN_RECV ?
        selected_chan->buf_cap > 0 ?
          chan_buf_recv(
            selected_chan, recv_buf[ selected_ref->param_idx ],
            might_as_well_wait_time
          ) :
          chan_unbuf_recv(
            selected_chan, recv_buf[ selected_ref->param_idx ],
            might_as_well_wait_time
          )
      :
        selected_chan->buf_cap > 0 ?
          chan_buf_send(
            selected_chan, send_buf[ selected_ref->param_idx ],
            might_as_well_wait_time
          ) :
          chan_unbuf_send(
            selected_chan, send_buf[ selected_ref->param_idx ],
            might_as_well_wait_time
          );
    }

    if ( is_blocking ) {
      for ( unsigned i = 0; i < recv_len; ++i )
        chan_remove_obs( recv_chan[i], CHAN_RECV, &select_obs );
      for ( unsigned i = 0; i < send_len; ++i )
        chan_remove_obs( send_chan[i], CHAN_SEND, &select_obs );
    }

    // If rv is:
    //
    //  + 0: we received from or sent to the selected channel.
    //
    //  + ETIMEDOUT: we timed out.
    //
    // For either of those, we're done; if rv is:
    //
    //  + EAGAIN: the selected channel wasn't ready: try again.
    //
    //  + EPIPE: the selected channel was closed between when we called
    //    chan_select_init() an when we called either chan_recv() or
    //    chan_send().  However, if there's at least one other channel that may
    //    still be open, try again.
  } while ( rv == EAGAIN || (rv == EPIPE && chans_open > 1) );

  if ( selected_ref == NULL ) {
    if ( rv != 0 )
      errno = rv;
    rv = -1;
  }
  else {
    rv = selected_ref->dir == CHAN_RECV ?
      CHAN_RECV( selected_ref->param_idx ) :
      CHAN_SEND( selected_ref->param_idx );
  }

  if ( is_blocking ) {
    chan_obs_cleanup( &select_obs );
    PTHREAD_MUTEX_DESTROY( &select_mtx );
  }
  if ( ref != stack_ref )
    free( ref );

  return rv;
}

int chan_send( struct chan *chan, void const *send_buf,
               struct timespec const *duration ) {
  assert( chan != NULL );

  struct timespec abs_ts;
  struct timespec const *const abs_time = ts_dur_to_abs( duration, &abs_ts );

  if ( chan->buf_cap > 0 ) {
    if ( unlikely( send_buf == NULL ) )
      return EINVAL;
    return chan_buf_send( chan, send_buf, abs_time );
  }

  if ( unlikely( chan->msg_size > 0 && send_buf == NULL ) )
    return EINVAL;
  return chan_unbuf_send( chan, send_buf, abs_time );
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
