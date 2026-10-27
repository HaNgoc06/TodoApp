CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread
LDLIBS = -lsqlite3 -ljson-c -pthread
.PHONY: all clean v1 v2 v3
all: task-manager
task-manager: src/main.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
v1: all
	./task-manager --version 1
v2: all
	./task-manager --version 2
v3: all
	./task-manager --version 3
clean:
	rm -f task-manager
