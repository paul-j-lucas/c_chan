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
 * [doi:10.1145/359576.359585](https://doi.org/10.1145/359576.35958).
 */

// local
#include "config.h"                     /* must go first */

// standard
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>                       /* for timespec */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct channel;

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
  struct channel   *chan;               ///< The channel being observed.
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
 * Macros for use with cases of a `switch` statement on chan_select().
 * @{
 */
#define CHAN_SELECT_RECV(IDX)     ((int)(IDX))
#define CHAN_SELECT_SEND(IDX)     (1024 + (int)(IDX))
/** @} */

/**
 * Channel send/receive function return value.
 *
 * @sa chan_recv()
 * @sa chan_send()
 */
enum chan_rv {
  CHAN_OK,                              ///< Channel operation succeeded.
  CHAN_CLOSED,                          ///< Channel is closed.
  CHAN_TIMEDOUT                         ///< Channel operation timed out.
};
typedef enum chan_rv chan_rv;

/**
 * A Go-like channel.
 *
 * @sa Hoare, C. A. R., "Communicating Sequential Processes," Communications of
 * the ACM, 21(8), 1978, pp. 666–677,
 * [doi:10.1145/359576.359585](https://doi.org/10.1145/359576.35958).
 */
struct channel {
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
 * Cleans-up a \ref channel.
 *
 * @param chan The \ref channel to clean-up.  If `NULL`, does nothing.
 * @param free_fn For buffered channels only, the function to free unreceived
 * messages, if any.
 *
 * @note A channel _must_ be closed before it's cleaned-up.
 *
 * @sa chan_close()
 * @sa chan_init()
 */
void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) );

/**
 * Closes a channel.  If already closed, does nothing.  Once a channel is
 * closed, it can no longer be sent to.  Messages may still be received from a
 * buffered channel until it becomes empty.
 *
 * @param chan The \ref channel to close.
 *
 * @note A channel _must_ be cleaned-up eventually.
 *
 * @sa chan_cleanup()
 * @sa chan_init()
 */
void chan_close( struct channel *chan );

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
 * @param chan The \ref channel to initialize.
 * @param buf_cap The buffer capacity.  If zero, the channel is unbuffered.
 * @param msg_size The size of a message.  It must be &gt; 0 only if \a buf_cap
 * is &gt; 0.
 * @return Returns `true` only if initialization succeeded; `false` only if
 * memory allocation for a buffered channel fails.
 *
 * @sa chan_cleanup()
 * @sa chan_close()
 */
bool chan_init( struct channel *chan, unsigned buf_cap, size_t msg_size );

/**
 * Receives data from a \ref channel.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.  It must be at least \ref
 * channel::msg_size "msg_size" bytes.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns #CHAN_OK upon success, #CHAN_CLOSED if \a chan either is or
 * becomes closed, or #CHAN_TIMEDOUT if \a duration expires.
 *
 * @sa chan_send()
 */
chan_rv chan_recv( struct channel *chan, void *recv_buf,
                   struct timespec const *duration );

/**
 * Sends data to a channel.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.  It must be at least \ref
 * channel::msg_size "msg_size" bytes.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns #CHAN_OK upon success, #CHAN_CLOSED if \a chan either is or
 * becomes closed, or #CHAN_TIMEDOUT if \a duration expires.
 *
 * @sa chan_recv()
 */
chan_rv chan_send( struct channel *chan, void const *send_buf,
                   struct timespec const *duration );

/**
 * Selects at most one channel from either \a recv_chan or \a send_chan that
 * has either received or sent a message, respectively.
 *
 *  ```c
 *  struct channel *const r_chan[] = { &r_chan1, &r_chan2 };
 *  int r1, r2;
 *  void *const r_buf[] = { &r1, &r2 };
 *
 *  struct channel *const s_chan[] = { &s_chan1, &s_chan2 };
 *  int s1 = 1, s2 = 2;
 *  void *const s_buf[] = { &s1, &s2 };
 *
 *  switch ( chan_select( 2, r_chan, r_buf, 2, s_chan, s_buf, duration ) ) {
 *    case CHAN_SELECT_RECV(0):
 *      // ...
 *      break;
 *    case CHAN_SELECT_RECV(1):
 *      // ...
 *      break;
 *    case CHAN_SELECT_SEND(0):
 *      // ...
 *      break;
 *    case CHAN_SELECT_SEND(1):
 *      // ...
 *      break;
 *    default:
 *      // ...
 *  }
 *  ```
 *
 * @param recv_len The length of \a recv_chan and \a recv_buf.
 * @param recv_chan An array of zero or more channels to read from.  If \a
 * recv_len is 0, may be `NULL`.
 * @param recv_buf An array of zero or more buffers to receive into
 * corresponding to \a recv_chan.  If \a recv_len is 0, may be `NULL`.
 * @param send_len The length of \a send_chan and \a send_buf.
 * @param send_chan An array of zero or more channels to send from.  If \a
 * send_len is 0, may be `NULL`.
 * @param send_buf An array of zero or more buffers to send from corresponding
 * to \a send_chan.  If \a send_len is 0, may be `NULL`.
 * @param duration The duration of time to wait. If `NULL`, it's considered
 * zero (does not wait); if #CHAN_NO_TIMEOUT, waits indefinitely.
 * @return Returns an integer &ge; 0 for a selected channel or `-1` only if all
 * channels are or became closed or \a duration expired.
 *
 * @warning No \ref channel may appear in both \a recv_chan and \a send_chan
 * nor more than once in either.
 */
int chan_select( unsigned recv_len, struct channel *recv_chan[recv_len],
                 void *recv_buf[recv_len],
                 unsigned send_len, struct channel *send_chan[send_len],
                 void const *send_buf[send_len],
                 struct timespec const *duration );

/** @} */

///////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#endif /* C_CHAN_H */
