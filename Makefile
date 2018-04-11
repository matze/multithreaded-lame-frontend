CFLAGS ?= -O2 -Wall
CFLAGS += -pthread
LDFLAGS += -lmp3lame

.PHONY: clean

all: encoder

clean:
	rm -f encoder

encoder: main.c
	$(CC) -o $@ $(CFLAGS) $< $(LDFLAGS)
