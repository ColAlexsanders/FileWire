# FileWire build. Plain Make by design (no CMake): `make` builds both binaries,
# `make test` runs the integration suite, and `make asan|ubsan|tsan` rebuild from
# scratch under a sanitizer.
CXX      ?= g++
CXXFLAGS ?= -g -O2 -Wall -Wextra -Wpedantic -Werror -std=c++17
SRC_DIR  := src

# SANITIZE=address|undefined|thread adds instrumentation; unset means a normal build.
# + make cannot see this variable change, so the sanitizer targets clean first.
SANITIZE ?=
ifneq ($(SANITIZE),)
SANITIZE_FLAGS := -fsanitize=$(SANITIZE) -fno-omit-frame-pointer -g
ifneq (,$(filter address undefined,$(SANITIZE)))
SANITIZE_FLAGS += -fno-sanitize-recover=all
endif
endif

BINARIES := server client

.PHONY: all server client test slow-test asan ubsan tsan format lint clean dist

all: $(BINARIES)

server: $(SRC_DIR)/server.cpp
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) -o $@ $<

client: $(SRC_DIR)/client.cpp
	$(CXX) $(CXXFLAGS) $(SANITIZE_FLAGS) -o $@ $<

# Integration tests drive the real binaries, so they need `all` first.
test: all
	tests/integration.sh

# RUN_SLOW=1 also runs the idle-timeout case (adds a few seconds).
slow-test: all
	RUN_SLOW=1 tests/integration.sh

asan:
	$(MAKE) clean
	$(MAKE) SANITIZE=address test

ubsan:
	$(MAKE) clean
	$(MAKE) SANITIZE=undefined test

tsan:
	$(MAKE) clean
	$(MAKE) SANITIZE=thread test

format:
	clang-format -i $(SRC_DIR)/*.cpp

lint:
	clang-tidy $(SRC_DIR)/server.cpp -- $(CXXFLAGS)
	clang-tidy $(SRC_DIR)/client.cpp -- $(CXXFLAGS)

clean:
	rm -rf *.o *~ *.gch *.swp *.dSYM $(BINARIES) *.tar.gz

dist: clean
	tar -cvzf /tmp/FileWire.tar.gz --exclude=./.git . && mv /tmp/FileWire.tar.gz .
