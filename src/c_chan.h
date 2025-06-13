/*
**      c_chan -- Channels Library for C
**      src/c_chan.h
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

#ifndef C_CHAN_H
#define C_CHAN_H

/**
 * @file
 * Declares types, macros, and functions to implement Go-like channels in C.
 *
 * @sa Hoare, C. A. R., "Communicating Sequential Processes," Communications of
 * the ACM, 21(8), 1978, pp. 666–677,
 * [doi:10.1145/359576.359585](https://doi.org/10.1145%2F359576.359585).
 */

// local
#include "config.h"                     /* must go first */

/// @cond DOXYGEN_IGNORE

// standard
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>                       /* for timespec */

/// @endcond

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct chan;

////////// implementation /////////////////////////////////////////////////////

/**
 * @defgroup c-chan-implementation-group Implementation API
 * Declares types, macros, and functions for the implementation.
 * @{
 */

typedef struct chan_obs_impl chan_obs_impl;

/**
 * An "observer" for a channel that is used to wait until it's ready.
 *
 * @note This is an implementation detail not part of the public API.
 */
struct chan_obs_impl {
  struct chan      *chan;               ///< The channel being observed.
  pthread_cond_t    chan_ready;         ///< Is \ref chan ready?
  unsigned          key;                ///< A fairly unique key.
  chan_obs_impl    *next;               ///< The next observer, if any.
  pthread_mutex_t  *pmtx;               ///< The mutex to use.
};

/** @} */

////////// public /////////////////////////////////////////////////////////////

/**
 * @defgroup c-chan-public-api-group Public API
 * Declares types and functions for public use.
 * @{
 */

/**
 * For use with cases of a `switch` statement on chan_select() to specify the
 * index of a receive channel.
 *
 * @sa #CHAN_SEND
 */
#define CHAN_RECV(IDX)            ((int)(IDX))

/**
 * For use with cases of a `switch` statement on chan_select() to specify the
 * index of a send channel.
 *
 * @sa #CHAN_RECV
 */
#define CHAN_SEND(IDX)            (1024 + (int)(IDX))

/**
 * A Go-like channel.
 *
 * @sa Hoare, C. A. R., "Communicating Sequential Processes," Communications of
 * the ACM, 21(8), 1978, pp. 666–677,
 * [doi:10.1145/359576.359585](https://doi.org/10.1145/359576.35958).
 */
struct chan {
  union {
    struct {
      void           *ring_buf;         ///< Message ring buffer.
      unsigned        ring_len;         ///< Number of messages in buffer.
      unsigned        recv_idx;         ///< Ring buffer receive index.
      unsigned        send_idx;         ///< Ring buffer send index.
    } buf;
    struct {
      void           *recv_buf;         ///< Where to put a received message.
      pthread_cond_t  recv_buf_is_null; ///< Is `recv_buf` `NULL`?
    } unbuf;
  };

  chan_obs_impl       observer[2];      ///< Receiver (0), Sender (1).
  unsigned short      wait_cnt[2];      ///< Waiting to receive (0) or send (1).

  pthread_mutex_t     mtx;              ///< Channel mutex.
  size_t              msg_size;         ///< Size of a message.
  unsigned            buf_cap;          ///< Channel capacity; 0 = unbuffered.
  bool                is_closed;        ///< Is channel closed?
};

////////// extern variables ///////////////////////////////////////////////////

/**
 * A `timespec` value to wait indefinitely.
 */
extern struct timespec const *const CHAN_NO_TIMEOUT;

////////// extern functions ///////////////////////////////////////////////////

/**
 * Cleans-up a \ref chan.
 *
 * @param chan The \ref chan to clean-up.  If `NULL`, does nothing.
 * @param free_fn For buffered channels only, the function to free unreceived
 * messages, if any.
 *
 * @warning A channel _must_ be closed before it's cleaned-up and no other
 * threads may be using it.  This function can not call chan_close()
 * automatically if it's open because that would result in a race condition
 * since other threads may not finish interacting with it before it's cleaned-
 * up.
 *
 * @sa chan_close()
 * @sa chan_init()
 */
void chan_cleanup( struct chan *chan, void (*free_fn)( void* ) );

/**
 * Closes a channel.
 *
 * @remarks Once a channel is closed, it can no longer be sent to.  For a
 * buffered channel only, queued messages may still be received; an unbuffered
 * channel can no longer be received from.
 *
 * @param chan The \ref chan to close.  If already closed, does nothing.
 *
 * @note A channel _must_ be cleaned-up eventually.
 *
 * @sa chan_cleanup()
 * @sa chan_init()
 */
void chan_close( struct chan *chan );

/**
 * Initializes a channel.
 *
 * @remarks
 * There are two types of channels:
 *
 *  1. **Buffered**: the channel has a buffer of a fixed capacity.
 *
 *     + For senders, new messages may be sent as long as the buffer is not
 *       full.  Once full, senders will wait (unless instructed not to) for it
 *       to become not full.
 *
 *     + For receivers, messages may be received as long as the buffer is not
 *       empty.  If empty, receivers will wait (unless instructed not to) for
 *       it to become not empty.
 *
 *  2. **Unbuffered**: A sender and receiver will wait (unless instructed not
 *     to) until both are simultaneously ready.
 *
 * @param chan The \ref chan to initialize.
 * @param buf_cap The buffer capacity.  If zero, the channel is unbuffered.
 * @param msg_size The size of a message.  It must be &gt; 0 only if \a buf_cap
 * is &gt; 0.
 * @return Returns `true` only if initialization succeeded; `false` only if
 * memory allocation for a buffered channel fails.
 *
 * @sa chan_cleanup()
 * @sa chan_close()
 */
bool chan_init( struct chan *chan, unsigned buf_cap, size_t msg_size );

/**
 * Receives a message from a \ref chan.
 *
 * @param chan The \ref chan to receive from.
 * @param recv_buf The buffer to receive into.  It must be at least \ref
 * chan::msg_size "msg_size" bytes.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message is available and \a duration is `NULL`; or:
 *  + `ETIMEDOUT` if \a duration expired.
 *
 * @sa chan_send()
 */
int chan_recv( struct chan *chan, void *recv_buf,
               struct timespec const *duration );

/**
 * Sends a message to a channel.
 *
 * @param chan The \ref chan to send to.
 * @param send_buf The buffer to send from.  It must be at least \ref
 * chan::msg_size "msg_size" bytes.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return
 *  + 0 upon success; or:
 *  + `EPIPE` if \a chan is closed; or:
 *  + `EAGAIN` if no message can be sent and \a duration is `NULL`; or:
 *  + `ETIMEDOUT` if \a duration expired.
 *
 * @sa chan_recv()
 */
int chan_send( struct chan *chan, void const *send_buf,
               struct timespec const *duration );

/**
 * Selects at most one \ref chan from either \a recv_chan or \a send_chan that
 * has either received or sent a message, respectively.
 *
 * @remarks For example:
 *  ```c
 *  struct chan r_chan0, r_chan1, s_chan0, s_chan1;
 *  // ...
 *
 *  struct chan *const r_chan[] = { &r_chan0, &r_chan1 };
 *  int r0, r1;
 *  void *const r_buf[] = { &r0, &r1 };
 *
 *  struct chan *const s_chan[] = { &s_chan0, &s_chan1 };
 *  int s0 = 1, s1 = 2;
 *  void const *const s_buf[] = { &s0, &s1 };
 *
 *  struct timespec const duration = { .tv_sec = 5 };
 *
 *  switch ( chan_select( 2, r_chan, r_buf, 2, s_chan, s_buf, &duration ) ) {
 *    case CHAN_RECV(0):          // r_chan0 selected
 *      // ...
 *      break;
 *    case CHAN_RECV(1):          // r_chan1 selected
 *      // ...
 *      break;
 *    case CHAN_SEND(0):          // s_chan0 selected
 *      // ...
 *      break;
 *    case CHAN_SEND(1):          // s_chan1 selected
 *      // ...
 *      break;
 *    default:                    // no channel selected
 *      // ...
 *  }
 *  ```
 * where #CHAN_RECV(i) refers to the ith \ref chan in `r_chan` and
 * #CHAN_SEND(i) refers to the ith \ref chan in `s_chan`.
 * @par
 * When more than one \ref chan is ready, one is selected randomly.
 *
 * @param recv_len The length of \a recv_chan and \a recv_buf.
 * @param recv_chan An array of zero or more channels to read from.  If \a
 * recv_len is 0, may be `NULL`.  The same channel may not appear more than
 * once in the array.
 * @param recv_buf An array of zero or more pointers to buffers to receive into
 * corresponding to \a recv_chan.  If \a recv_len is 0, may be `NULL`.  The
 * same pointer may appear more than once in the array.
 * @param send_len The length of \a send_chan and \a send_buf.
 * @param send_chan An array of zero or more channels to send from.  If \a
 * send_len is 0, may be `NULL`.  The same channel may not appear more than
 * once in the array.
 * @param send_buf An array of zero or more pointers to buffers to send from
 * corresponding to \a send_chan.  If \a send_len is 0, may be `NULL`.  The
 * same pointer may appear more than once in the array.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns an integer &ge; 0 to indicate a selected channel (to be used
 * with #CHAN_RECV or #CHAN_SEND) or -1 if no channel was selected either
 * because all channels are closed or none are ready \a duration is `NULL` or
 * it expired.
 *
 * @warning No \ref chan may appear in both \a recv_chan and \a send_chan.
 */
int chan_select( unsigned recv_len, struct chan *recv_chan[recv_len],
                 void *recv_buf[recv_len],
                 unsigned send_len, struct chan *send_chan[send_len],
                 void const *send_buf[send_len],
                 struct timespec const *duration );

/** @} */

///////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#endif /* C_CHAN_H */
