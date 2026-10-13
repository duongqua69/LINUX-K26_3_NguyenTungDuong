CC := gcc
CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -pthread -MMD -MP
LDFLAGS := -pthread
SQLITE_LIBS ?= -l:libsqlite3.so.0

TARGET := server
SOURCES := src/main.c src/logger.c src/sbuffer.c src/connmgr.c src/protocol_adapters.c src/datamgr.c src/storagemgr.c
OBJECTS := $(SOURCES:.c=.o)
SENSOR_NODE := sensor_node
BASE_PORT ?= 1234
HTTP_PORT ?= 8080
SENSOR_INTERVAL_MS ?= 500
DEPENDENCIES := $(OBJECTS:.o=.d) src/sensor_node.d test/test_protocol_adapters.d \
	test/test_datamgr.d

.PHONY: all debug test run demo valgrind helgrind run-valgrind run-helgrind clean

all: $(TARGET) $(SENSOR_NODE)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(SQLITE_LIBS) -o $@

$(SENSOR_NODE): src/sensor_node.o
	$(CC) $^ -o $@

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

debug: clean
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DDEBUG" CFLAGS="$(CFLAGS) -g" all

test: test/test_protocol_adapters test/test_datamgr
	./test/test_protocol_adapters
	./test/test_datamgr

# Run the gateway and six representative sensor nodes; Ctrl+C stops the demo.
run demo: all
	BASE_PORT="$(BASE_PORT)" HTTP_PORT="$(HTTP_PORT)" \
	SENSOR_INTERVAL_MS="$(SENSOR_INTERVAL_MS)" ./scripts/run_demo.sh

valgrind run-valgrind: all
	ANALYZER=memcheck BASE_PORT="$(BASE_PORT)" HTTP_PORT="$(HTTP_PORT)" \
	SENSOR_INTERVAL_MS="$(SENSOR_INTERVAL_MS)" ./scripts/run_demo.sh

helgrind run-helgrind: all
	ANALYZER=helgrind BASE_PORT="$(BASE_PORT)" HTTP_PORT="$(HTTP_PORT)" \
	SENSOR_INTERVAL_MS="$(SENSOR_INTERVAL_MS)" ./scripts/run_demo.sh

test/test_protocol_adapters: test/test_protocol_adapters.c src/protocol_adapters.c
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_protocol_adapters.c \
		src/protocol_adapters.c -lm -o $@

test/test_datamgr: test/test_datamgr.c src/datamgr.c
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_datamgr.c src/datamgr.c -o $@

clean:
	rm -f $(OBJECTS) src/sensor_node.o $(DEPENDENCIES) $(TARGET) $(SENSOR_NODE) \
		test/test_protocol_adapters test/test_datamgr

-include $(DEPENDENCIES)
