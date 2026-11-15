# BlackHole Browser
CC      ?= gcc
PKGS     = openssl zlib freetype2 x11
CSTD     = -std=gnu11 -D_GNU_SOURCE
WARN     = -Wall -Wextra -Wshadow -Wformat=2 -Wno-unused-parameter
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-strict-overflow
OPT     ?= -O2 -g
SAN     ?=
CFLAGS   = $(OPT) $(SAN) $(CSTD) $(WARN) $(HARDEN) $(shell pkg-config --cflags $(PKGS))
LDFLAGS  = -pie -Wl,-z,relro,-z,now $(SAN)
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

SRC      = $(wildcard src/*.c)
BDIR    ?= build
OBJ      = $(SRC:src/%.c=$(BDIR)/%.o)
BIN      = $(BDIR)/blackhole

all: $(BIN)

$(BDIR)/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(BDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Debug build with AddressSanitizer + UndefinedBehaviorSanitizer (separate directory)
asan:
	$(MAKE) BDIR=build-asan OPT="-O1 -g -fno-omit-frame-pointer" SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"

test: all
	./tests/run.sh

clean:
	rm -rf build build-asan

.PHONY: all asan test clean
