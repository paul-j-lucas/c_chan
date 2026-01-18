/*
**      c_chan -- Channels Library for C
**      src/c_chan.c
**
**      Copyright (C) 2025-2026  Paul J. Lucas
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
typedef enum    chan_dir              chan_dir;
typedef struct  chan_select_init_args chan_select_init_args;
typedef struct  chan_select_ref       chan_select_ref;
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
 * Contains arguments updated by chan_select_init().
 */
struct chan_select_init_args {
  unsigned  chans_open;                 ///< Number of channels open.
  unsigned  chans_maybe_ready;          ///< Subset that may be ready.
  unsigned  ref_len;                    ///< Length of ref array.
};

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

NODISCARD
static int  chan_unbuf_recv( struct chan*, void*, struct timespec const* ),
            chan_unbuf_send( struct chan*, void const*,
                             struct timespec const* ),
            chan_wait( struct chan*, chan_dir, struct timespec const* );

static void chan_unbuf_release( struct chan*, chan_dir );


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

/**
 * Checks whether \a chan is "hard closed."
 *
 * @remarks A non-empty buffered channel can still be received from even if
 * it's closed, so \a chan shouldn't be considered "hard closed" if it's such a
 * channel.
 *
 * @param chan The \ref chan to check.
 * @param dir The direction of \a chan to check.
 * @return Returns `true` only if \a chan is "hard closed."
 */
NODISCARD
static inline bool chan_is_hard_closed( struct chan const *chan,
                                        chan_dir dir ) {
  return  chan->is_closed &&
          (dir == CHAN_SEND || chan->buf_cap == 0 || chan->buf.ring_len == 0);
}

/**
 * Gets whether a channel is ready.
 *
 * @param chan The channel to check.
 * @param dir The direction of \a chan.
 * @return Returns `true` only if \a chan is ready.
 */
NODISCARD
static inline bool chan_is_ready( struct chan const *chan, chan_dir dir ) {
  if ( chan->buf_cap == 0 )
    return chan->wait_cnt[ !dir ] > 0;
  return dir == CHAN_RECV ?
    chan->buf.ring_len > 0 :
    chan->buf.ring_len < chan->buf_cap;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Adds \a add_obs as an observer of \a chan.
 *
 * @param chan Then \ref chan to add \a add_obs to.
 * @param dir The direction of \a chan.
 * @param add_obs The observer to add.
 * @return Returns `true` upon success or `false` only if `malloc`(3) failed.
 *
 * @warning \ref chan::mtx _must_ be locked before calling this function.
 *
 * @sa chan_remove_obs()
 * @sa chan_select_init()
 */
NODISCARD
static bool chan_add_obs( struct chan *chan, chan_dir dir,
                          chan_impl_obs *add_obs ) {
  assert( chan != NULL );
  assert( add_obs != NULL );

  chan_impl_link *const new_link = malloc( sizeof( chan_impl_link ) );
  if ( unlikely( new_link == NULL ) )
    return false;
  *new_link = (chan_impl_link){
    .obs = add_obs,
    .next = chan->head_link[ dir ].next
  };
  chan->head_link[ dir ].next = new_link;

  ++chan->wait_cnt[ dir ];
  return true;
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
 *  + `EAGAIN` if no message is available and \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
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
      memcpy( recv_buf, chan_buf_at( chan, chan->buf.ring_idx[ CHAN_RECV ] ),
              chan->msg_size );
      chan->buf.ring_idx[ CHAN_RECV ] =
        (chan->buf.ring_idx[ CHAN_RECV ] + 1) % chan->buf_cap;
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
 *  + `EAGAIN` if no message can be sent and \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
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
      memcpy( chan_buf_at( chan, chan->buf.ring_idx[ CHAN_SEND ] ), send_buf,
              chan->msg_size );
      chan->buf.ring_idx[ CHAN_SEND ] =
        (chan->buf.ring_idx[ CHAN_SEND ] + 1) % chan->buf_cap;
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
 */
static void chan_obs_init( chan_impl_obs *obs, pthread_mutex_t *pmtx ) {
  assert( obs != NULL );
  obs->chan = NULL;
  PTHREAD_COND_INIT( &obs->chan_ready, /*attr=*/NULL );
  obs->pmtx = pmtx;
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

  PTHREAD_MUTEX_LOCK( &chan->mtx );

  chan_impl_link *curr_link = &chan->head_link[ dir ];
  do {
    chan_impl_link *const next_link = curr_link->next;
    assert( next_link != NULL );
    if ( next_link->obs == remove_obs ) {
      curr_link->next = next_link->next;
      free( next_link );
      break;
    }
    curr_link = next_link;
  } while ( curr_link != NULL );

  --chan->wait_cnt[ dir ];
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
  assert( curr_link != NULL );
}

/**
 * Initializes \a ref for chan_select().
 *
 * @param ref The array of channel references to initialize.
 * @param chan_len The length of \a chan.
 * @param chan The channels to initialize from.  May be `NULL` only if \a
 * chan_len is 0.
 * @param dir The common direction of \a chan.
 * @param add_obs The observer to add to each channel.  If `NULL`, the select
 * is non-blocking.
 * @param csi The chan_select_init_args to use.
 * @return Returns `true` only upon success or `false` on failure.
 */
NODISCARD
static bool chan_select_init( chan_select_ref ref[],
                              unsigned chan_len, struct chan *chan[chan_len],
                              chan_dir dir, chan_impl_obs *add_obs,
                              chan_select_init_args *csi ) {
  assert( ref != NULL );
  assert( csi != NULL );
  if ( chan_len == 0 )
    return true;
  assert( chan != NULL );

  unsigned i = 0;

  for ( ; i < chan_len; ++i ) {
    bool add_failed = false;
    bool is_ready = false;
    PTHREAD_MUTEX_LOCK( &chan[i]->mtx );

    bool const is_hard_closed = chan_is_hard_closed( chan[i], dir );
    if ( !is_hard_closed ) {
      is_ready = chan_is_ready( chan[i], dir );
      if ( add_obs != NULL )
        add_failed = !chan_add_obs( chan[i], dir, add_obs );
    }

    PTHREAD_MUTEX_UNLOCK( &chan[i]->mtx );

    if ( unlikely( add_failed ) )
      goto remove_already_added;
    if ( is_hard_closed )
      continue;
    ++csi->chans_open;
    if ( is_ready || add_obs != NULL ) {
      ref[ csi->ref_len++ ] = (chan_select_ref){
        .chan = chan[i],
        .dir = dir,
        .param_idx = (unsigned short)i,
        .maybe_ready = is_ready
      };
    }
    if ( is_ready )
      ++csi->chans_maybe_ready;
  } // for

  return true;

remove_already_added:
  //
  // The adding of add_obs as an observer for chan[i] failed: remove add_obs as
  // an observer from all chan[j] for j < i.
  //
  for ( unsigned j = 0; j < i; ++j )
    chan_remove_obs( chan[j], dir, add_obs );
  return false;
}

/**
 * Performs the receive or send on a channel for chan_select().
 *
 * @param ref The chan_select_ref to use.
 * @param recv_buf An array of zero or more pointers to buffers to receive
 * into.  May be `NULL`.
 * @param send_buf An array of zero or more pointers to buffers to send from.
 * May be `NULL`.
 * @param abs_time When to wait until. If `NULL`, it's considered now (does not
 * wait); if \ref CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EAGAIN` if no message is available and \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `ETIMEDOUT` if it's now \a abs_time or later.
 */
static int chan_select_io( chan_select_ref const *ref,
                           void *recv_buf[], void const *send_buf[],
                           struct timespec const *abs_time ) {
  return ref->dir == CHAN_RECV ?
    ref->chan->buf_cap > 0 ?
      chan_buf_recv( ref->chan, recv_buf[ ref->param_idx ], abs_time ) :
      chan_unbuf_recv( ref->chan, recv_buf[ ref->param_idx ], abs_time )
  :
    ref->chan->buf_cap > 0 ?
      chan_buf_send( ref->chan, send_buf[ ref->param_idx ], abs_time ) :
      chan_unbuf_send( ref->chan, send_buf[ ref->param_idx ], abs_time );
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

  for ( chan_impl_link *curr_link = &chan->head_link[ dir ]; curr_link != NULL;
        curr_link = curr_link->next ) {
    chan_impl_obs *const obs = curr_link->obs;
    if ( obs->pmtx != NULL ) {
      PTHREAD_MUTEX_LOCK( obs->pmtx );
      obs->chan = chan;
    }
    PERROR_EXIT_IF( (*pthread_cond_fn)( &obs->chan_ready ) != 0, EX_IOERR );
    if ( obs->pmtx != NULL )
      PTHREAD_MUTEX_UNLOCK( obs->pmtx );
  } // for
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
 *  + `EAGAIN` if no message is available and \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
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
        // See the big comment in chan_unbuf_send.
        chan->unbuf.is_copy_done[ CHAN_SEND ] = true;
        PTHREAD_COND_SIGNAL( &chan->unbuf.copy_done[ CHAN_SEND ] );
        chan->unbuf.is_copy_done[ CHAN_RECV ] = false;
        do {                            // guard against spurious wake-ups
          PTHREAD_COND_WAIT( &chan->unbuf.copy_done[ CHAN_RECV ], &chan->mtx );
        } while ( !chan->unbuf.is_copy_done[ CHAN_RECV ] );
        chan->unbuf.is_copy_done[ CHAN_SEND ] = true;
        PTHREAD_COND_SIGNAL( &chan->unbuf.copy_done[ CHAN_SEND ] );
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
 *  + `EAGAIN` if no message can be sent and \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
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
        //
        // The sender has to block to allow the receiver to do something with
        // the message, before attempting to send another message. To
        // understand the problem, consider the following sequence of events
        // between two threads where each thread is in a loop, one sending and
        // the other receiving:
        //
        // 1. On thread 1, chan_unbuf_recv is called on a channel, but no
        //    sender is present, so it waits.
        //
        // 2. On thread 2, chan_unbuf_send is called, sees a receiver is
        //    waiting, copies the message immediately, and returns.
        //
        // 3. The kernel's task scheduler arbitrarily decides to schedule
        //    thread 2 again immediately. As stated, it's in a loop, so
        //    chan_unbuf_send is called again, sees a receiver is still
        //    "waiting" (even though the message was already copied), and
        //    immediately copies a new message overwriting the previous
        //    message!
        //
        // Step 3 can happen any number of times overwriting messages before
        // the scheduler could decide to run thread 1. Note that the same thing
        // can happen with the sender and receiver roles reversed, i.e., the
        // receiver could receive the same message multiple times thinking it's
        // a new message since the sender isn't given a chance to run.
        //
        // What's needed is a way to force thread 2 to block after sending a
        // message and wait on a condition variable. Since it's blocked, the
        // scheduler won't schedule it again and it therefore will schedule
        // thread 1 to allow its chan_unbuf_recv to return and allow its loop
        // to do whatever with the message.
        //
        // This is what the copy_done condition variable is for. Additionally,
        // both calls to pthread_cond_signal are necessary to implement a
        // handshake between the two threads.
        //
        chan->unbuf.is_copy_done[ CHAN_RECV ] = true;
        PTHREAD_COND_SIGNAL( &chan->unbuf.copy_done[ CHAN_RECV ] );
        chan->unbuf.is_copy_done[ CHAN_SEND ] = false;
        do {                            // guard against spurious wake-ups
          PTHREAD_COND_WAIT( &chan->unbuf.copy_done[ CHAN_SEND ], &chan->mtx );
        } while ( !chan->unbuf.is_copy_done[ CHAN_SEND ] );
        chan->unbuf.is_copy_done[ CHAN_RECV ] = true;
        PTHREAD_COND_SIGNAL( &chan->unbuf.copy_done[ CHAN_RECV ] );
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
 *  + `EAGAIN` if \a abs_time is `NULL`; or:
 *  + `EPIPE` if \a chan is closed; or:
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
  int const rv = pthread_cond_wait_wrapper( &chan->self_obs[ dir ].chan_ready,
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
 * @return Returns a pseudo-random unsigned integer to be used as a seed.
 */
static unsigned rand_seed( struct timespec const *abs_time ) {
  if ( abs_time != NULL && abs_time != CHAN_NO_TIMEOUT )
    return (unsigned)abs_time->tv_nsec;
  struct timespec now;
  CLOCK_GETTIME( CLOCK_REALTIME, &now );
  return (unsigned)now.tv_nsec;
}

/**
 * Converts a relative time (duration) to an absolute time in the future
 * relative to now.
 *
 * @param rel_time The relative time to convert.  May be `NULL` to mean do not
 * wait or \ref CHAN_NO_TIMEOUT to mean wait indefinitely.
 * @param abs_time A pointer to receive the absolute time, but only if \a
 * rel_time is neither `NULL` nor \ref CHAN_NO_TIMEOUT.
 * @return Returns \a rel_time if either `NULL` or \ref CHAN_NO_TIMEOUT, or \a
 * abs_time otherwise.
 */
NODISCARD
static struct timespec const* ts_rel_to_abs( struct timespec const *rel_time,
                                             struct timespec *abs_time ) {
  assert( abs_time != NULL );

  if ( rel_time == NULL || rel_time == CHAN_NO_TIMEOUT )
    return rel_time;

  struct timespec now;
  CLOCK_GETTIME( CLOCK_REALTIME, &now );

  *abs_time = (struct timespec){
    .tv_sec  = now.tv_sec  + rel_time->tv_sec,
    .tv_nsec = now.tv_nsec + rel_time->tv_nsec
  };

  return abs_time;
}

/** @} */

////////// extern functions ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE

void chan_cleanup( struct chan *chan, void (*msg_cleanup_fn)( void* ) ) {
  if ( chan == NULL )
    return;

  if ( chan->buf_cap > 0 ) {
    if ( chan->buf.ring_len > 0 && msg_cleanup_fn != NULL ) {
      unsigned recv_idx = chan->buf.ring_idx[ CHAN_RECV ];
      for ( unsigned i = 0; i < chan->buf.ring_len; ++i ) {
        (*msg_cleanup_fn)( chan_buf_at( chan, recv_idx ) );
        recv_idx = (recv_idx + 1) % chan->buf_cap;
      }
    }
    free( chan->buf.ring_buf );
  }
  else {
    PTHREAD_COND_DESTROY( &chan->unbuf.copy_done[ CHAN_RECV ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.copy_done[ CHAN_SEND ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.not_busy[ CHAN_RECV ] );
    PTHREAD_COND_DESTROY( &chan->unbuf.not_busy[ CHAN_SEND ] );
  }

  assert( chan->head_link[ CHAN_RECV ].obs == &chan->self_obs[ CHAN_RECV ] );
  assert( chan->head_link[ CHAN_SEND ].obs == &chan->self_obs[ CHAN_SEND ] );

  chan_obs_cleanup( &chan->self_obs[ CHAN_RECV ] );
  chan_obs_cleanup( &chan->self_obs[ CHAN_SEND ] );
  PTHREAD_MUTEX_DESTROY( &chan->mtx );
}

void chan_close( struct chan *chan ) {
  assert( chan != NULL );
  PTHREAD_MUTEX_LOCK( &chan->mtx );
  if ( !chan->is_closed ) {
    chan->is_closed = true;
    chan_signal_all_obs( chan, CHAN_RECV, &pthread_cond_broadcast );
    chan_signal_all_obs( chan, CHAN_SEND, &pthread_cond_broadcast );
    if ( chan->buf_cap == 0 ) {
      chan->unbuf.is_copy_done[ CHAN_RECV ] = true;
      PTHREAD_COND_BROADCAST( &chan->unbuf.copy_done[ CHAN_RECV ] );
      chan->unbuf.is_copy_done[ CHAN_SEND ] = true;
      PTHREAD_COND_BROADCAST( &chan->unbuf.copy_done[ CHAN_SEND ] );
      PTHREAD_COND_BROADCAST( &chan->unbuf.not_busy[ CHAN_RECV ] );
      PTHREAD_COND_BROADCAST( &chan->unbuf.not_busy[ CHAN_SEND ] );
    }
  }
  PTHREAD_MUTEX_UNLOCK( &chan->mtx );
}

int chan_init( struct chan *chan, unsigned buf_cap, size_t msg_size ) {
  assert( chan != NULL );

  if ( buf_cap > 0 ) {
    if ( unlikely( msg_size == 0 ) ) {
      errno = EINVAL;
      return EINVAL;
    }
    chan->buf.ring_buf = malloc( buf_cap * msg_size );
    if ( unlikely( chan->buf.ring_buf == NULL ) )
      return ENOMEM;                    // malloc sets errno to ENOMEM
    chan->buf.ring_idx[ CHAN_RECV ] = 0;
    chan->buf.ring_idx[ CHAN_SEND ] = 0;
    chan->buf.ring_len = 0;
  }
  else {
    chan->unbuf.recv_buf = NULL;
    PTHREAD_COND_INIT( &chan->unbuf.copy_done[ CHAN_RECV ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.copy_done[ CHAN_SEND ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.not_busy[ CHAN_RECV ], /*attr=*/NULL );
    PTHREAD_COND_INIT( &chan->unbuf.not_busy[ CHAN_SEND ], /*attr=*/NULL );
    chan->unbuf.is_busy[ CHAN_RECV ] = false;
    chan->unbuf.is_busy[ CHAN_SEND ] = false;
    chan->unbuf.is_copy_done[ CHAN_RECV ] = false;
    chan->unbuf.is_copy_done[ CHAN_SEND ] = false;
  }

  chan->buf_cap = buf_cap;
  chan->head_link[ CHAN_RECV ] = (chan_impl_link){
    .obs = &chan->self_obs[ CHAN_RECV ]
  };
  chan->head_link[ CHAN_SEND ] = (chan_impl_link){
    .obs = &chan->self_obs[ CHAN_SEND ]
  };
  chan->is_closed = false;
  chan->msg_size = msg_size;
  PTHREAD_MUTEX_INIT( &chan->mtx, /*attr=*/NULL );
  chan_obs_init( &chan->self_obs[ CHAN_RECV ], /*ptmx=*/NULL );
  chan_obs_init( &chan->self_obs[ CHAN_SEND ], /*ptmx=*/NULL );
  chan->wait_cnt[ CHAN_RECV ] = chan->wait_cnt[ CHAN_SEND ] = 0;

  return 0;
}

int chan_recv( struct chan *chan, void *recv_buf,
               struct timespec const *duration ) {
  assert( chan != NULL );

  struct timespec abs_ts;
  struct timespec const *const abs_time = ts_rel_to_abs( duration, &abs_ts );
  int rv;

  if ( chan->buf_cap > 0 ) {
    if ( unlikely( recv_buf == NULL ) )
      rv = EINVAL;
    else
      rv = chan_buf_recv( chan, recv_buf, abs_time );
  }
  else {
    if ( unlikely( chan->msg_size > 0 && recv_buf == NULL ) )
      rv = EINVAL;
    else
      rv = chan_unbuf_recv( chan, recv_buf, abs_time );
  }

  if ( rv > 0 )
    errno = rv;
  return rv;
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
  struct timespec const *const  abs_time = ts_rel_to_abs( duration, &abs_ts );
  chan_select_init_args         csi;
  bool const                    is_blocking = duration != NULL;
  unsigned                      seed = 0;     // random number seed
  chan_impl_obs                 select_obs;   // observer for this select
  pthread_mutex_t               select_mtx;   // mutex for select_obs
  chan_select_ref const        *selected_ref; // reference to selected channel
  int                           rv;

  if ( is_blocking ) {
    PTHREAD_MUTEX_INIT( &select_mtx, /*attr=*/NULL );
    chan_obs_init( &select_obs, &select_mtx );
  }

  do {
    csi = (chan_select_init_args){ 0 };
    rv = 0;
    selected_ref = NULL;

    if ( !chan_select_init( ref, recv_len, recv_chan, CHAN_RECV,
                            is_blocking ? &select_obs : NULL, &csi ) ) {
      break;
    }

    if ( !chan_select_init( ref, send_len, send_chan, CHAN_SEND,
                            is_blocking ? &select_obs : NULL, &csi ) ) {
      if ( is_blocking ) {
        for ( unsigned i = 0; i < recv_len; ++i )
          chan_remove_obs( recv_chan[i], CHAN_RECV, &select_obs );
      }
      break;
    }

    if ( csi.chans_open == 0 ) {
      rv = EPIPE;
      break;
    }

    struct chan *selected_chan = NULL;
    struct timespec const *select_abs_time = NULL;

    if ( csi.chans_open == 1 ) {        // Degenerate case.
      if ( csi.ref_len > 0 )
        selected_ref = ref;
      select_abs_time = abs_time;
    }
    else if ( csi.chans_maybe_ready == 0 && is_blocking ) {
      // None of the channels may be ready and we should wait -- so wait.
      PTHREAD_MUTEX_LOCK( &select_mtx );
      select_obs.chan = NULL;
      do {
        if ( pthread_cond_wait_wrapper( &select_obs.chan_ready, &select_mtx,
                                        abs_time ) == ETIMEDOUT ) {
          rv = ETIMEDOUT;
        }
      } while ( rv == 0 && select_obs.chan == NULL );
      // Must copy select_obs.chan to a local variable while mutex is locked.
      selected_chan = select_obs.chan;
      PTHREAD_MUTEX_UNLOCK( &select_mtx );

      if ( rv == 0 ) {                  // A channel became ready: find it.
        for ( unsigned i = 0; i < csi.chans_open; ++i ) {
          if ( selected_chan == ref[i].chan ) {
            selected_ref = &ref[i];
            break;
          }
        } // for
        assert( selected_ref != NULL );
      }
    }
    else {                              // Some or all may be ready: pick one.
      unsigned select_len;
      if ( csi.chans_maybe_ready > 0 && csi.chans_maybe_ready < csi.ref_len ) {
        // Only some channels may be ready: sort those first and select only
        // from those.
        qsort(
          ref, csi.ref_len, sizeof( chan_select_ref ),
          (qsort_cmp_fn)&chan_select_ref_cmp
        );
        select_len = csi.chans_maybe_ready;
      }
      else {
        // Otherwise, either no or all channels are ready, so there's no need
        // to sort them.
        select_len = csi.chans_open;
      }

      if ( seed == 0 )
        seed = rand_seed( abs_time );
      selected_ref = &ref[ (unsigned)rand_r( &seed ) % select_len ];
    }

    if ( selected_ref != NULL )
      rv = chan_select_io( selected_ref, recv_buf, send_buf, select_abs_time );

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
    //    chan_select_init() and when we called either chan_recv() or
    //    chan_send().  However, if there's at least one other channel that may
    //    still be open, try again.
  } while ( rv == EAGAIN || (rv == EPIPE && csi.chans_open > 1) );

  if ( selected_ref == NULL ) {
    if ( rv > 0 )
      errno = rv;
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
  struct timespec const *const abs_time = ts_rel_to_abs( duration, &abs_ts );
  int rv;

  if ( chan->buf_cap > 0 ) {
    if ( unlikely( send_buf == NULL ) )
      rv = EINVAL;
    else
      rv = chan_buf_send( chan, send_buf, abs_time );
  }
  else {
    if ( unlikely( chan->msg_size > 0 && send_buf == NULL ) )
      rv = EINVAL;
    else
      rv = chan_unbuf_send( chan, send_buf, abs_time );
  }

  if ( rv > 0 )
    errno = rv;
  return rv;
}

/// @endcond

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
