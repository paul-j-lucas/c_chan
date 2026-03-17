---
name: Bug report
about: Report a bug (build-time error, run-time error, or bad documentation).
title: ''
labels: ''
assignees: ''

---

## Notes

1. **USE THIS TEMPLATE ONLY TO REPORT BUGS**.  Anything that is not a bug will be closed and deleted immediately.

2. Please delete sections from this template that do not apply to your particular bug.

## Issue Type

There are a few types of issues:

1. **Build-time issue**: you get either a compile-time error or warning while building **c_chan**.
2. **Run-time issue**: **c_chan** doesn't behave correctly, deadlocks, or crashes.
3. **Documentation issue**: part of the documentation (man page, Doxygen, or source code comment) is wrong, unclear, or has a typographical or grammatical error.

If your issue is an enhancement request, please use the **Enhancement request** template.

If your issue is none of the above, **DO NOT CREATE AN ISSUE**. It will be closed and deleted. General comments and questions should be sent by e-mail instead.

## Build-time Issue

**What error or warning did you get?**

(Please include **_only_** the error or warning message line of output and **_not_** the entire build output unless subsequently requested.)

**What compiler and version are you using?**

(Please include the output given by your compiler when you request its version information typically via the `--version` command-line option.)

**What operating system and version (Linux, FreeBSD, macOS, etc.) are you building on?**

**Additional Comments**

(Please include any additional comments that may be relevant.)

## Run-time Issue

Please create the smallest possible test program that elicits the bug and attach it.

**If cdecl crashed, what was the crash message and backtrade?**

**Additional Comments**

(Please include any additional comments that may be relevant.)

## Documentation Issue

**What file is the documentation in (if known)?**

**Please copy & paste the relevant part of the documentation and briefly explain what's wrong with it.**
