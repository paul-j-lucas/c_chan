/*
**      C Channels -- Channels Library for C
**      src/util.h
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

#ifndef C_CHAN_UTIL_H
#define C_CHAN_UTIL_H

// local
#include "config.h"                     /* must go first */

// standard
#include <attribute.h>
#include <pthread.h>
#include <sysexits.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////

/**
 * Asserts that this line of code is run at most once --- useful in
 * initialization functions that must be called at most once.  For example:
 *
 *      void initialize() {
 *        ASSERT_RUN_ONCE();
 *        // ...
 *      }
 *
 * @sa #RUN_ONCE
 */
#ifndef NDEBUG
# define ASSERT_RUN_ONCE() BLOCK(       \
    static bool UNIQUE_NAME(run_once);  \
    assert( !UNIQUE_NAME(run_once) );   \
    UNIQUE_NAME(run_once) = true; )
#else
# define ASSERT_RUN_ONCE()        NO_OP
#endif /* NDEBUG */

/**
 * Calls **atexit**(3) and checks for failure.
 *
 * @param FN The pointer to the function to call **atexit**(3) with.
 */
#define ATEXIT(FN) \
  PERROR_EXIT_IF( atexit( (FN) ) != 0, EX_OSERR )

/**
 * Embeds the given statements into a compound statement block.
 *
 * @param ... The statement(s) to embed.
 */
#define BLOCK(...)                do { __VA_ARGS__ } while (0)

/**
 * Shorthand for printing to standard error.
 *
 * @param ... The `printf()` arguments.
 */
#define EPRINTF(...)              fprintf( stderr, __VA_ARGS__ )

#ifdef HAVE___BUILTIN_EXPECT

/**
 * Specifies that \a EXPR is _very_ likely (as in 99.99% of the time) to be
 * non-zero (true) allowing the compiler to better order code blocks for
 * marginally better performance.
 *
 * @param EXPR An expression that can be cast to `bool`.
 *
 * @sa #unlikely()
 * @sa [Memory part 5: What programmers can do](http://lwn.net/Articles/255364/)
 */
#define likely(EXPR)              __builtin_expect( !!(EXPR), 1 )

/**
 * Specifies that \a EXPR is _very_ unlikely (as in .01% of the time) to be
 * non-zero (true) allowing the compiler to better order code blocks for
 * marginally better performance.
 *
 * @param EXPR An expression that can be cast to `bool`.
 *
 * @sa #likely()
 * @sa [Memory part 5: What programmers can do](http://lwn.net/Articles/255364/)
 */
#define unlikely(EXPR)            __builtin_expect( !!(EXPR), 0 )

#else
# define likely(EXPR)             (EXPR)
# define unlikely(EXPR)           (EXPR)
#endif /* HAVE___BUILTIN_EXPECT */

/**
 * Concatenate \a A and \a B together to form a single token.
 *
 * @remarks This macro is needed instead of simply using `##` when either
 * argument needs to be expanded first, e.g., `__LINE__`.
 *
 * @param A The first token.
 * @param B The second token.
 */
#define NAME2(A,B)                NAME2_HELPER(A,B)

/// @cond DOXYGEN_IGNORE
#define NAME2_HELPER(A,B)         A ## B
/// @endcond

#define PERROR_EXIT_IF(EXPR,STATUS) \
  BLOCK( if ( unlikely( (EXPR) ) ) perror_exit( (STATUS) ); )

#define PTHREAD_COND_BROADCAST(COND) \
  PERROR_EXIT_IF( pthread_cond_broadcast( (COND) ) != 0, EX_IOERR )

#define PTHREAD_COND_DESTROY(COND) \
  PERROR_EXIT_IF( pthread_cond_destroy( (COND) ) != 0, EX_IOERR )

#define PTHREAD_COND_INIT(COND,ATTR) \
  PERROR_EXIT_IF( pthread_cond_init( (COND), (ATTR) ) != 0, EX_IOERR )

#define PTHREAD_COND_SIGNAL(COND) \
  PERROR_EXIT_IF( pthread_cond_signal( (COND) ) != 0, EX_IOERR )

#define PTHREAD_COND_WAIT(COND,MTX) \
  PERROR_EXIT_IF( pthread_cond_wait( (COND), (MTX) ) != 0, EX_IOERR )

#define PTHREAD_COND_TIMEDWAIT(COND,MTX,ABSTIME) \
  PERROR_EXIT_IF( pthread_cond_timedwait( (COND), (MTX), (ABSTIME) ) != 0, EX_IOERR )

#define PTHREAD_MUTEX_DESTROY(MTX) \
  PERROR_EXIT_IF( pthread_mutex_destroy( (MTX) ) != 0, EX_IOERR )

#define PTHREAD_MUTEX_INIT(MTX,ATTR) \
  PERROR_EXIT_IF( pthread_mutex_init( (MTX), (ATTR) ) != 0, EX_IOERR )

#define PTHREAD_MUTEX_LOCK(MTX) \
  PERROR_EXIT_IF( pthread_mutex_lock( (MTX) ) != 0, EX_IOERR )

#define PTHREAD_MUTEX_UNLOCK(MTX) \
  PERROR_EXIT_IF( pthread_mutex_unlock( (MTX) ) != 0, EX_IOERR )

/**
 * Synthesises a name prefixed by \a PREFIX unique to the line on which it's
 * used.
 *
 * @param PREFIX The prefix of the synthesized name.
 *
 * @warning All uses for a given \a PREFIX that refer to the same name _must_
 * be on the same line.  This is not a problem within macro definitions, but
 * won't work outside of them since there's no way to refer to a previously
 * used unique name.
 */
#define UNIQUE_NAME(PREFIX)       NAME2(NAME2(PREFIX,_),__LINE__)

///////////////////////////////////////////////////////////////////////////////

/**
 * Extracts the base portion of a path-name.
 *
 * @remarks Unlike **basename**(3):
 *  + Trailing `/` characters are not deleted.
 *  + \a path_name is never modified (hence can therefore be `const`).
 *  + Returns a pointer within \a path_name (hence is multi-call safe).
 *
 * @param path_name The path-name to extract the base portion of.
 * @return Returns a pointer to the last component of \a path_name.  If \a
 * path_name consists entirely of `/` characters, a pointer to the string `/`
 * is returned.
 */
NODISCARD
char const* base_name( char const *path_name );

/**
 * Prints an error message for `errno` to standard error and exits.
 *
 * @param status The exit status code.
 *
 * @sa #INTERNAL_ERROR()
 * @sa #PERROR_EXIT_IF()
 * @sa #UNEXPECTED_INT_VALUE()
 */
_Noreturn void perror_exit( int status );

///////////////////////////////////////////////////////////////////////////////
#endif /* C_CHAN_UTIL_H */
