/*
**      c_chan -- Channels Library for C
**      src/c_chan_test.c
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
#include "config.h"                     /* must go first */
#include "c_chan.h"
#include "util.h"
#include "unit_test.h"

// standard
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <time.h>

/**
 * Calls **pthread_create**(3), checks for an error, and exits if there was
 * one.
 *
 * @param THR The thread variable to create.
 * @param ATTR The attributes to use, if any.
 * @param START_FN The thread's start function.
 * @param ARG The argument passed to \a START_FN.
 *
 * @sa #PTHREAD_COND_INIT()
 */
#define PTHREAD_CREATE(THR,ATTR,START_FN,ARG) \
  PERROR_EXIT_IF( pthread_create( (THR), (ATTR), (START_FN), (ARG) ) != 0, EX_IOERR )

/**
 * Calls **pthread_join**(3), checks for an error, and exits if there was
 * one.
 *
 * @param THR The thread to join.
 * @param VALUE A pointer to receive the start function's return value, if any.
 */
#define PTHREAD_JOIN(THR,VALUE) \
  PERROR_EXIT_IF( pthread_join( (THR), (VALUE) ) != 0, EX_IOERR )

#define FN_TEST(EXPR)             TEST_INC(EXPR, ++fn_fail_cnt)

#define TEST_FN_BEGIN()           test_fail_cnt_t fn_fail_cnt = 0

#define TEST_FN_END() \
  BLOCK( test_fail_cnt += fn_fail_cnt; return fn_fail_cnt == 0; )

#define TEST_THRD_ARG(...) \
  ((thrd_arg){ __VA_ARGS__, .fail_cnt = &fn_fail_cnt })

#define THRD_TEST(EXPR)           TEST_INC(EXPR, ++*arg->fail_cnt)

typedef unsigned _Atomic test_fail_cnt_t;

/**
 * Argument passed to a thread's start function.
 */
struct thrd_arg {
  struct timespec const  *duration;     ///< Duration to wait, if any.
  test_fail_cnt_t        *fail_cnt;     ///< Test failure count.
  int                     rv;           ///< Expected function return value.

  union {

    /// For chan_recv(), chan_send() only.
    struct {
      struct channel     *chan;         ///< The channel to use.
      union {
        int               send_val;     ///< Value to send.
        int               recv_val;     ///< Value expected to receive.
      };
    };

    /// For chan_select() only.
    struct {
      unsigned            recv_len;     ///< Length of recv_chan, recv_buf.
      struct channel    **recv_chan;    ///< Array of receive channels, if any.
      void              **recv_buf;     ///< Array of receive buffers, if any.
      unsigned            send_len;     ///< Length of send_chan, send_buf.
      struct channel    **send_chan;    ///< Array of send channels, if any.
      void const        **send_buf;     ///< Array of send buffers, if any.
    };

  };
};
typedef struct thrd_arg thrd_arg;

////////// local functions ////////////////////////////////////////////////////

/**
 * Sleeps for \a ms milliseconds.
 *
 * @param ms The number of milliseconds to sleep.
 */
static void ms_sleep( unsigned ms ) {
  struct timespec req, rem;

  req.tv_sec  =  ms / 1000;
  req.tv_nsec = (ms % 1000) * 1000000L;

  while ( nanosleep( &req, &rem ) == -1 && errno == EINTR )
    req = rem;
}

/**
 * Spin-waits for `*pus` to be non-zero.
 *
 * @param mtx The mutex to lock/unlock before/after checking `*pus`.
 * @param pus A pointer to an `unsigned short`.
 */
static void spin_wait_us( pthread_mutex_t *mtx, unsigned short *pus ) {
  PTHREAD_MUTEX_LOCK( mtx );
  while ( *pus == 0 ) {
    PTHREAD_MUTEX_UNLOCK( mtx );
    ms_sleep( 5 );
    PTHREAD_MUTEX_LOCK( mtx );
  }
  PTHREAD_MUTEX_UNLOCK( mtx );
}

////////// test helper functions //////////////////////////////////////////////

static void* thrd_chan_recv( void *p ) {
  thrd_arg *const arg = p;
  int data = 0;
  int const rv = chan_recv( arg->chan, &data, arg->duration );
  if ( THRD_TEST( rv == arg->rv ) && arg->rv == 0 ) {
    THRD_TEST( data == arg->recv_val );
  }
  return NULL;
}

static void* thrd_chan_select( void *p ) {
  thrd_arg *const arg = p;
  int const rv = chan_select(
    arg->recv_len, arg->recv_chan, arg->recv_buf,
    arg->send_len, arg->send_chan, arg->send_buf,
    arg->duration
  );
  THRD_TEST( rv == arg->rv );
  return NULL;
}

static void* thrd_chan_send( void *p ) {
  thrd_arg *const arg = p;
  int const rv = chan_send( arg->chan, &arg->send_val, arg->duration );
  THRD_TEST( rv == arg->rv );
  return NULL;
}

////////// test functions /////////////////////////////////////////////////////

/**
 * Tests that buffered channels work.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_chan( void ) {
  TEST_FN_BEGIN();

  struct channel chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    // Create a receiving thread that won't wait and no sender.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv,
      &TEST_THRD_ARG(
        .chan = &chan,
        .rv = EAGAIN
      )
    );
    PTHREAD_JOIN( recv_thrd, NULL );

    thrd_arg arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .send_val = 42
    );

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );

    // Create the sending thread first and ensure it sent before creating the
    // receiving thread.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( send_thrd, NULL );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );

    // Check that you can't send to a full buffered channel.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( send_thrd, NULL );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send,
      &TEST_THRD_ARG(
        .chan = &chan,
        .rv = EAGAIN
      )
    );
    PTHREAD_JOIN( send_thrd, NULL );

    // Check that you can still receive from a closed but non-empty channel.
    chan_close( &chan );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );

    // Check that you can't send to a closed channel.
    arg.rv = EPIPE;
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( send_thrd, NULL );

    // Check that you can't receive from a closed and empty channel.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );

    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from a buffered channel that isn't ready and not
 * waiting works.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_select_recv_nowait( void ) {
  TEST_FN_BEGIN();

  struct channel chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    int data = 0;
    pthread_t thrd;

    PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_select,
      &TEST_THRD_ARG(
        .recv_len = 1,
        .recv_chan = (struct channel*[]){ &chan },
        .recv_buf = (void*[]){ &data },
        .rv = -1
      )
    );
    PTHREAD_JOIN( thrd, NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from a ready buffered channel works.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_select_recv_1( void ) {
  TEST_FN_BEGIN();

  struct channel chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    int data = 0;
    pthread_t thrd;

    PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_send,
      &TEST_THRD_ARG(
        .chan = &chan,
        .send_val = 42
      )
    );
    PTHREAD_JOIN( thrd, NULL );

    PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_select,
      &TEST_THRD_ARG(
        .recv_len = 1,
        .recv_chan = (struct channel*[]){ &chan },
        .recv_buf = (void*[]){ &data },
        .duration = CHAN_NO_TIMEOUT,
        .rv = CHAN_RECV(0)
      )
    );
    PTHREAD_JOIN( thrd, NULL );
    FN_TEST( data == 42 );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from two buffered channels works.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_select_recv_2( void ) {
  TEST_FN_BEGIN();

  struct channel chan0, chan1;
  if ( !FN_TEST( chan_init( &chan0, /*buf_cap=*/1, sizeof(int) ) ) )
    goto error;
  if ( !FN_TEST( chan_init( &chan1, /*buf_cap=*/1, sizeof(int) ) ) )
    goto close0;

  pthread_t thrd;
  int data0 = 0, data1 = 0;

  PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_send,
    &TEST_THRD_ARG(
      .chan = &chan1,
      .send_val = 42
    )
  );
  PTHREAD_JOIN( thrd, NULL );

  PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_select,
    &TEST_THRD_ARG(
      .recv_len = 2,
      .recv_chan = (struct channel*[]){ &chan0, &chan1 },
      .recv_buf = (void*[]){ &data0, &data1 },
      .rv = CHAN_RECV(1)
    )
  );
  PTHREAD_JOIN( thrd, NULL );
  FN_TEST( data1 == 42 );

  chan_close( &chan1 );
  chan_cleanup( &chan1, /*free_fn=*/NULL );

close0:
  chan_close( &chan0 );
  chan_cleanup( &chan0, /*free_fn=*/NULL );

error:
  TEST_FN_END();
}

/**
 * Tests that selecting from a ready buffered channel works.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_select_send_1( void ) {
  TEST_FN_BEGIN();

  struct channel chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    int data = 42;
    pthread_t recv_thrd, send_thrd;

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv,
      &TEST_THRD_ARG(
        .chan = &chan,
        .duration = CHAN_NO_TIMEOUT,
        .recv_val = 42
      )
    );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );

    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_select,
      &TEST_THRD_ARG(
        .send_len = 1,
        .send_chan = (struct channel*[]){ &chan },
        .send_buf = (void const*[]){ &data },
        .duration = CHAN_NO_TIMEOUT,
        .rv = CHAN_SEND(0)
      )
    );
    PTHREAD_JOIN( send_thrd, NULL );
    PTHREAD_JOIN( recv_thrd, NULL );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that unbuffered channels work.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_unbuf_chan( void ) {
  TEST_FN_BEGIN();

  struct channel chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/0, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    thrd_arg arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .send_val = 42
    );

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );

    // Create the sending thread first and ensure it's ready before creating
    // the receiving thread.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[1] );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );

    chan_close( &chan );

    // Check that you can't send to a closed channel.
    arg.rv = EPIPE;
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    PTHREAD_JOIN( send_thrd, NULL );

    // Check that you can't receive from a closed and empty channel.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );

    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

////////// main ///////////////////////////////////////////////////////////////

int main( int argc, char const *argv[] ) {
  test_prog_init( argc, argv );

  if ( test_buf_chan() && test_unbuf_chan() ) {
    test_buf_select_recv_nowait();
    test_buf_select_recv_1() && test_buf_select_recv_2();
    test_buf_select_send_1();
  }
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
