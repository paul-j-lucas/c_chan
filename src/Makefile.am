##
#       c_chan -- Channels Library for C
#	src/Makefile.am
#
#	Copyright (C) 2025-2026  Paul J. Lucas
#
#	This program is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	(at your option) any later version.
#
#	This program is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
#	You should have received a copy of the GNU General Public License
#	along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

lib_LTLIBRARIES =	libc_chan.la
include_HEADERS =	c_chan.h

AM_CFLAGS =		$(C_CHAN_CFLAGS) $(PTHREAD_CFLAGS)
if ENABLE_ASAN
AM_CFLAGS +=		-fsanitize=address -fno-omit-frame-pointer
endif
if ENABLE_MSAN
AM_CFLAGS +=		-fsanitize=memory -fno-omit-frame-pointer
endif
if ENABLE_UBSAN
AM_CFLAGS +=		-fsanitize=undefined
endif

AM_CPPFLAGS =		-I$(top_srcdir)/lib -I$(top_builddir)/lib
LDADD =			$(top_builddir)/lib/libgnu.a

libc_chan_la_LDFLAGS =	-version-info 1:0:0

libc_chan_la_SOURCES =	c_chan.c $(include_HEADERS) \
			util.c util.h

check_PROGRAMS =	c_chan_test
c_chan_test_SOURCES =	$(libc_chan_a_SOURCES) \
			c_chan_test.c \
			unit_test.c unit_test.h

c_chan_test_LDADD =	libc_chan.la $(PTHREAD_LIBS)

TESTS =			$(check_PROGRAMS)

# vim:set noet sw=8 ts=8:
