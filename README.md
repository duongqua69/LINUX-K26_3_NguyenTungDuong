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

This creates `./server` and the sensor simulator `./sensor_node`
using GCC with `-Wall -Wextra -pthread`.

Build with debug diagnostics enabled:

```sh
make debug
```

## Steps 3–4: sensor data path and protocol support

Run the gateway with a TCP base port (HTTP port remains optional for the earlier
startup interface):

```sh
./server 1234
```

The gateway listens on four consecutive TCP ports: temperature at P, humidity
at P+1, motion and door at P+2, and smoke/gas at P+3. Simulators accept the
types `temperature`, `humidity`, `motion`, `door`, and `smoke_gas`:

```sh
./sensor_node temperature 10 127.0.0.1 1234 500 --fragment
./sensor_node humidity 21 127.0.0.1 1234 500 --glue
./sensor_node motion 30 127.0.0.1 1234 500
./sensor_node door 40 127.0.0.1 1234 500 --corrupt
./sensor_node smoke_gas 50 127.0.0.1 1234 500
```

Simulator options `--fragment`, `--glue`, and `--corrupt` exercise TCP frame
assembly, parsing of multiple frames in one read, and malformed frame handling.
In a debug build, Data manager prints each received measurement; Storage manager
reports its SQLite connection and table status through the logger. Run parser
checks with `make test`. Stop the gateway with
Ctrl+C; it wakes both blocked sbuffer readers and closes active sensor
connections. Connection manager uses `poll()` to serve sockets on all four ports
in one thread. The shared sbuffer uses a mutex and condition variable; each
item is freed after both readers have consumed it. See [step4_report.md](step4_report.md)
for protocol details and verification results.

## Step 5: Data manager

The gateway loads the allowed room/sensor/type catalog from
`room_sensor.map` at startup. A different file can be selected as the third
argument after the HTTP port, for example `./server 1234 8080 config.map`.
Data manager rejects unknown IDs and protocol/type mismatches. Temperature and
humidity alerts use a rolling average of five samples; smoke/gas alarms use the
current sample; motion and door events are logged as events or state changes.
The sensor simulator accepts `--value <number>` for threshold checks:

```sh
./sensor_node temperature 10 127.0.0.1 1234 100 --value 25
./sensor_node humidity 21 127.0.0.1 1234 100 --value 20
./sensor_node smoke_gas 50 127.0.0.1 1234 100 --value 350
```

Run parser and Data manager unit checks with `make test`. See
[step5_report.md](step5_report.md) for the state and threshold behavior.

## Steps 6–7: SQLite storage and orderly shutdown

The Storage manager writes every normalized measurement to the `SensorData`
table in `sensor_data.sqlite`. The optional database path follows the room map:

```sh
./server <base-port> [http-port [room-sensor-file [database-file]]]
```

For example, `./server 1234 8080 room_sensor.map data/gateway.sqlite`. Storage
keeps each sbuffer item pending until SQLite commits its INSERT. If opening or
writing the database fails, it logs the failure and retries three times with a
one-second delay; exhaustion requests the same orderly gateway shutdown used by
SIGINT/SIGTERM. On Ctrl+C, the managers are joined, sockets and SQLite are
closed, the log FIFO is closed, the logger child is waited for, and buffered
memory is released. See [step6_report.md](step6_report.md) and
[step7_report.md](step7_report.md) for implementation and verification notes.

### Run the full project demo

Use the Make target to launch the gateway plus temperature, humidity, motion,
door, and smoke/gas sensor nodes. The target follows `gateway.log` in the same
terminal so you can watch connections, measurements, and alerts as they happen:

```sh
make run
```

The defaults are base port `1234`, HTTP port `8080`, and a 500 ms sensor cycle.
Override them when needed:

```sh
make run BASE_PORT=2234 HTTP_PORT=9080 SENSOR_INTERVAL_MS=1000
```

Press Ctrl+C in that terminal to stop the sensor nodes and gateway cleanly.

Remove generated objects and the executable:

```sh
make clean
```
