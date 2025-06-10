/*
**      cdecl -- C gibberish translator
**      src/unit_test.c
**
**      Copyright (C) 2021-2025  Paul J. Lucas
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
#include "unit_test.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <assert.h>
#include <attribute.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

/// @endcond

///////////////////////////////////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

// extern variables
unsigned    test_fail_cnt;
char const *test_prog_name;

/// @endcond

/**
 * @addtogroup unit-test-group
 * @{
 */

////////// local functions ////////////////////////////////////////////////////

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
char const* base_name( char const *path_name ) {
  assert( path_name != NULL );
  char const *const slash = strrchr( path_name, '/' );
  if ( slash != NULL )
    return slash[1] != '\0' ? slash + 1 : path_name;
  return path_name;
}

/**
 * Called at unit-test program termination via **atexit**(3) to print the
 * number of test failures and exits with either `EX_OK` if all tests passed or
 * `EX_SOFTWARE` if at least one test failed.
 *
 * @note This function is called only via **atexit**(3).
 *
 * @sa test_prog_init()
 */
_Noreturn
static void test_prog_exit( void ) {
  printf( "%u failures\n", test_fail_cnt );
  _Exit( test_fail_cnt > 0 ? EX_SOFTWARE : EX_OK );
}

/**
 * Prints the usage message to standard error and exits.
 */
_Noreturn
static void test_prog_usage( void ) {
  // LCOV_EXCL_START
  EPRINTF( "usage: %s\n", test_prog_name );
  exit( EX_USAGE );
  // LCOV_EXCL_STOP
}

////////// extern functions ///////////////////////////////////////////////////

void test_prog_init( int argc, char const *const argv[] ) {
  ASSERT_RUN_ONCE();
  test_prog_name = base_name( argv[0] );
  if ( --argc != 0 )
    test_prog_usage();                  // LCOV_EXCL_LINE
  ATEXIT( &test_prog_exit );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
