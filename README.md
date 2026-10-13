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

This creates `./server` and the temperature sensor simulator `./sensor_node`
using GCC with `-Wall -Wextra -pthread`.

Build with debug diagnostics enabled:

```sh
make debug
```

## Step 3: temperature sensor data path

Run the gateway with a TCP base port (HTTP port remains optional for the earlier
startup interface):

```sh
./server 1234
```

In other terminals, start one or more simulated temperature sensors:

```sh
./sensor_node temperature 10 127.0.0.1 1234 500
```

In a debug build, both Data manager and Storage manager print each received
measurement once. Stop the gateway with Ctrl+C; it wakes both blocked sbuffer
readers and closes active sensor connections. Connection manager uses `poll()`
to serve all sensor sockets in one thread, avoiding one thread per client while
still allowing concurrent connections. The shared sbuffer uses a mutex and
condition variable; each item is freed after both readers have consumed it.

Remove generated objects and the executable:

```sh
make clean
```
