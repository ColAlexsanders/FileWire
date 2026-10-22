#!/usr/bin/env bash
# FileWire integration tests: drive the real server/client binaries end to end.
# Env: RUN_SLOW=1 also runs the idle-timeout case (adds a few seconds).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="$ROOT/server"
CLIENT="$ROOT/client"

if [ ! -x "$SERVER" ] || [ ! -x "$CLIENT" ]; then
    echo "build the binaries first: make" >&2
    exit 2
fi

pass_count=0
fail_count=0
pass() { printf '  ok    %s\n' "$1"; pass_count=$((pass_count + 1)); }
fail() { printf '  FAIL  %s\n' "$1"; fail_count=$((fail_count + 1)); }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

server_pid=""
server_log=""
port=""

# kill -0 succeeds for a zombie, so consult the process state instead.
process_alive() {
    local pid="$1" state
    [ -d "/proc/$pid" ] || return 1
    state="$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)"
    [ -n "$state" ] && [ "$state" != "Z" ]
}

find_free_port() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
    else
        shuf -i 20000-60000 -n 1
    fi
}

# start_server <recv_dir> [ENV=VAL ...]
start_server() {
    local dir="$1"
    shift
    server_log="$work/server-$RANDOM.log"
    env "$@" "$SERVER" "$port" "$dir" >"$server_log" 2>&1 &
    server_pid=$!

    local i
    for i in $(seq 1 100); do
        if grep -q "server listening" "$server_log" 2>/dev/null; then
            return 0
        fi
        if ! process_alive "$server_pid"; then
            echo "server exited during startup:" >&2
            cat "$server_log" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "server never reported listening:" >&2
    cat "$server_log" >&2
    return 1
}

# The server only checks its shutdown flag between connections, so SIGINT (which
# it does not handle) is the reliable way to stop it here.
stop_server() {
    [ -n "$server_pid" ] || return 0
    kill -INT "$server_pid" 2>/dev/null
    local i
    for i in $(seq 1 40); do
        process_alive "$server_pid" || break
        sleep 0.05
    done
    kill -KILL "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    server_pid=""
}

# wait_for_size <file> <bytes> [seconds]
wait_for_size() {
    local file="$1" want="$2" limit="${3:-5}"
    local deadline=$((SECONDS + limit))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$file" ] && [ "$(stat -c %s "$file")" = "$want" ]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

make_fixture() { # <bytes> <path>
    head -c "$1" /dev/urandom > "$2"
}

digest() { sha256sum "$1" | cut -d' ' -f1; }

test_transfer() { # <size>
    local size="$1" name="transfer-$1"
    local dir="$work/$name"
    local src="$work/$name.bin"
    local recv="$dir/$name.bin"
    mkdir -p "$dir"
    make_fixture "$size" "$src"

    port="$(find_free_port)"
    if ! start_server "$dir"; then
        fail "$name (server start)"
        stop_server
        return
    fi

    "$CLIENT" 127.0.0.1 "$port" "$src" >"$work/$name.client.log" 2>&1
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "$name (client exit $rc)"
        cat "$work/$name.client.log" >&2
        stop_server
        return
    fi
    if ! wait_for_size "$recv" "$size"; then
        fail "$name (received file size mismatch)"
        stop_server
        return
    fi
    if [ "$(digest "$src")" != "$(digest "$recv")" ]; then
        fail "$name (checksum mismatch)"
        stop_server
        return
    fi

    pass "$name"
    stop_server
}

test_oversize() {
    local name="oversize"
    local dir="$work/$name"
    local src="$work/$name.bin"
    local recv="$dir/$name.bin"
    mkdir -p "$dir"
    make_fixture 2097152 "$src" # 2 MiB, over the 1 MiB server limit below

    port="$(find_free_port)"
    if ! start_server "$dir" MAX_FILE_SIZE=1048576; then
        fail "$name (server start)"
        stop_server
        return
    fi

    "$CLIENT" 127.0.0.1 "$port" "$src" >"$work/$name.client.log" 2>&1 || true

    if wait_for_size "$recv" 5 && [ "$(cat "$recv")" = "ERROR" ]; then
        pass "$name"
    else
        fail "$name (expected a 5-byte ERROR marker)"
    fi
    stop_server
}

test_truncated_transfer() {
    local name="truncated-transfer"
    local dir="$work/$name"
    local recv="$dir/trunc.bin"
    mkdir -p "$dir"

    port="$(find_free_port)"
    if ! start_server "$dir"; then
        fail "$name (server start)"
        stop_server
        return
    fi

    # Declare "trunc.bin" and 100 bytes, but send only "hello", then hang up.
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    printf '\x00\x09trunc.bin\x00\x00\x00\x00\x00\x00\x00\x64hello' >&3
    exec 3>&- 3<&-

    if wait_for_size "$recv" 5 && [ "$(cat "$recv")" = "ERROR" ]; then
        pass "$name"
    else
        fail "$name (expected ERROR for a truncated transfer)"
    fi
    stop_server
}

test_client_size_check() {
    local name="client-size-check"
    local dir="$work/$name"
    local src="$work/$name.bin"
    mkdir -p "$dir"
    make_fixture 2097152 "$src" # 2 MiB, over the 1 MiB client limit below

    port="$(find_free_port)"
    if ! start_server "$dir"; then
        fail "$name (server start)"
        stop_server
        return
    fi

    if MAX_FILE_SIZE=1048576 "$CLIENT" 127.0.0.1 "$port" "$src" >"$work/$name.log" 2>&1; then
        fail "$name (client accepted an oversized file)"
    elif ! grep -q "too large" "$work/$name.log"; then
        fail "$name (client failed for the wrong reason)"
    elif grep -q "Sent " "$work/$name.log"; then
        fail "$name (client sent bytes before checking the size)"
    else
        pass "$name"
    fi
    stop_server
}

test_connection_refused() {
    local name="connection-refused"
    local src="$work/$name.bin"
    make_fixture 1024 "$src"

    port="$(find_free_port)" # nothing is listening here
    if "$CLIENT" 127.0.0.1 "$port" "$src" >"$work/$name.client.log" 2>&1; then
        fail "$name (client unexpectedly succeeded)"
    else
        pass "$name"
    fi
}

test_usage_errors() {
    local name="usage-errors"
    if "$SERVER" >"$work/$name.server.log" 2>&1; then
        fail "$name (server accepted missing args)"
    elif "$CLIENT" >"$work/$name.client.log" 2>&1; then
        fail "$name (client accepted missing args)"
    else
        pass "$name"
    fi
}

test_bad_directory() {
    local name="bad-directory"
    port="$(find_free_port)"
    if "$SERVER" "$port" "$work/does-not-exist" >"$work/$name.log" 2>&1; then
        fail "$name (server accepted a missing directory)"
    else
        pass "$name"
    fi
}

test_port_range() {
    local name="port-range"
    local dir="$work/$name"
    mkdir -p "$dir"

    # Both sides must agree that 1023 is below the shared 1024 floor.
    if "$CLIENT" 127.0.0.1 1023 "$dir/nope" >"$work/$name.client.log" 2>&1; then
        fail "$name (client accepted port 1023)"
    elif ! grep -q "1024 and 65535" "$work/$name.client.log"; then
        fail "$name (client failed for the wrong reason)"
    elif "$SERVER" 1023 "$dir" >"$work/$name.server.log" 2>&1; then
        fail "$name (server accepted port 1023)"
    elif ! grep -q "1024 and 65535" "$work/$name.server.log"; then
        fail "$name (server failed for the wrong reason)"
    else
        pass "$name"
    fi
}

test_idle_timeout() {
    local name="idle-timeout"
    local dir="$work/$name"
    mkdir -p "$dir"

    port="$(find_free_port)"
    if ! start_server "$dir" TIMEOUT_SEC=2; then
        fail "$name (server start)"
        stop_server
        return
    fi

    # Hold a raw connection open without sending: the server times out before
    # it learns a filename, so no file appears and the server stays up.
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    sleep 3
    if [ -z "$(ls -A "$dir")" ] && process_alive "$server_pid"; then
        pass "$name"
    else
        fail "$name (expected no file and a live server)"
    fi
    exec 3>&- 3<&- 2>/dev/null || true
    stop_server
}

echo "Filewire integration tests"

test_transfer 0
test_transfer 1
test_transfer 4096
test_transfer 4097
test_transfer 1048576
test_transfer 3000000
test_oversize
test_truncated_transfer
test_client_size_check
test_connection_refused
test_usage_errors
test_bad_directory
test_port_range

if [ "${RUN_SLOW:-0}" = "1" ]; then
    test_idle_timeout
fi

echo
echo "passed: $pass_count  failed: $fail_count"
[ "$fail_count" -eq 0 ]
