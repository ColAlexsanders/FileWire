# FileWire

A TCP client/server file-transfer tool written in C++ using abstraction libraries for file handling, and BSD sockets for the socket logic. This application includes a length-prefixed protocol, an integration test suite, sanitizer build support, and CI.


## Build

Running make would create two binaries: `server` and `client`

```sh
make -j $(nproc)
make clean
```

## Run

```sh
./server <PORT> <FILE-DIR>                    # port 1024-65535
./client <HOSTNAME-OR-IP> <PORT> <FILENAME>   # port 1024-65535
```

The server saves each transfer under its original filename inside `<FILE-DIR>`, so a file sent from the client (for example an image) arrives with its name and extension intact and opens normally on the server side.

```sh
./server 8080 ./save &
./client localhost 8080 ./photo.jpg
```

## Wire Protocol

```
+-------------+----------+--------------+----------+
| 2-byte name | filename | 8-byte size  | payload  |
| length      |          | (big-endian) |          |
+-------------+----------+--------------+----------+
```

+ 2-byte name length - A frame for the filename, provides the length of the filename itself
+ filename - what's a filename?
+ 8-byte size - The frame for the payload. Tells the receiver how many bytes the segment is, which can then be used for truncation detection.

The client sends the application header first, then the TCP segments. The server saves the file under the transmitted name and marks it `ERROR` if the connection closes early, so a truncated transfer can never look like a complete one. The header's purpose is to provide the receiver with information on when the message is supposed to end and begin since TCP doesn't preserve message boundaries. 

## Test

```sh
make test        # integration suite: transfers, size limit, truncation, error paths
make slow-test   # adds the 10s idle-timeout case
```

`tests/integration.sh` starts real `server`/`client` processes on ephemeral ports, transfers files of several sizes, verifies checksums, and checks the size-limit, truncation, and argument-error paths. It is the reason the binaries accept the env overrides below.

## Sanitizers

```sh
make asan        # -fsanitize=address,undefined
make ubsan       # -fsanitize=undefined
make tsan        # -fsanitize=thread (for potential concurrency functionality)
```

Each target cleans first, since Make cannot detect a change to `SANITIZE`.

## Environment overrides

Both binaries read these so tests can exercise slow/large paths quickly. Unset or invalid values fall back to the defaults.

| Variable        | Default      | Effect                          |
| --------------- | ------------ | ------------------------------- |
| `TIMEOUT_SEC`   | `10`         | send/recv/connect idle timeout  |
| `MAX_FILE_SIZE` | `104857600`  | 100 MiB transfer cap, in bytes  |

## Layout

```
src/server.cpp          listener + receiver
src/client.cpp          connector + sender
tests/integration.sh    end-to-end tests (drives the built binaries)
tests/fail_socket.c     shim for the bad socket test
Makefile                build, test, sanitizer, format, lint targets
.github/workflows/      CI: build+test on gcc/clang, and the three sanitizers
```

## Formatting and linting

```sh
make format                          # clang-format -i src/*.cpp
make lint                            # clang-tidy with the .clang-tidy checks defined in Makefile
make lint LINT_FLAGS=--list-checks   # list the enabled checks
```

Primarily for me to learn how linting works when building C++ applications. `.clang-format` preserves the hand-written block comments, so running `make format` doesn't break anything in my current coding style. `.clang-tidy` enables the clang-analyzer, bugprone, performance, and portability check groups.
