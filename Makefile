CC := gcc
CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -pthread -MMD -MP
LDFLAGS := -pthread

TARGET := server
SOURCES := src/main.c src/logger.c src/sbuffer.c src/connmgr.c
OBJECTS := $(SOURCES:.c=.o)
SENSOR_NODE := sensor_node
DEPENDENCIES := $(OBJECTS:.o=.d) src/sensor_node.d

.PHONY: all debug clean

all: $(TARGET) $(SENSOR_NODE)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(SENSOR_NODE): src/sensor_node.o
	$(CC) $^ -o $@

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

debug: clean
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DDEBUG" CFLAGS="$(CFLAGS) -g" all

clean:
	rm -f $(OBJECTS) src/sensor_node.o $(DEPENDENCIES) $(TARGET) $(SENSOR_NODE)

-include $(DEPENDENCIES)
