CC := gcc
CPPFLAGS := -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -pthread
LDFLAGS := -pthread

TARGET := server
SOURCES := src/main.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all debug clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

debug: clean
	$(MAKE) CPPFLAGS="$(CPPFLAGS) -DDEBUG" CFLAGS="$(CFLAGS) -g" $(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET)
