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
#include <sys/time.h>                   /* for gettimeofday(2) */
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

/**
 * For unbuffered channels, a more mnemonic name for specifying which observer
 * (receive or send) is meant.
 *
 * @sa chan_dir
 */
#define CHAN_UNBUF_SEND_DONE  CHAN_RECV

/**
 * @copydoc CHAN_UNBUF_SEND_DONE
 */
#define CHAN_UNBUF_RECV_WAIT  CHAN_SEND

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
  struct channel *chan;                 ///< The \ref channel referred to.

  /**
   * Index into either \p recv_chan and \p recv_buf, or \p send_chan and \p
   * send_buf parameters of chan_select().
   */
  unsigned short  param_idx;

  /**
   * Indicates whether param_idx refers to either the \p recv_chan and \p
   * recv_buf, or \p send_chan and \p send_buf parameters of chan_select().
   */
  chan_dir        dir;

  bool            maybe_ready;          ///< Is \ref chan maybe ready?
};

////////// local functions ////////////////////////////////////////////////////

static void chan_notify( struct channel*, chan_dir,
                         int (*)( pthread_cond_t* ) );

NODISCARD
static int  chan_unbuf_recv( struct channel*, void*, struct timespec const* );

NODISCARD
static int  chan_unbuf_send( struct channel*, void const*,
                             struct timespec const* );

NODISCARD
static int  chan_wait( struct channel*, chan_dir, struct timespec const* );

NODISCARD
static int  pthread_cond_wait_wrapper( pthread_cond_t*, pthread_mutex_t*,
                                       struct timespec const* );

// local variables

/// A variable to which \ref CHAN_NO_TIMEOUT can point.
static struct timespec const  CHAN_NO_TIMEOUT_TIMESPEC;

/// @cond DOXYGEN_IGNORE

// extern variables
struct timespec const *const CHAN_NO_TIMEOUT = &CHAN_NO_TIMEOUT_TIMESPEC;

/// @endcond

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
 * Receives a message from a buffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
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
static int chan_buf_recv( struct channel *chan, void *recv_buf,
                          struct timespec const *abs_time ) {
  int rv = 0;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  do {
    if ( chan->buf.ring_len > 0 ) {
      memcpy( recv_buf, chan_buf_at( chan, chan->buf.recv_idx ),
              chan->msg_size );
      chan->buf.recv_idx = (chan->buf.recv_idx + 1) % chan->buf_cap;
      if ( chan->buf.ring_len-- == chan->buf_cap )
        chan_notify( chan, CHAN_BUF_NOT_FULL, &pthread_cond_signal );
      break;
    }
    // Since we can still read from a closed, non-empty, buffered channel, this
    // check is after the above.
    if ( chan->is_closed )
      rv = EPIPE;
    else if ( abs_time == NULL )        // No sender and shouldn't wait.
      rv = EAGAIN;
    else
      rv = chan_wait( chan, CHAN_BUF_NOT_EMPTY, abs_time );
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends a message to a buffered \ref channel.
 *
 * @param chan The \ref channel to send to.
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
static int chan_buf_send( struct channel *chan, void const *send_buf,
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
        chan_notify( chan, CHAN_BUF_NOT_EMPTY, &pthread_cond_signal );
      break;
    }
    else if ( abs_time == NULL ) {      // Channel is full and shouldn't wait.
      rv = EAGAIN;
    }
    else {
      rv = chan_wait( chan, CHAN_BUF_NOT_FULL, abs_time );
    }
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Signals all of a channel's observers' conditions.
 *
 * @param chan The \ref channel to notify about.
 * @param dir Whether to notify about a receive or send condition.
 * @param pthread_cond_fn The `pthread_cond_t` function to call, either
 * **pthread_cond_signal**(3) or **pthread_cond_broadcast**(3).
 *
 * @warning \ref channel::mtx _must_ be locked before calling this function.
 */
static void chan_notify( struct channel *chan, chan_dir dir,
                         int (*pthread_cond_fn)( pthread_cond_t* ) ) {
  if ( chan->wait_cnt[ dir ] == 0 )     // Nobody is waiting.
    return;

  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  for ( chan_obs_impl *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    obs->chan = chan;
    PERROR_EXIT_IF( (*pthread_cond_fn)( &obs->chan_ready ) != 0, EX_IOERR );
    next_obs = obs->next;
    if ( next_obs != NULL ) {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ...
    }
    if ( pmtx != NULL )
      PTHREAD_MUTEX_UNLOCK( pmtx );     // ... before unlocking previous.
  } // for
}

/**
 * Adds \a add_obs as an observer of \a chan.
 *
 * @param chan Then \ref channel to add \a add_obs to.
 * @param dir The direction of \a chan.
 * @param add_obs The observer to add.
 *
 * @warning \ref channel::mtx _must_ be locked before calling this function.
 *
 * @sa chan_obs_remove()
 * @sa chan_select_init()
 */
static void chan_obs_add( struct channel *chan, chan_dir dir,
                          chan_obs_impl *add_obs ) {
  assert( chan != NULL );
  assert( add_obs != NULL );

  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  for ( chan_obs_impl *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    next_obs = obs->next;
    if ( next_obs == NULL ) {           // At end of list.
      obs->next = add_obs;
    }
    else {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ... (1)
      if ( add_obs->key < next_obs->key ) {
        obs->next = add_obs;
        add_obs->next = next_obs;
        PTHREAD_MUTEX_UNLOCK( next_pmtx );
        next_obs = NULL;                // Causes loop to exit ... (2)
      }
    }
    if ( pmtx != NULL )                 // (2) ... yet still runs this code.
      PTHREAD_MUTEX_UNLOCK( pmtx );     // (1) ... before unlocking previous.
  } // for

  ++chan->wait_cnt[ dir ];
}

/**
 * Initializes a \ref chan_obs_impl.
 *
 * @param obs The \ref chan_obs_impl to initialize.
 * @param pmtx The mutex to use, if any.
 *
 * @sa chan_obs_init_key()
 */
static void chan_obs_init( chan_obs_impl *obs, pthread_mutex_t *pmtx ) {
  assert( obs != NULL );

  obs->chan = NULL;
  PTHREAD_COND_INIT( &obs->chan_ready, /*attr=*/0 );
  obs->key  = 0;
  obs->next = NULL;
  obs->pmtx = pmtx;
}

/**
 * Initializes a \ref chan_obs_impl key.
 *
 * @remarks An observer has an arbitrary, comparable key so the linked list of
 * a channel's observers can be in ascending key order.  The linked list is
 * traversed using hand-over-hand locking.  Since an observer can be in
 * multiple channels' lists, the ordering ensures that pairs of mutexes are
 * always locked in the same order on every list to avoid deadlocks.
 *
 * @param obs The \ref chan_obs_impl to initialize the key of.
 *
 * @sa chan_obs_init()
 * @sa chan_select_init()
 */
static void chan_obs_init_key( chan_obs_impl *obs ) {
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
 * @param chan Then \ref channel to remove \a remove_obs from.
 * @param dir The direction of \a chan.
 * @param remove_obs The observer to remove.
 *
 * @warning \ref channel::mtx _must_ be locked before calling this function.
 *
 * @sa chan_obs_add()
 * @sa obs_remove_all_chan()
 */
static void chan_obs_remove( struct channel *chan, chan_dir dir,
                             chan_obs_impl *remove_obs ) {
  assert( chan != NULL );
  assert( remove_obs != NULL );

  // Whether the channel is closed now doesn't matter since it may have been
  // open when the observer was added.

  pthread_mutex_t *pmtx = NULL, *next_pmtx = NULL;

  for ( chan_obs_impl *obs = &chan->observer[ dir ], *next_obs; obs != NULL;
        obs = next_obs, pmtx = next_pmtx ) {
    next_obs = obs->next;
    if ( next_obs == remove_obs ) {
      --chan->wait_cnt[ dir ];
      // remove_obs is an observer in our caller's stack frame, i.e., this
      // thread, so there's no need to lock next_obs->pmtx.
      obs->next = next_obs->next;
      next_obs = NULL;                  // Causes loop to exit ... (1)
    }
    else if ( next_obs != NULL ) {
      next_pmtx = next_obs->pmtx;       // Do hand-over-hand locking:
      PTHREAD_MUTEX_LOCK( next_pmtx );  // lock next mutex ... (2)
    }
    if ( pmtx != NULL )                 // (1) ... yet still runs this code.
      PTHREAD_MUTEX_UNLOCK( pmtx );     // (2) ... before unlocking previous.
  } // for
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
                                  struct channel *chan[chan_len], chan_dir dir,
                                  chan_obs_impl *add_obs ) {
  assert( ref != NULL );
  assert( pref_len != NULL );
  assert( chan_len == 0 || chan != NULL );

  unsigned maybe_ready_len = 0;

  if ( chan != NULL ) {
    for ( unsigned i = 0; i < chan_len; ++i ) {
      bool is_ready = false;
      PTHREAD_MUTEX_LOCK( &chan[i]->mtx );

      if ( !chan[i]->is_closed ) {
        is_ready = chan_is_buffered( chan[i] ) ?
          (dir == CHAN_RECV ?
            chan[i]->buf.ring_len > 0 :
            chan[i]->buf.ring_len < chan[i]->buf_cap) :
          chan[i]->wait_cnt[ !dir ] > 0;

        if ( add_obs != NULL )
          chan_obs_add( chan[i], dir, add_obs );

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
 * Receives a message from an unbuffered \ref channel.
 *
 * @param chan The \ref channel to receive from.
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
static int chan_unbuf_recv( struct channel *chan, void *recv_buf,
                            struct timespec const *abs_time ) {
  int rv = 0;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  do {
    if ( chan->is_closed ) {
      rv = EPIPE;
    }
    else if ( abs_time == NULL &&
              (chan->wait_cnt[ CHAN_SEND ] == 0 ||
               chan->unbuf.recv_buf != NULL) ) {
      rv = EAGAIN;
    }
    else if ( chan->unbuf.recv_buf == NULL ) {
      chan->unbuf.recv_buf = recv_buf;
      chan_notify( chan, CHAN_UNBUF_RECV_WAIT, &pthread_cond_signal );

      // Wait for a sender to copy the message.
      rv = chan_wait( chan, CHAN_UNBUF_SEND_DONE, abs_time );

      chan->unbuf.recv_buf = NULL;
      PTHREAD_COND_SIGNAL( &chan->unbuf.recv_buf_is_null );
      break;
    }
    else {
      // Some other thread has called chan_unbuf_recv() that has already set
      // recv_buf and is waiting for a sender.  We must wait for that other
      // thread to reset recv_buf.
      rv = pthread_cond_wait_wrapper( &chan->unbuf.recv_buf_is_null,
                                      &chan->mtx, abs_time );
    }
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Sends a message to an unbuffered \ref channel.
 *
 * @param chan The \ref channel to send to.
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
static int chan_unbuf_send( struct channel *chan, void const *send_buf,
                            struct timespec const *abs_time ) {
  int rv = 0;
  PTHREAD_MUTEX_LOCK( &chan->mtx );

  do {
    if ( chan->is_closed ) {
      rv = EPIPE;
    }
    else if ( chan->unbuf.recv_buf != NULL ) {  // There is a receiver.
      if ( chan->msg_size > 0 )
        memcpy( chan->unbuf.recv_buf, send_buf, chan->msg_size );
      chan_notify( chan, CHAN_UNBUF_SEND_DONE, &pthread_cond_broadcast );
      break;
    }
    else if ( abs_time == NULL ) {      // No receiver and shouldn't wait.
      rv = EAGAIN;
    }
    else {
      rv = chan_wait( chan, CHAN_UNBUF_RECV_WAIT, abs_time );
    }
  } while ( rv == 0 );

  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  return rv;
}

/**
 * Like **pthread_cond_wait**(3)** and **pthread_cond_timedwait**(3) except:
 *  + If \a abs_time is `NULL`, does not wait.
 *  + If \a abs_time is \ref CHAN_NO_TIMEOUT, waits indefinitely.
 *
 * @param chan The \ref channel to wait for.
 * @param dir Whether to wait to receive or send.
 * @param abs_time When to wait until. If \ref CHAN_NO_TIMEOUT, waits
 * indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 *
 * @warning \ref channel::mtx _must_ be locked before calling this function.
 */
NODISCARD
static int chan_wait( struct channel *chan, chan_dir dir,
                      struct timespec const *abs_time ) {
  assert( chan != NULL );
  assert( !chan->is_closed );
  assert( abs_time != NULL );

  ++chan->wait_cnt[ dir ];
  int const rv = pthread_cond_wait_wrapper( &chan->observer[ dir ].chan_ready,
                                            &chan->mtx,
                                            abs_time ) == ETIMEDOUT ? EPIPE : 0;
  --chan->wait_cnt[ dir ];

  return chan->is_closed ? EPIPE : rv;
}

/**
 * Removes \a remove_obs as an observer from all \a chan.
 *
 * @param remove_obs The observer to remove.
 * @param chan_len The length of \a chan.
 * @param chan The array of channels to remove \a remove_obs from.  If \a
 * chan_len is 0, may be `NULL`.
 * @param dir The common direction of \a chan.
 *
 * @sa chan_obs_remove()
 */
static void obs_remove_all_chan( chan_obs_impl *remove_obs, unsigned chan_len,
                                 struct channel *chan[chan_len],
                                 chan_dir dir ) {
  assert( remove_obs != NULL );
  assert( chan_len == 0 || chan != NULL );

  for ( unsigned i = 0; i < chan_len; ++i ) {
    PTHREAD_MUTEX_LOCK( &chan[i]->mtx );
    chan_obs_remove( chan[i], dir, remove_obs );
    PTHREAD_MUTEX_UNLOCK( &chan[i]->mtx );
  } // for
}

/**
 * Like **pthread_cond_wait**(3)** and **pthread_cond_timedwait**(3) except:
 *  + If \a abs_time is \ref CHAN_NO_TIMEOUT, waits indefinitely.
 *  + Checks the return value of **pthread_cond_timedwait**(3) for errors other
 *    than `ETIMEDOUT`.
 *
 * @param cond The condition to wait for.
 * @param mtx The mutex to unlock temporarily.
 * @param abs_time When to wait until.  If \ref CHAN_NO_TIMEOUT, waits
 * indefinitely.
 * @return Returns either 0 only if \a cond was signaled or `ETIMEDOUT` only if
 * it's now \a abs_time or later.
 */
NODISCARD
static int pthread_cond_wait_wrapper( pthread_cond_t *cond,
                                      pthread_mutex_t *mtx,
                                      struct timespec const *abs_time ) {
  assert( cond != NULL );
  assert( mtx != NULL );
  assert( abs_time != NULL );

  if ( abs_time == CHAN_NO_TIMEOUT ) {
    PTHREAD_COND_WAIT( cond, mtx );
    return 0;
  }

  int const pct_rv = pthread_cond_timedwait( cond, mtx, abs_time );
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

/**
 * Converts a duration to an absolute time in the future relative to now.
 *
 * @param duration The duration to convert.  May be `NULL` to mean do not wait
 * or \ref CHAN_NO_TIMEOUT to mean wait indefinitely.
 * @param abs_time A pointer to receive the absolute time, but only if \a
 * duration is neither `NULL` nor \ref CHAN_NO_TIMEOUT.
 * @return Returns \a duration if either `NULL` or \a CHAN_NO_TIMEOUT, or \a
 * abs_time otherwise.
 */
NODISCARD
static struct timespec const* ts_dur_to_abs( struct timespec const *duration,
                                             struct timespec *abs_time ) {
  assert( abs_time != NULL );

  if ( duration == NULL || duration == CHAN_NO_TIMEOUT )
    return duration;

  struct timeval now;
  (void)gettimeofday( &now, /*tzp=*/NULL );

  *abs_time = (struct timespec){
    .tv_sec  = now.tv_sec         + duration->tv_sec,
    .tv_nsec = now.tv_usec * 1000 + duration->tv_nsec
  };

  return abs_time;
}

/** @} */

////////// extern functions ///////////////////////////////////////////////////

void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) ) {
  if ( chan == NULL )
    return;

  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool const is_closed = chan->is_closed;
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( !is_closed ) {
    EPRINTF( PACKAGE ": %s called on open channel\n", __func__ );
    abort();
  }

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

  PTHREAD_COND_DESTROY( &chan->observer[ CHAN_RECV ].chan_ready );
  PTHREAD_COND_DESTROY( &chan->observer[ CHAN_SEND ].chan_ready );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct channel *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  bool const was_already_closed = chan->is_closed;
  chan->is_closed = true;
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  if ( !was_already_closed ) {          // Wake up all waiting threads, if any.
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

  chan_obs_init( &chan->observer[ CHAN_RECV ], /*pmtx=*/NULL );
  chan_obs_init( &chan->observer[ CHAN_SEND ], /*pmtx=*/NULL );

  return true;
}

int chan_recv( struct channel *chan, void *recv_buf,
               struct timespec const *duration ) {
  assert( chan != NULL );
  assert( recv_buf != NULL );

  struct timespec abs_ts;
  struct timespec const *const abs_time = ts_dur_to_abs( duration, &abs_ts );

  return chan_is_buffered( chan ) ?
    chan_buf_recv( chan, recv_buf, abs_time ) :
    chan_unbuf_recv( chan, recv_buf, abs_time );
}

int chan_select( unsigned recv_len, struct channel *recv_chan[recv_len],
                 void *recv_buf[recv_len],
                 unsigned send_len, struct channel *send_chan[send_len],
                 void const *send_buf[send_len],
                 struct timespec const *duration ) {
  assert( recv_len == 0 || (recv_chan != NULL && recv_buf != NULL) );
  assert( send_len == 0 || (send_chan != NULL && send_buf != NULL) );

  unsigned const total_channels = recv_len + send_len;

  chan_select_ref stack_ref[16];
  chan_select_ref *const ref = total_channels <= ARRAY_SIZE( stack_ref ) ?
    stack_ref : malloc( total_channels * sizeof( chan_select_ref ) );

  struct timespec               abs_ts;
  struct timespec const *const  abs_time = ts_dur_to_abs( duration, &abs_ts );
  unsigned                      chans_open;   // number of open channels
  chan_obs_impl                 select_obs;   // observer for this select
  pthread_mutex_t               select_mtx;   // mutex for select_obs
  chan_select_ref const        *selected_ref;
  int                           rv;
  bool const                    wait = duration != NULL;

  if ( wait ) {
    chan_obs_init( &select_obs, &select_mtx );
    chan_obs_init_key( &select_obs );
    PTHREAD_MUTEX_INIT( &select_mtx, /*attr=*/0 );
  }

  do {
    chans_open = 0;
    rv = 0;
    selected_ref = NULL;

    unsigned const maybe_ready_len =    // number of channels that may be ready
      chan_select_init(
        ref, &chans_open, recv_len, recv_chan, CHAN_RECV,
        wait ? &select_obs : NULL
      ) +
      chan_select_init(
        ref, &chans_open, send_len, send_chan, CHAN_SEND,
        wait ? &select_obs : NULL
      );

    if ( chans_open == 0 )
      break;

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

    if ( maybe_ready_len == 0 && wait ) {
      // None of the channels may be ready and we should wait -- so wait.
      PTHREAD_MUTEX_LOCK( &select_mtx );
      if ( pthread_cond_wait_wrapper( &select_obs.chan_ready, &select_mtx,
                                      abs_time ) == ETIMEDOUT ) {
        rv = ETIMEDOUT;
      }
      else {
        // A channel became ready: find it in our list.
        for ( unsigned i = 0; i < select_len; ++i ) {
          if ( select_obs.chan == ref[i].chan ) {
            selected_ref = &ref[i];
            break;
          }
        } // for
        assert( selected_ref != NULL );
      }
      PTHREAD_MUTEX_UNLOCK( &select_mtx );
    }
    else {
      // Some or all channels may be ready: pick one at random.
      static pthread_once_t once = PTHREAD_ONCE_INIT;
      PTHREAD_ONCE( &once, &srand_init );
      selected_ref = &ref[ rand() % (int)select_len ];
    }

    if ( rv == 0 ) {
      rv = selected_ref->dir == CHAN_RECV ?
        chan_recv(
          recv_chan[ selected_ref->param_idx ],
          recv_buf[ selected_ref->param_idx ],
          /*duration=*/NULL
        ) :
        chan_send(
          send_chan[ selected_ref->param_idx ],
          send_buf[ selected_ref->param_idx ],
          /*duration=*/NULL
        );
    }

    if ( wait ) {
      obs_remove_all_chan( &select_obs, recv_len, recv_chan, CHAN_RECV );
      obs_remove_all_chan( &select_obs, send_len, send_chan, CHAN_SEND );
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

  if ( ref != stack_ref )
    free( ref );

  if ( wait ) {
    PTHREAD_COND_DESTROY( &select_obs.chan_ready );
    PTHREAD_MUTEX_DESTROY( &select_mtx );
  }

  if ( selected_ref == NULL )
    return -1;
  return selected_ref->dir == CHAN_RECV ?
    CHAN_RECV( selected_ref->param_idx ) :
    CHAN_SEND( selected_ref->param_idx );
}

int chan_send( struct channel *chan, void const *send_buf,
               struct timespec const *duration ) {
  assert( chan != NULL );

  struct timespec abs_ts;
  struct timespec const *const abs_time = ts_dur_to_abs( duration, &abs_ts );

  if ( chan_is_buffered( chan ) ) {
    assert( send_buf != NULL );
    return chan_buf_send( chan, send_buf, abs_time );
  }

  return chan_unbuf_send( chan, send_buf, abs_time );
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
