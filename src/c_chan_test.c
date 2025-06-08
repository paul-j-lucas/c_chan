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

#define PTHREAD_CREATE(THR,ATTR,START_FN,ARG) \
  PERROR_EXIT_IF( pthread_create( (THR), (ATTR), (START_FN), (ARG) ) != 0, EX_IOERR )

#define PTHREAD_JOIN(THR,VALUE) \
  PERROR_EXIT_IF( pthread_join( (THR), (VALUE) ) != 0, EX_IOERR )

#define FN_TEST(EXPR)             TEST_INC(EXPR, ++fn_fail_cnt)

#define THRD_TEST(EXPR)           TEST_INC(EXPR, ++*arg->fail_cnt)

typedef unsigned _Atomic test_fail_cnt_t;

/**
 * Argument passed to a thread's start function.
 */
struct thrd_arg {
  struct timespec const  *duration;
  union {
    struct {                            // chan_recv(), chan_send() only
      struct channel     *chan;
      chan_rv             expected_rv;
    };
    struct {                            // chan_select() only
      unsigned            recv_len, send_len;
      struct channel    **recv_chan, **send_chan;
      void              **recv_buf;
      void const        **send_buf;
      int                 expected_select_rv;
    };
  };
  test_fail_cnt_t        *fail_cnt;
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

static void* thrd_chan_select( void *p ) {
  thrd_arg *const arg = p;
  int const cs_rv = chan_select(
    arg->recv_len, arg->recv_chan, arg->recv_buf,
    arg->send_len, arg->send_chan, arg->send_buf,
    arg->duration
  );
  THRD_TEST( cs_rv == arg->expected_select_rv );
  return NULL;
}

static void* thrd_chan_recv( void *p ) {
  thrd_arg *const arg = p;
  int data = 0;
  if ( THRD_TEST( chan_recv( arg->chan, &data,
                        arg->duration ) == arg->expected_rv ) &&
       arg->expected_rv == CHAN_OK ) {
    THRD_TEST( data == 42 );
  }
  return NULL;
}

static void* thrd_chan_send( void *p ) {
  thrd_arg *const arg = p;
  int data = 42;
  THRD_TEST( chan_send( arg->chan, &data, arg->duration ) == arg->expected_rv );
  return NULL;
}

////////// test functions /////////////////////////////////////////////////////

/**
 * Tests that buffered channels work.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_buf_chan( void ) {
  struct channel  chan;
  test_fail_cnt_t fn_fail_cnt = 0;

  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    // Create a receiving thread that won't wait and no sender.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_recv, (&(thrd_arg){
      .chan = &chan,
      .expected_rv = CHAN_TIMEDOUT,
      .fail_cnt = &fn_fail_cnt
    }) );
    PTHREAD_JOIN( recv_thrd, NULL );

    thrd_arg arg = {
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .fail_cnt = &fn_fail_cnt
    };

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

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  test_fail_cnt += fn_fail_cnt;
  return fn_fail_cnt == 0;
}

static bool test_buf_select_recv_0_nowait( void ) {
  struct channel  chan;
  test_fail_cnt_t fn_fail_cnt = 0;

  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    pthread_t recv_thrd;

    int data = 0;
    thrd_arg arg = {
      .recv_len = 1,
      .recv_chan = (struct channel*[]){ &chan },
      .recv_buf = (void*[]){ &data },
      .duration = NULL,
      .expected_select_rv = -1,
      .fail_cnt = &fn_fail_cnt
    };

    // Create a receiving thread that won't wait and no sender.
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &thrd_chan_select, &arg );
    PTHREAD_JOIN( recv_thrd, NULL );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  test_fail_cnt += fn_fail_cnt;
  return fn_fail_cnt == 0;
}

/**
 * Tests that unbuffered channels work.
 *
 * @return Returns `true` only if all tests passed.
 */
static bool test_unbuf_chan( void ) {
  struct channel  chan;
  test_fail_cnt_t fn_fail_cnt = 0;

  if ( FN_TEST( chan_init( &chan, /*buf_cap=*/0, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    thrd_arg arg = {
      .chan = &chan,
      .duration = CHAN_NO_TIMEOUT,
      .fail_cnt = &fn_fail_cnt
    };

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
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }

  test_fail_cnt += fn_fail_cnt;
  return fn_fail_cnt == 0;
}

////////// main ///////////////////////////////////////////////////////////////

int main( int argc, char const *argv[] ) {
  test_prog_init( argc, argv );

  if ( test_buf_chan() && test_unbuf_chan() ) {
    test_buf_select_recv_0_nowait();
  }
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
