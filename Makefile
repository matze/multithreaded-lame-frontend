CFLAGS ?= -O2 -Wall
CFLAGS += -pthread
CXXFLAGS += -std=c++17 -lstdc++fs
LDFLAGS += -lmp3lame

.PHONY: clean

all: encoder-c encoder-cc

clean:
	rm -f encoder

encoder-c: main.c
	$(CC) -o $@ $(CFLAGS) $< $(LDFLAGS)

encoder-cc: main.cc
	$(CXX) -o $@ $(CFLAGS) $< $(CXXFLAGS) $(LDFLAGS)
