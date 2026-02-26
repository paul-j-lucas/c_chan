/*
**      PJL Library
**      stdc_threads.h
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

#ifndef __STDC_NO_THREADS__
#include <threads.h>
#else

#ifndef pjl_stdc_threads_H
#define pjl_stdc_threads_H

/**
 * @file
 * Declares types, constants, and function wrappers around POSIX threads for
 * standard C11 threads when __STDC_NO_THREADS__ is defined to 1.
 */

// local
#include "config.h"                     /* IWYU pragma: keep */

// standard
#include <errno.h>
#include <pthread.h>
#include <time.h>

///////////////////////////////////////////////////////////////////////////////

enum {
  mtx_plain     = 0,
  mtx_timed     = 1 << 0,
  mtx_recursive = 1 << 1
};

enum {
  thrd_success,
  thrd_busy     = EBUSY,
  thrd_nomem    = ENOMEM,
  thrd_timedout = ETIMEDOUT,
  thrd_error
};

typedef pthread_cond_t  cnd_t;
typedef pthread_mutex_t mtx_t;
typedef int           (*thrd_start_t)( void* );
typedef pthread_t       thrd_t;

///////////////////////////////////////////////////////////////////////////////

int cnd_init( cnd_t *c );

inline int cnd_broadcast( cnd_t *c ) {
  return pthread_cond_broadcast( c ) == 0 ? thrd_success : thrd_error;
}

void cnd_destroy( cnd_t *c );

inline int cnd_signal( cnd_t *c ) {
  return pthread_cond_signal( c ) == 0 ? thrd_success : thrd_error;
}

int cnd_timedwait( cnd_t *c, mtx_t *m, struct timespec const *ts );

inline int cnd_wait( cnd_t *c, mtx_t *m ) {
  return pthread_cond_wait( c, m ) == 0 ? thrd_success : thrd_error;
}

void mtx_destroy( mtx_t *m );

int mtx_init( mtx_t *m, int type );

inline int mtx_lock( mtx_t *m ) {
  return pthread_mutex_lock( m ) == 0 ? thrd_success : thrd_error;
}

inline int mtx_unlock( mtx_t *m ) {
  return pthread_mutex_unlock( m ) == 0 ? thrd_success : thrd_error;
}

int thrd_create( thrd_t *t, thrd_start_t start_fn, void *user_data );

int thrd_join( thrd_t t, int *pexit_value );

///////////////////////////////////////////////////////////////////////////////
#endif /* pjl_stdc_threads_H */
#endif /* __STDC_NO_THREADS__ */
