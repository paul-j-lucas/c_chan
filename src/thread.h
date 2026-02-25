/*
**      c_chan -- Channels Library for C
**      src/thread.h
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

#ifndef C_CHAN_THREAD_H
#define C_CHAN_THREAD_H

// local
#include "config.h"                     /* IWYU pragma: keep */
#include "util.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <pthread.h>                    /* IWYU pragma: export */
#include <sysexits.h>                   /* IWYU pragma: export */

/// @endcond

/**
 * @defgroup thread-group Thread Macros
 * Thread macros.
 * @{
 */

///////////////////////////////////////////////////////////////////////////////

/**
 * Calls **pthread_cond_broadcast**(3), checks for an error, and exits if there
 * was one.
 *
 * @param COND The condition variable to broadcast.
 *
 * @sa #PTHREAD_COND_SIGNAL()
 * @sa #PTHREAD_COND_TIMEDWAIT()
 * @sa #PTHREAD_COND_WAIT()
 */
#define PTHREAD_COND_BROADCAST(COND) \
  PERROR_EXIT_IF( pthread_cond_broadcast( (COND) ) != 0, EX_IOERR )

/**
 * Calls **pthread_cond_destroy**(3), checks for an error, and exits if there
 * was one.
 *
 * @param COND The condition variable to destroy.
 *
 * @sa #PTHREAD_COND_INIT()
 */
#define PTHREAD_COND_DESTROY(COND) \
  PERROR_EXIT_IF( pthread_cond_destroy( (COND) ) != 0, EX_IOERR )

/**
 * Calls **pthread_cond_init**(3), checks for an error, and exits if there was
 * one.
 *
 * @param COND The condition variable to initialize.
 * @param ATTR The attributes to use, if any.
 *
 * @sa #PTHREAD_COND_DESTROY()
 */
#define PTHREAD_COND_INIT(COND,ATTR) \
  PERROR_EXIT_IF( pthread_cond_init( (COND), (ATTR) ) != 0, EX_IOERR )

/**
 * Calls **pthread_cond_signal**(3), checks for an error, and exits if there
 * was one.
 *
 * @param COND The condition variable to signal.
 *
 * @sa #PTHREAD_COND_BROADCAST()
 * @sa #PTHREAD_COND_TIMEDWAIT()
 * @sa #PTHREAD_COND_WAIT()
 */
#define PTHREAD_COND_SIGNAL(COND) \
  PERROR_EXIT_IF( pthread_cond_signal( (COND) ) != 0, EX_IOERR )

/**
 * Calls **pthread_cond_wait**(3), checks for an error, and exits if there was
 * one.
 *
 * @param COND The condition variable to wait for.
 * @param MTX The mutex to unlock temporarily.
 *
 * @sa #PTHREAD_COND_BROADCAST()
 * @sa #PTHREAD_COND_SIGNAL()
 * @sa #PTHREAD_COND_TIMEDWAIT()
 */
#define PTHREAD_COND_WAIT(COND,MTX) \
  PERROR_EXIT_IF( pthread_cond_wait( (COND), (MTX) ) != 0, EX_IOERR )

/**
 * Calls **pthread_cond_timedwait**(3), checks for an error, and exits if there
 * was one.
 *
 * @param COND The condition variable to wait for.
 * @param MTX The mutex to unlock temporarily.
 * @param ABSTIME The absolute time to wait until.
 *
 * @sa #PTHREAD_COND_BROADCAST()
 * @sa #PTHREAD_COND_SIGNAL()
 * @sa #PTHREAD_COND_WAIT()
 */
#define PTHREAD_COND_TIMEDWAIT(COND,MTX,ABSTIME) \
  PERROR_EXIT_IF( pthread_cond_timedwait( (COND), (MTX), (ABSTIME) ) != 0, EX_IOERR )

/**
 * Calls **pthread_create**(3), checks for an error, and exits if there was
 * one.
 *
 * @param THR The thread variable to create.
 * @param ATTR The attributes to use, if any.
 * @param START_FN The thread's start function.
 * @param ARG The argument passed to \a START_FN.
 *
 * @sa #PTHREAD_JOIN()
 */
#define PTHREAD_CREATE(THR,ATTR,START_FN,ARG) \
  PERROR_EXIT_IF( pthread_create( (THR), (ATTR), (START_FN), (ARG) ) != 0, EX_IOERR )

/**
 * Calls **pthread_join**(3), checks for an error, and exits if there was
 * one.
 *
 * @param THR The thread to join.
 * @param VALUE A pointer to receive the start function's return value, if any.
 *
 * @sa #PTHREAD_CREATE()
 */
#define PTHREAD_JOIN(THR,VALUE) \
  PERROR_EXIT_IF( pthread_join( (THR), (VALUE) ) != 0, EX_IOERR )

/**
 * Calls **pthread_mutex_destroy**(3), checks for an error, and exits if there
 * was one.
 *
 * @param MTX The mutex variable to destroy.
 *
 * @sa #PTHREAD_MUTEX_INIT()
 */
#define PTHREAD_MUTEX_DESTROY(MTX) \
  PERROR_EXIT_IF( pthread_mutex_destroy( (MTX) ) != 0, EX_IOERR )

/**
 * Calls **pthread_mutex_init**(3), checks for an error, and exits if there was
 * one.
 *
 * @param MTX The mutex variable to initialize.
 * @param ATTR The attributes to use, if any.
 *
 * @sa #PTHREAD_MUTEX_DESTROY()
 */
#define PTHREAD_MUTEX_INIT(MTX,ATTR) \
  PERROR_EXIT_IF( pthread_mutex_init( (MTX), (ATTR) ) != 0, EX_IOERR )

/**
 * Calls **pthread_mutex_lock**(3), checks for an error, and exits if there was
 * one.
 *
 * @param MTX The mutex variable to lock.
 *
 * @sa #PTHREAD_MUTEX_UNLOCK()
 */
#define PTHREAD_MUTEX_LOCK(MTX) \
  PERROR_EXIT_IF( pthread_mutex_lock( (MTX) ) != 0, EX_IOERR )

/**
 * Calls **pthread_mutex_unlock**(3), checks for an error, and exits if there
 * was one.
 *
 * @param MTX The mutex variable to unlock.
 *
 * @sa #PTHREAD_MUTEX_LOCK()
 */
#define PTHREAD_MUTEX_UNLOCK(MTX) \
  PERROR_EXIT_IF( pthread_mutex_unlock( (MTX) ) != 0, EX_IOERR )

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* C_CHAN_THREAD_H */
