#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d /tmp/ring-functional.XXXXXX)"
BASE_PORT=$((20000 + ($$ % 20000)))
DRIVER_PIDS=()
RECV_FILES=()
FAILURES=0

cd "$ROOT" || exit 1

stop_drivers() {
    local pid

    for pid in "${DRIVER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${DRIVER_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    DRIVER_PIDS=()
}

cleanup() {
    local file

    stop_drivers
    for file in "${RECV_FILES[@]:-}"; do
        rm -f "$file"
    done
    rm -rf "$TMP"
}

trap cleanup EXIT

pass() {
    printf '[PASS] %s\n' "$1"
}

fail() {
    printf '[FAIL] %s\n' "$1"
    FAILURES=$((FAILURES + 1))
}

start_driver() {
    local machine_id="$1"
    shift

    ./bin/ring_driver "$machine_id" "$@" > "$TMP/driver_${machine_id}.log" 2>&1 &
    DRIVER_PIDS+=("$!")
}

assert_grep() {
    local pattern="$1"
    local file="$2"
    local label="$3"

    if grep -q "$pattern" "$file"; then
        pass "$label"
    else
        fail "$label"
        printf '  Missing pattern: %s\n  In file: %s\n' "$pattern" "$file"
    fi
}

run_build_test() {
    if make > "$TMP/make.log" 2>&1; then
        pass "compilation"
    else
        fail "compilation"
        sed -n '1,120p' "$TMP/make.log"
    fi
}

run_basic_ring_test() {
    local p1=$((BASE_PORT + 1))
    local p2=$((BASE_PORT + 2))

    start_driver 101 "$p1" 127.0.0.1 "$p2" 1
    start_driver 102 "$p2" 127.0.0.1 "$p1" 0
    sleep 0.5

    { sleep 1; printf '1\n102\nunicast-ok\n'; sleep 1; printf '2\nbroadcast-ok\n'; sleep 1; printf '3\n'; sleep 2; printf '8\n'; } |
        timeout 12s ./bin/ring_comm 101 > "$TMP/comm_101.log" 2>&1 &
    local c1=$!
    { sleep 5; printf '8\n'; } |
        timeout 12s ./bin/ring_comm 102 > "$TMP/comm_102.log" 2>&1 &
    local c2=$!

    wait "$c1"
    wait "$c2"
    stop_drivers

    assert_grep 'Message: unicast-ok' "$TMP/comm_102.log" "unicast 101 -> 102"
    assert_grep 'Message: broadcast-ok' "$TMP/comm_101.log" "broadcast revient a la source"
    assert_grep 'Message: broadcast-ok' "$TMP/comm_102.log" "broadcast recu par 102"
    assert_grep 'machine=101' "$TMP/comm_101.log" "recuperer contient 101"
    assert_grep 'machine=102' "$TMP/comm_101.log" "recuperer contient 102"
}

run_file_transfer_test() {
    local p1=$((BASE_PORT + 11))
    local p2=$((BASE_PORT + 12))
    local payload="$TMP/payload_240x2.bin"
    local recv="received/recv_$(basename "$payload")"

    RECV_FILES+=("$recv")
    dd if=/dev/urandom of="$payload" bs=240 count=2 status=none

    start_driver 201 "$p1" 127.0.0.1 "$p2" 1
    start_driver 202 "$p2" 127.0.0.1 "$p1" 0
    sleep 0.5

    { sleep 1; printf '4\n202\n%s\n' "$payload"; sleep 3; printf '8\n'; } |
        timeout 15s ./bin/ring_comm 201 > "$TMP/comm_201.log" 2>&1 &
    local c1=$!
    { sleep 6; printf '8\n'; } |
        timeout 15s ./bin/ring_comm 202 > "$TMP/comm_202.log" 2>&1 &
    local c2=$!

    wait "$c1"
    wait "$c2"
    stop_drivers

    assert_grep 'Transfert termine' "$TMP/comm_201.log" "transfert fichier termine cote emetteur"
    assert_grep 'Reception du fichier terminee' "$TMP/comm_202.log" "transfert fichier termine cote recepteur"

    if [ -f "$recv" ] && cmp -s "$payload" "$recv"; then
        pass "fichier binaire recu identique"
    else
        fail "fichier binaire recu identique"
    fi
}

run_join_test() {
    local p1=$((BASE_PORT + 21))
    local p2=$((BASE_PORT + 22))
    local p3=$((BASE_PORT + 23))

    start_driver 301 "$p1" 127.0.0.1 "$p2" 1
    start_driver 302 "$p2" 127.0.0.1 "$p1" 0
    start_driver 303 "$p3" - 0 0
    sleep 0.7

    { sleep 8; printf '8\n'; } |
        timeout 16s ./bin/ring_comm 301 > "$TMP/comm_301.log" 2>&1 &
    local c1=$!
    { sleep 10; printf '8\n'; } |
        timeout 16s ./bin/ring_comm 302 > "$TMP/comm_302.log" 2>&1 &
    local c2=$!
    { sleep 1; printf '5\n127.0.0.1\n%s\n' "$p1"; sleep 4; printf '1\n302\nfrom-303-after-join\n'; sleep 4; printf '8\n'; } |
        timeout 16s ./bin/ring_comm 303 > "$TMP/comm_303.log" 2>&1 &
    local c3=$!

    wait "$c1"
    wait "$c2"
    wait "$c3"
    stop_drivers

    assert_grep 'JOIN termine' "$TMP/comm_303.log" "join termine"
    assert_grep 'Message: from-303-after-join' "$TMP/comm_302.log" "message envoye apres join"
}

run_leave_test() {
    local p1=$((BASE_PORT + 31))
    local p2=$((BASE_PORT + 32))
    local p3=$((BASE_PORT + 33))

    start_driver 401 "$p1" 127.0.0.1 "$p2" 1
    start_driver 402 "$p2" 127.0.0.1 "$p3" 0
    start_driver 403 "$p3" 127.0.0.1 "$p1" 0
    sleep 0.7

    { sleep 10; printf '8\n'; } |
        timeout 18s ./bin/ring_comm 401 > "$TMP/comm_401.log" 2>&1 &
    local c1=$!
    { sleep 5; printf '1\n401\nafter-leave-402-to-401\n'; sleep 5; printf '8\n'; } |
        timeout 18s ./bin/ring_comm 402 > "$TMP/comm_402.log" 2>&1 &
    local c2=$!
    { sleep 1; printf '6\n'; sleep 7; printf '8\n'; } |
        timeout 18s ./bin/ring_comm 403 > "$TMP/comm_403.log" 2>&1 &
    local c3=$!

    wait "$c1"
    wait "$c2"
    wait "$c3"
    stop_drivers

    assert_grep 'Machine sortie de l anneau' "$TMP/comm_403.log" "leave confirme par la machine sortante"
    assert_grep 'Machine retiree de l anneau' "$TMP/comm_402.log" "voisin gauche reconfigure apres leave"
    assert_grep 'Message: after-leave-402-to-401' "$TMP/comm_401.log" "message envoye apres leave"
}

run_build_test
run_basic_ring_test
run_file_transfer_test
run_join_test
run_leave_test

printf '\nLogs temporaires: %s\n' "$TMP"

if [ "$FAILURES" -eq 0 ]; then
    printf 'Tous les tests fonctionnels sont passes.\n'
    exit 0
fi

printf '%d test(s) en echec.\n' "$FAILURES"
exit 1
