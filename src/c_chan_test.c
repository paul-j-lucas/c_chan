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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>

#define FN_TEST(EXPR)             TEST_INC_FAIL_CNT(EXPR, ++fn_fail_cnt)

#define TEST_FN_BEGIN()           unsigned fn_fail_cnt = 0

#define TEST_FN_END() \
  BLOCK( test_fail_cnt += fn_fail_cnt; return fn_fail_cnt == 0; )

#define THRD_FN_BEGIN()           unsigned thrd_fail_cnt = 0;

#define THRD_FN_END()             return (void*)(uintptr_t)thrd_fail_cnt

#define TEST_PTHREAD_JOIN(THR)                        \
  BLOCK(                                              \
    void *thrd_result;                                \
    PTHREAD_JOIN( THR, &thrd_result );                \
    fn_fail_cnt += (unsigned)(uintptr_t)thrd_result;  \
  )

#define THRD_FN_TEST(EXPR)        TEST_INC_FAIL_CNT(EXPR, ++thrd_fail_cnt)

#define TEST_THRD_ARG(...)        ((test_thrd_arg){ __VA_ARGS__ })

/**
 * Argument passed to a thread's start function.
 */
struct test_thrd_arg {
  union {
    /// For chan_recv(), chan_send() only.
    struct {
      struct chan        *chan;         ///< The channel to use.
      union {
        int               send_val;     ///< Value to send.
        int               recv_val;     ///< Value expected to receive.
      };
    };

    /// For chan_select() only.
    struct {
      unsigned            recv_len;     ///< Length of recv_chan, recv_buf.
      struct chan       **recv_chan;    ///< Array of receive channels, if any.
      void              **recv_buf;     ///< Array of receive buffers, if any.
      unsigned            send_len;     ///< Length of send_chan, send_buf.
      struct chan       **send_chan;    ///< Array of send channels, if any.
      void const        **send_buf;     ///< Array of send buffers, if any.
    };
  };

  struct timespec const  *duration;     ///< Duration to wait, if any.
  int                     rv;           ///< Expected function return value.
};
typedef struct test_thrd_arg test_thrd_arg;

/**
 * Argument passed to the thrd_fib_start function.
 */
struct fib_thrd_arg {
  struct chan *fib_chan;
  struct chan *quit_chan;
};
typedef struct fib_thrd_arg fib_thrd_arg;

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
 * Prints expected and actual function return values and their meanings.
 *
 * @param expected_rv The expected function return value.
 * @param actual_rv The actual function return value.
 */
static void print_rvs( int expected_rv, int actual_rv ) {
  EPRINTF( "expected_rv=%d", expected_rv );
  if ( expected_rv != 0 )
    EPRINTF( " (%s)", strerror( expected_rv ) );
  EPRINTF( ", actual_rv=%d", actual_rv );
  if ( actual_rv != 0 )
    EPRINTF( " (%s)", strerror( actual_rv ) );
  EPUTC( '\n' );
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

/**
 * Generates Fibonacci numbers until told quit.
 *
 * @param fib_chan The \ref chan to send the next Fibonacci number to.
 * @param quit_chan The \ref chan to receive a quit signal from.
 *
 * @sa [A Tour of Go: Select](https://go.dev/tour/concurrency/5)
 */
static void fibonacci( struct chan *fib_chan, struct chan *quit_chan ) {
  unsigned fib = 0, prev_fib, next_fib = 1;
  for (;;) {
    int const rv = chan_select(
      1, (struct chan*[]){ quit_chan }, (void      *[]){ NULL },
      1, (struct chan*[]){ fib_chan  }, (void const*[]){ &fib },
      CHAN_NO_TIMEOUT
    );
    switch ( rv ) {
      case CHAN_RECV(0):                // If received quit ...
        return;                         // ... quit.
      case CHAN_SEND(0):                // If sent number ...
        printf( "fib_chan <- %u\n", fib );
        prev_fib = fib;                 // ... calculate next number.
        fib = next_fib;
        next_fib += prev_fib;
        break;
      default:
        EPRINTF( "rv=%d, errno=%d (%s)", rv, errno, strerror( errno ) );
    } // switch
  } // for
}

static void* thrd_chan_recv( void *p ) {
  THRD_FN_BEGIN();
  test_thrd_arg *const arg = p;
  int data = 0;
  int const rv = chan_recv( arg->chan, &data, arg->duration );
  bool const rvs_equal = THRD_FN_TEST( rv == arg->rv );
  if ( !rvs_equal )
    print_rvs( arg->rv, rv );
  if ( rvs_equal && rv == 0 && arg->chan->msg_size > 0 )
    THRD_FN_TEST( data == arg->recv_val );
  THRD_FN_END();
}

static void* thrd_chan_select( void *p ) {
  THRD_FN_BEGIN();
  test_thrd_arg *const arg = p;
  int const rv = chan_select(
    arg->recv_len, arg->recv_chan, arg->recv_buf,
    arg->send_len, arg->send_chan, arg->send_buf,
    arg->duration
  );
  if ( !THRD_FN_TEST( rv == arg->rv ) )
    print_rvs( arg->rv, rv );
  THRD_FN_END();
}

static void* thrd_chan_send( void *p ) {
  THRD_FN_BEGIN();
  test_thrd_arg *const arg = p;
  int const rv = chan_send( arg->chan, &arg->send_val, arg->duration );
  if ( !THRD_FN_TEST( rv == arg->rv ) )
    print_rvs( arg->rv, rv );
  THRD_FN_END();
}

static void* thrd_fib_start( void *p ) {
  fib_thrd_arg const *const arg = p;
  fibonacci( arg->fib_chan, arg->quit_chan );
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

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) == 0 ) ) {
    pthread_t recv_thrd, send_thrd;
    test_thrd_arg arg;

    // Create a receiving thread that won't wait and no sender.
    arg = TEST_THRD_ARG(
      .chan = &chan,
      .rv = EAGAIN
    );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );

    arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .send_val = 42
    );

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );
    TEST_PTHREAD_JOIN( send_thrd );

    // Create the sending thread first and ensure it sent before creating the
    // receiving thread.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( send_thrd );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );

    // Check that you can't send to a full buffered channel.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( send_thrd );
    test_thrd_arg arg2 = TEST_THRD_ARG(
      .chan = &chan,
      .rv = EAGAIN
    );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg2 );
    TEST_PTHREAD_JOIN( send_thrd );

    // Check that you can still receive from a closed but non-empty channel.
    chan_close( &chan );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );

    // Check that you can't send to a closed channel.
    arg.rv = EPIPE;
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( send_thrd );

    // Check that you can't receive from a closed and empty channel.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );

    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from a ready channel works.
 *
 * @param buf_cap The channel's capacity.
 * @return Returns `true` only if all tests passed.
 */
static bool test_select_recv_1( unsigned buf_cap ) {
  TEST_FN_BEGIN();

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, buf_cap, sizeof(int) ) == 0 ) ) {
    pthread_t recv_thrd, send_thrd;

    test_thrd_arg send_arg = TEST_THRD_ARG(
      .chan = &chan,
      .send_val = 42,
      .duration = CHAN_NO_TIMEOUT
    );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &send_arg );

    int recv_val = 0;
    test_thrd_arg select_arg = TEST_THRD_ARG(
      .recv_len = 1,
      .recv_chan = (struct chan*[]){ &chan },
      .recv_buf = (void*[]){ &recv_val },
      .duration = CHAN_NO_TIMEOUT,
      .rv = CHAN_RECV(0)
    );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_select, &select_arg );
    TEST_PTHREAD_JOIN( recv_thrd );
    TEST_PTHREAD_JOIN( send_thrd );
    FN_TEST( recv_val == 42 );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from two channels works.
 *
 * @param buf_cap The channel's capacity.
 * @return Returns `true` only if all tests passed.
 */
static bool test_select_recv_2( unsigned buf_cap ) {
  TEST_FN_BEGIN();

  struct chan chan0, chan1;
  if ( !FN_TEST( chan_init( &chan0, buf_cap, sizeof(int) ) == 0 ) )
    goto error;
  if ( !FN_TEST( chan_init( &chan1, buf_cap, sizeof(int) ) == 0 ) )
    goto close0;

  pthread_t recv_thrd, send_thrd;
  int data0 = 0, data1 = 0;

  test_thrd_arg send_arg = TEST_THRD_ARG(
    .chan = &chan1,
    .send_val = 42,
    .duration = CHAN_NO_TIMEOUT
  );
  PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &send_arg );

  test_thrd_arg select_arg = TEST_THRD_ARG(
    .recv_len = 2,
    .recv_chan = (struct chan*[]){ &chan0, &chan1 },
    .recv_buf = (void*[]){ &data0, &data1 },
    .duration = CHAN_NO_TIMEOUT,
    .rv = CHAN_RECV(1)
  );
  PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_select, &select_arg );
  TEST_PTHREAD_JOIN( recv_thrd );
  TEST_PTHREAD_JOIN( send_thrd );
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
 * Tests that selecting from a channel that isn't ready and not waiting works.
 *
 * @param buf_cap The channel's capacity.
 * @return Returns `true` only if all tests passed.
 */
static bool test_select_recv_nowait( unsigned buf_cap ) {
  TEST_FN_BEGIN();

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, buf_cap, sizeof(int) ) == 0 ) ) {
    int data = 0;
    pthread_t thrd;

    test_thrd_arg select_arg = TEST_THRD_ARG(
      .recv_len = 1,
      .recv_chan = (struct chan*[]){ &chan },
      .recv_buf = (void*[]){ &data },
      .rv = -1
    );
    PTHREAD_CREATE( &thrd, /*attr=*/NULL, &thrd_chan_select, &select_arg );
    TEST_PTHREAD_JOIN( thrd );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from a closed, non-empty, buffered channel works.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_select_buf_recv_closed( void ) {
  TEST_FN_BEGIN();

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, 1, sizeof(int) ) == 0 ) ) {
    pthread_t recv_thrd, send_thrd;

    test_thrd_arg arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .send_val = 42
    );

    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( send_thrd );
    chan_close( &chan );

    int data = 0;
    test_thrd_arg select_arg = TEST_THRD_ARG(
      .recv_len = 1,
      .recv_chan = (struct chan*[]){ &chan },
      .recv_buf = (void*[]){ &data },
      .rv = CHAN_RECV(0)
    );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_select, &select_arg );
    TEST_PTHREAD_JOIN( recv_thrd );
    FN_TEST( data == 42 );
  }

  TEST_FN_END();
}

/**
 * Tests that selecting from a ready channel works.
 *
 * @param buf_cap The channel's capacity.
 * @return Returns `true` only if all tests passed.
 */
static bool test_select_send_1( unsigned buf_cap ) {
  TEST_FN_BEGIN();

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, buf_cap, sizeof(int) ) == 0 ) ) {
    int data = 42;
    pthread_t recv_thrd, send_thrd;

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    test_thrd_arg recv_arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .recv_val = 42
    );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &recv_arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );

    test_thrd_arg select_arg = TEST_THRD_ARG(
      .send_len = 1,
      .send_chan = (struct chan*[]){ &chan },
      .send_buf = (void const*[]){ &data },
      .duration = CHAN_NO_TIMEOUT,
      .rv = CHAN_SEND(0)
    );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_select, &select_arg );
    TEST_PTHREAD_JOIN( send_thrd );
    TEST_PTHREAD_JOIN( recv_thrd );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that unbuffered channels size work.
 *
 * @param msg_size The message size.
 * @return Returns `true` only if all tests passed.
 */
static bool test_unbuf_chan( size_t msg_size ) {
  TEST_FN_BEGIN();

  struct chan chan;
  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/0, msg_size ) == 0 ) ) {
    pthread_t recv_thrd, send_thrd;

    test_thrd_arg arg = TEST_THRD_ARG(
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .send_val = 42
    );

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[0] );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );
    TEST_PTHREAD_JOIN( send_thrd );

    // Create the sending thread first and ensure it's ready before creating
    // the receiving thread.
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    spin_wait_us( &chan.mtx, &chan.wait_cnt[1] );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );
    TEST_PTHREAD_JOIN( send_thrd );

    chan_close( &chan );

    // Check that you can't send to a closed channel.
    arg.rv = EPIPE;
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &thrd_chan_send, &arg );
    TEST_PTHREAD_JOIN( send_thrd );

    // Check that you can't receive from a closed and empty channel.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, &arg );
    TEST_PTHREAD_JOIN( recv_thrd );

    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  TEST_FN_END();
}

/**
 * Tests that a select of multiple unbuffered channels works.
 *
 * @sa [A Tour of Go: Select](https://go.dev/tour/concurrency/5)
 */
static bool test_select_unbuf_2( void ) {
  TEST_FN_BEGIN();

  struct chan fib_chan, quit_chan;
  if ( !FN_TEST( chan_init( &fib_chan, 0, sizeof(unsigned) ) == 0 ) )
    goto fib_chan_error;
  if ( !FN_TEST( chan_init( &quit_chan, 0, 0 ) == 0 ) )
    goto quit_chan_error;

  pthread_t fib_thrd;
  fib_thrd_arg arg = { &fib_chan, &quit_chan };
  PTHREAD_CREATE( &fib_thrd, /*attr=*/NULL, &thrd_fib_start, &arg );

  static unsigned const FIB[] = { 0, 1, 1, 2, 3, 5, 8, 13, 21, 34 };

  for ( unsigned i = 0; i < ARRAY_SIZE( FIB ); ++i ) {
    unsigned fib;
    if ( !FN_TEST( chan_recv( &fib_chan, &fib, CHAN_NO_TIMEOUT ) == 0 ) )
      goto fib_error;
    printf( "fib := <- %u\n", fib );
    if ( !FN_TEST( fib == FIB[i] ) )
      goto fib_error;
  }
  if ( FN_TEST( chan_send( &quit_chan, NULL, CHAN_NO_TIMEOUT ) == 0 ) )
    PTHREAD_JOIN( fib_thrd, NULL );

fib_error:
  chan_cleanup( &quit_chan, NULL );
quit_chan_error:
  chan_cleanup( &fib_chan, NULL );
fib_chan_error:
  TEST_FN_END();
}

////////// main ///////////////////////////////////////////////////////////////

int main( int argc, char const *argv[] ) {
  test_prog_init( argc, argv );

  if ( test_buf_chan() &&
       test_unbuf_chan( sizeof(int) ) && test_unbuf_chan( 0 ) ) {
    if ( test_select_recv_1( 0 ) && test_select_recv_1( 1 ) ) {
      test_select_recv_2( 0 );
      test_select_recv_2( 1 );
    }
    test_select_recv_nowait( 0 );
    test_select_recv_nowait( 1 );
    test_select_buf_recv_closed();
    test_select_send_1( 0 );
    test_select_send_1( 1 );
    test_select_unbuf_2();
  }
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
