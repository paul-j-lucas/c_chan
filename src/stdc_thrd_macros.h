/*
**      PJL Library
**      stdc_thrd_macros.h
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

#ifndef pjl_stdc_thrd_macros_H
#define pjl_stdc_thrd_macros_H

/**
 * @file
 * Macro wrappers around standard C thread functions that check for errors and
 * abort if any.
 */

// local
#include "stdc_threads.h"               /* IWYU pragma: export */
#include "util.h"

///////////////////////////////////////////////////////////////////////////////

#define CND_BROADCAST(COND) \
  ASSERT_EQ( cnd_broadcast( (COND) ), thrd_success )

#define CND_DESTROY(COND) \
  cnd_destroy( (COND) )

#define CND_INIT(COND) \
  ASSERT_EQ( cnd_init( (COND) ), thrd_success )

#define CND_SIGNAL(COND) \
  ASSERT_EQ( cnd_signal( (COND) ), thrd_success )

#define CND_WAIT(COND,MTX) \
  ASSERT_EQ( cnd_wait( (COND), (MTX) ), thrd_success )

#define CND_TIMEDWAIT(COND,MTX,ABSTIME) \
  ASSERT_EQ( cnd_timedwait( (COND), (MTX), (ABSTIME) ), thrd_success )

#define THRD_CREATE(THR,START_FN,ARG) \
  ASSERT_EQ( thrd_create( (THR), (START_FN), (ARG) ), thrd_success )

#define THRD_JOIN(THR,VALUE) \
  ASSERT_EQ( thrd_join( (THR), (VALUE) ), thrd_success )

#define MTX_DESTROY(MTX) \
  mtx_destroy( (MTX) )

#define MTX_INIT(MTX,TYPE) \
  ASSERT_EQ( mtx_init( (MTX), (TYPE) ), thrd_success )

#define MTX_LOCK(MTX) \
  ASSERT_EQ( mtx_lock( (MTX) ), thrd_success )

#define MTX_UNLOCK(MTX) \
  ASSERT_EQ( mtx_unlock( (MTX) ), thrd_success )

///////////////////////////////////////////////////////////////////////////////

#endif /* pjl_stdc_thrd_macros_H */
