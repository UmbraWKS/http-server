CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
LDFLAGS ?= -pthread

TARGET  := http_server
SRC     := main.c
HDR     := http_server.h

.PHONY: all debug run clean

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

debug: CFLAGS = -std=gnu11 -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: clean $(TARGET)

# The server serves files from ./server-files/ by default
run: $(TARGET)
	@mkdir -p server-files
	./$(TARGET)

clean:
	rm -f $(TARGET)
