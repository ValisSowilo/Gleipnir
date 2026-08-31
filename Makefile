# Gleipnir -- a context-mixing archiver
# Copyright 2026 ValisSowilo.  GPL-3.0-or-later; see LICENSE.md.
#
#   make            build ./gleipnir
#   make test       build, then round-trip a couple of files through it
#   make install    copy to $(PREFIX)/bin        (default ~/.local/bin)
#   make uninstall
#
# On Windows use build.sh instead; it links the bundled zlib and sets the
# flags MinGW needs.  This Makefile expects a system zlib (-lz), which is how
# every Linux distribution ships it:
#
#   Debian/Ubuntu   apt install build-essential zlib1g-dev
#   Fedora          dnf install gcc zlib-devel
#   Arch            pacman -S base-devel zlib

CC      ?= cc
PREFIX  ?= $(HOME)/.local

# -march=x86-64-v2, not -march=native: a native build dies with an illegal
# instruction on any older CPU, which is a miserable way to find out.  Override
# with `make ARCH=-march=native` for a machine-specific build.
ARCH    ?= -march=x86-64-v2

# -ffp-contract=off: detect_period() measures mean absolute difference in
# double precision and the stride it picks changes what the model does.
# Letting the compiler fuse multiply-add makes that arithmetic depend on the
# target CPU, which would let two builds disagree about a file.
CFLAGS  ?= -O3 -funroll-loops $(ARCH) -ffp-contract=off
LDFLAGS ?=
LIBS    ?= -lz -lm -lpthread

SRC = gleipnir.c
BIN = gleipnir

.PHONY: all test install uninstall clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LIBS)

# Not a substitute for fuzz.py / tfuzz.py / gfuzz.py, which are the real gates.
# This is the "did it come out of the compiler able to do its job" check.
test: $(BIN)
	@./$(BIN) --version
	@tmp=$$(mktemp -d) && \
	  cp $(SRC) $$tmp/big.c && cp Makefile $$tmp/small.txt && \
	  ./$(BIN) c -3 -q $$tmp/a.gl $$tmp/big.c $$tmp/small.txt && \
	  ./$(BIN) t $$tmp/a.gl && \
	  mkdir -p $$tmp/out && ./$(BIN) x -q $$tmp/a.gl $$tmp/out && \
	  cmp $$tmp/big.c $$tmp/out/big.c && \
	  cmp $$tmp/small.txt $$tmp/out/small.txt && \
	  echo "round trip OK" && rm -rf $$tmp

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	@echo "installed to $(DESTDIR)$(PREFIX)/bin/$(BIN)"
	@echo "if 'gleipnir' is not found, add $(PREFIX)/bin to your PATH"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)
