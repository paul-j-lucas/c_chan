# C Chan

## Introduction

**C Chan**
is a library that implements Go-like channels in C.
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
