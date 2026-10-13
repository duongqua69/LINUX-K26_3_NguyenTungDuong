# Sensor Gateway Project

Linux sensor gateway project written in C. The project is being built in stages
following the embedded Linux assignment.

## Project layout

- `src/`: C source files
- `include/`: shared header files
- `web/`: web interface assets
- `test/`: test code and fixtures
- `Makefile`: build, debug, and clean targets

## Build

```sh
make
```

This creates `./server` using GCC with `-Wall -Wextra -pthread`.

Build with debug diagnostics enabled:

```sh
make debug
```

Remove generated objects and the executable:

```sh
make clean
```
