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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

#define PTHREAD_CREATE(THR,ATTR,START_FN,ARG) \
  PERROR_EXIT_IF( pthread_create( (THR), (ATTR), (START_FN), (ARG) ) != 0, EX_IOERR )

#define PTHREAD_JOIN(THR,VALUE) \
  PERROR_EXIT_IF( pthread_join( (THR), (VALUE) ) != 0, EX_IOERR )

////////// test functions /////////////////////////////////////////////////////

static void* test_chan_recv_1( void *thrd_arg ) {
  struct channel *const chan = thrd_arg;
  int data = 0;
  if ( TEST( chan_recv( chan, &data, /*timeout=*/NULL ) == CHAN_OK ) )
    TEST( data == 42 );
  return NULL;
}

static void* test_chan_send_1( void *thrd_arg ) {
  struct channel *const chan = thrd_arg;
  int data = 42;
  TEST( chan_send( chan, &data, /*timeout=*/NULL ) == CHAN_OK );
  return NULL;
}

static bool test_buf_chan( void ) {
  TEST_FUNC_BEGIN();
  struct channel chan;
  if ( TEST( chan_init( &chan, /*buf_cap=*/1, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &test_chan_recv_1, &chan );
    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &test_chan_send_1, &chan );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );
  }
  TEST_FUNC_END();
}

static bool test_unbuf_chan( void ) {
  TEST_FUNC_BEGIN();
  struct channel chan;
  if ( TEST( chan_init( &chan, /*buf_cap=*/0, sizeof(int) ) ) ) {
    pthread_t recv_thrd, send_thrd;

    // Create the receiving thread first and ensure it's ready before creating
    // the sending thread.
    PTHREAD_MUTEX_LOCK( &chan.mtx );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &test_chan_recv_1, &chan );
    PTHREAD_COND_WAIT( &chan.not_full, &chan.mtx );
    PTHREAD_MUTEX_UNLOCK( &chan.mtx );

    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &test_chan_send_1, &chan );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );

    PTHREAD_CREATE( &send_thrd, /*attr=*/NULL, &test_chan_send_1, &chan );
    PTHREAD_CREATE( &recv_thrd, /*attr=*/NULL, &test_chan_recv_1, &chan );
    PTHREAD_JOIN( recv_thrd, NULL );
    PTHREAD_JOIN( send_thrd, NULL );

    chan_close( &chan );
    chan_cleanup( &chan, /*free_fn=*/NULL );
  }
  TEST_FUNC_END();
}

////////// main ///////////////////////////////////////////////////////////////

int main( int argc, char const *argv[] ) {
  test_prog_init( argc, argv );

  test_buf_chan();
  test_unbuf_chan();
}

///////////////////////////////////////////////////////////////////////////////
/* vim:set et sw=2 ts=2: */
