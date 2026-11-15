# BlackHole Browser
CC      ?= gcc
PKGS     = openssl zlib freetype2 x11
CSTD     = -std=gnu11 -D_GNU_SOURCE
WARN     = -Wall -Wextra -Wshadow -Wformat=2 -Wno-unused-parameter
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-strict-overflow
CFLAGS  ?= -O2 -g
CFLAGS  += $(CSTD) $(WARN) $(HARDEN) $(shell pkg-config --cflags $(PKGS))
LDFLAGS += -pie -Wl,-z,relro,-z,now
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

SRC      = $(wildcard src/*.c)
OBJ      = $(SRC:src/%.c=build/%.o)
BIN      = build/blackhole

all: $(BIN)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build:
	mkdir -p build

# Debug build with AddressSanitizer + UndefinedBehaviorSanitizer
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

test: $(BIN)
	./tests/run.sh

clean:
	rm -rf build

.PHONY: all asan test clean
