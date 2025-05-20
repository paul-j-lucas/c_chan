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

// local
#include "config.h"                     /* must go first */

// standard
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define CHAN_SELECT_EACH_MAX      32

/**
 * Macros for use with cases of a `switch` statement on chan_select().
 *{
 */
#define CHAN_SELECT_RECV(IDX)     (IDX)
#define CHAN_SELECT_SEND(IDX)     (CHAN_SELECT_EACH_MAX * 2 + (IDX))
/** @> */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

///////////////////////////////////////////////////////////////////////////////

/**
 * Channel send/receive return value.
 */
enum chan_rv {
  CHAN_OK,                              ///< Channel operation succeeded.
  CHAN_CLOSED,                          ///< Channel is closed.
  CHAN_TIMEDOUT                         ///< Channel timed out.
};
typedef enum chan_rv chan_rv;

/**
 * TODO
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
      pthread_mutex_t recv_buf_mtx;
    } unbuf;
  };

  unsigned            buf_cap;          ///< Channel capacity; 0 = unbuffered.
  size_t              msg_size;         ///< Size of a message.
  pthread_mutex_t     mtx;              ///< Channel mutex.
  pthread_cond_t      not_empty;        ///< Channel is no longer empty.
  pthread_cond_t      not_full;         ///< Channel is no longer full.
  unsigned            recv_wait_cnt;    ///< Number of receivers waiting.
  unsigned            send_wait_cnt;    ///< Number of senders waiting.
  bool                is_closed;        ///< Is channel closed?
};

/**
 * Cleans-up a \ref channel.
 *
 * @param chan The \ref channel to clean-up.
 * @param free_fn For buffered channels only, the function to free unreceived
 * messages, if any.
 *
 * @sa chan_close()
 * @sa chan_init()
 */
void chan_cleanup( struct channel *chan, void (*free_fn)( void* ) );

/**
 * Closes a channel.  If already closed, has no effect.  Once a channel is
 * closed, it can no longer be sent to.  Unreceived messages may still be
 * received until the channel becomes empty.
 *
 * @param chan The \ref channel to close.
 *
 * @sa chan_cleanup()
 * @sa chan_init()
 */
void chan_close( struct channel *chan );

/**
 * Initializes a channel.
 *
 * @param chan The \ref channel to initialize.
 * @param buf_cap The buffer capacity.  If zero, the channel is unbuffered.
 * @param msg_size The size of a message.  It must be &gt; 0.
 * @return Returns `true` only if initialization succeeded; `false` otherwise.
 *
 * @sa chan_cleanup()
 * @sa chan_close()
 */
bool chan_init( struct channel *chan, unsigned buf_cap, size_t msg_size );

/**
 * Receives data from a \ref channel.
 *
 * @remarks If the channel is closed, the receive is aborted.
 *
 * @param chan The \ref channel to receive from.
 * @param recv_buf The buffer to receive into.  It must be at least `msg_size`
 * bytes.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 */
chan_rv chan_recv( struct channel *chan, void *recv_buf,
                   struct timespec const *timeout );

/**
 * Sends data to a channel.
 *
 * @remarks If the channel is closed, the send is aborted.
 *
 * @param chan The \ref channel to send to.
 * @param send_buf The buffer to send from.  It must be at least `msg_size`
 * bytes.
 * @param timeout The timeout to use. If `NULL`, waits indefinitely.
 * @return Returns a \ref chan_rv.
 */
chan_rv chan_send( struct channel *chan, void const *send_buf,
                   struct timespec const *timeout );

/**
 * Selects at most one channel from either \a recv_chan or \a send_chan that
 * has either received or sent a message, respectively.
 *
 *  ```c
 *  struct channel *const r_chan[] = { &r1, &r2 };
 *  int r_buf[2];
 *  struct channel *const s_chan[] = { &s1, &s2 };
 *  int s_buf[2];
 *  switch ( chan_select( chan, 2, r_chan, r_buf, 2, s_chan, s_buf, false ) ) {
 *    case CHAN_SELECT_RECV(1):
 *      // ...
 *      break;
 *    case CHAN_SELECT_RECV(2):
 *      // ...
 *      break;
 *    case CHAN_SELECT_SEND(1):
 *      // ...
 *      break;
 *    case CHAN_SELECT_SEND(2):
 *      // ...
 *      break;
 *    default:
 *      // ...
 *  }
 *  ```
 *
 * @param recv_n The size of \a recv_chan and \a recv_buf.
 * @param recv_chan TODO.
 * @param recv_buf TODO.
 * @param send_n The size of \a send_chan and \a send_buf.
 * @param send_chan TODO.
 * @param send_buf TODO.
 * @return Returns TODO.
 */
int chan_select( unsigned recv_n, struct channel *recv_chan[recv_n],
                 void *recv_buf[recv_n],
                 unsigned send_n, struct channel *send_chan[send_n],
                 void const *send_buf[send_n] );

///////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#endif /* C_CHAN_H */
