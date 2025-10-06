/*
**      c_chan -- Channels Library for C
**      src/unit_test.h
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

#ifndef c_chan_unit_test_H
#define c_chan_unit_test_H

#pragma GCC diagnostic ignored "-Wunused-value"

// standard
#include <stdio.h>

/**
 * @defgroup unit-test-group Unit Tests
 * Macros, variables, and functions for unit-test programs.
 * @{
 */

///////////////////////////////////////////////////////////////////////////////

/**
 * Tests \a EXPR: only if it fails, evaluates \a INC_FAIL_CNT_EXPR and prints
 * that it failed.
 *
 * @param EXPR The expression to evaluate.
 * @param INC_FAIL_CNT_EXPR The expression to evaluate only if \a EXPR is
 * false.
 * @return Returns `true` only if \a EXPR is non-zero; `false` only if zero.
 */
#define TEST_INC_FAIL_CNT(EXPR,INC_FAIL_CNT_EXPR) \
  ( !!(EXPR) ||                                   \
    ( (INC_FAIL_CNT_EXPR),                        \
      !fprintf( stderr, "%s:%d: " #EXPR "\n", test_prog_name, __LINE__ ) ) )

///////////////////////////////////////////////////////////////////////////////

// extern variables
extern unsigned     test_fail_cnt;      ///< Test failure count.
extern char const  *test_prog_name;     ///< Program name.

/**
 * Initializes a unit-test program.
 *
 * @note This function must be called exactly once.
 *
 * @param argc The command-line argument count.
 * @param argv The command-line argument values.
 */
void test_prog_init( int argc, char const *const argv[] );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* c_chan_unit_test_H */
/* vim:set et sw=2 ts=2: */
