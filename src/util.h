/*
**      c_chan -- Channels Library for C
**      src/util.h
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

#ifndef C_CHAN_UTIL_H
#define C_CHAN_UTIL_H

// local
#include "config.h"                     /* IWYU pragma: keep */

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <sysexits.h>                   /* IWYU pragma: export */

/// @endcond

/**
 * @defgroup util-group Utility Macros and Functions
 * Utility macros and functions.
 * @{
 */

///////////////////////////////////////////////////////////////////////////////

/**
 * Gets the number of elements of the given array.
 *
 * @param ARRAY The array to get the number of elements of.
 * @return Returns the number of elements of \a ARRAY.
 *
 * @note \a ARRAY _must_ be a statically allocated array.
 */
#define ARRAY_SIZE(ARRAY)       (sizeof( (ARRAY) ) / sizeof( 0[ (ARRAY) ] ))

/**
 * Asserts that this line of code is run at most once --- useful in
 * initialization functions that must be called at most once.  For example:
 *
 *      void initialize() {
 *        ASSERT_RUN_ONCE();
 *        // ...
 *      }
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
 * Calls **clock_gettime**(3) and checks for failure.
 *
 * @param CLOCK_ID The clock ID to use.
 * @param PTS A pointer to the `timespec` `struct` to store the time into.
 */
#define CLOCK_GETTIME(CLOCK_ID,PTS) \
  PERROR_EXIT_IF( clock_gettime( (CLOCK_ID), (PTS) ) != 0, EX_OSERR )

/**
 * Shorthand for printing to standard error.
 *
 * @param ... The `printf()` arguments.
 *
 * @sa #EPUTC()
 * @sa #EPUTS()
 */
#define EPRINTF(...)              fprintf( stderr, __VA_ARGS__ )

/**
 * Shorthand for printing a character to standard error.
 *
 * @param C The character to print.
 *
 * @sa #EPRINTF()
 * @sa #EPUTS()
 */
#define EPUTC(C)                  fputc( (C), stderr )

/**
 * Shorthand for printing a C string to standard error.
 *
 * @param S The C string to print.
 *
 * @sa #EPRINTF()
 * @sa #EPUTC()
 */
#define EPUTS(S)                  fputs( (S), stderr )

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

/**
 * If \a EXPR is `true`, prints an error message for `errno` to standard error
 * and exits with status \a STATUS.
 *
 * @param EXPR The expression.
 * @param STATUS The exit status code.
 *
 * @sa perror_exit()
 */
#define PERROR_EXIT_IF(EXPR,STATUS) \
  BLOCK( if ( unlikely( (EXPR) ) ) perror_exit( (STATUS) ); )

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
 * Prints an error message for `errno` to standard error and exits.
 *
 * @param status The exit status code.
 *
 * @sa #PERROR_EXIT_IF()
 */
_Noreturn void perror_exit( int status );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* C_CHAN_UTIL_H */
