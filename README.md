# C Chan

## Introduction

**C Chan**
is a library that implements Go-like channels in C
for an alternative way to share data
among threads
rather than using mutexes explicitly.

As in Go,
there are two types of channels:

1. **Buffered**: the channel has a buffer of a fixed capacity.
2. **Unbuffered**: A sender and receiver will wait until both are
   simultaneously ready.

In addition to
`chan_send()`
and
`chan_recv()`,
`chan_select()`
can be either
blocking
or
non-blocking.
Unlike Go,
timeouts may optionally be specified
for all operations.

### Message Management

**C Chan**
treats messages as opaque
chunks of memory
of a pre-set,
fixed
size.
Hence messages may be copies of your actual data
(e.g., integers)
or may be pointers to your data
(i.e., `T*` for some type `T`).
If pointers,
both memory management
and thread-safety
of the pointed-to data
is entirely left to you.

## Installation

The git repository contains only the necessary source code.
Things like `configure` are _derived_ sources and
[should not be included in repositories](http://stackoverflow.com/a/18732931).
If you have
[`autoconf`](https://www.gnu.org/software/autoconf/),
[`automake`](https://www.gnu.org/software/automake/),
and
[`m4`](https://www.gnu.org/software/m4/)
installed,
you can generate `configure` yourself by doing:

    ./bootstrap

Then follow the generic installation instructions given in
[`INSTALL`](https://github.com/paul-j-lucas/c_exception/blob/master/INSTALL).

If you would like to generate the developer documentation,
you will also need
[Doxygen](http://www.doxygen.org/);
then do:

    make doc                            # or: make docs

**Paul J. Lucas**  
San Francisco Bay Area, California, USA  
15 May 2025
