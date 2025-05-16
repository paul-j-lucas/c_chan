/*
**      C Channels -- Channels Library for C
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

// standard
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define CHAN_SELECT_RECV(X) (X)

#define CHAN_SELECT_SEND(X) (1000 + (X))

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

///////////////////////////////////////////////////////////////////////////////

/**
 * TODO
 */
struct channel {
  size_t              buf_cap;          ///< Capacity. If zero, unbuffered.

  union {
    struct {
      void           *buf;
      unsigned        len;              ///< If buffered, length of buffer.
      unsigned        idx[2];           ///< 0 = read; 1 = write.
    } buf;
    struct {
      void           *recv_buf;
      pthread_mutex_t mtx[2];
    } unbuf;
  };

  size_t              msg_size;
  pthread_mutex_t     mtx;
  pthread_cond_t      not_empty;        ///< Channel is no longer empty.
  pthread_cond_t      not_full;         ///< Channel is no longer full.
  bool                is_closed;        ///< Is channel closed?
};

/**
 * TODO
 *
 * @param chan The \ref channel to clean-up.
 * @param free_fn TODO.
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
 */
void chan_close( struct channel *chan );

/**
 * Initializes a channel.
 *
 * @param chan The \ref channel to initialize.
 * @param buf_cap The buffer capacity.  If zero, the channel is unbuffered.
 * @param msg_size The size of a message.
 * @return Returns `true` only if initialization succeeded; `false` otherwise.
 *
 * @sa chan_cleanup()
 * @sa chan_close()
 */
bool chan_init( struct channel *chan, size_t buf_cap, size_t msg_size );

/**
 * TODO
 *
 * @param chan The \ref channel to receive from.
 * @param data TODO
 * @param timeout TODO
 * @return Returns `true` only upon success or `false` upon failure.
 */
bool chan_recv( struct channel *chan, void *data,
                struct timespec const *timeout );

/**
 * TODO
 *
 * @param chan The \ref channel to send to.
 * @param data TODO
 * @param timeout TODO
 * @return Returns `true` only upon success or `false` upon failure.
 */
bool chan_send( struct channel *chan, void *data,
                struct timespec const *timeout );

/**
 * TODO
 *
 * @param recv_n TODO.
 * @param recv_chan TODO.
 * @param recv_data TODO.
 * @param send_n TODO.
 * @param send_chan TODO.
 * @param send_data TODO.
 * @return Returns TODO.
 */
int chan_select( size_t recv_n, struct channel *recv_chan[recv_n],
                 void *recv_data[recv_n],
                 size_t send_n, struct channel *send_chan[send_n],
                 void const *send_data[send_n] );

///////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#endif /* C_CHAN_H */
