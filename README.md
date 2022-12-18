# Jiayi_Wang_Shell
A simple shell with basic command line functions and file I/O.

## Intro
* Constructed Linux interactive command-line interpreter with built-in commands, foreground/background job control, system
signal handling, file I/O redirection, and error handling.
* Achieved concurrent background job management by forking processes; ensured safety of concurrent programming through use
of system signal blocking/unblocking and async-signal-safe functions.
* Implemented signal handlers in response to SIGCHLD, SIGINT, and SIGSTP signals to assist communication between multiple
processes.

## Note
This repo only contains the implementation without the auxiliary helper functions provided as starter code due to privacy reason.
