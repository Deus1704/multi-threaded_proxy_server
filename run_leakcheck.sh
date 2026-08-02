#!/usr/bin/env bash
# Leak check for the connection lifecycle.
#
# server_phase5 heap-allocates a Conn per accepted connection and stores the
# pointer in epoll_event.data.ptr, so every accept path must be matched by
# exactly one close_conn(). A missed delete leaks memory; a missed close leaks a
# file descriptor. Neither shows up in a throughput benchmark -- both show up
# here, as a line that keeps climbing across rounds.
set -u
PORT=${PORT:-18081}
ORIGIN_PORT=${ORIGIN_PORT:-19001}
ROUNDS=${ROUNDS:-6}

SRV=""; ORIGIN=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; [ -n "$ORIGIN" ] && kill "$ORIGIN" 2>/dev/null; }
trap cleanup EXIT
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

./origin_server -p "$ORIGIN_PORT" -w 8 -b 512 > /dev/null 2>&1 &
ORIGIN=$!
sleep 0.7
./server_phase5 "$PORT" --acl access_control.conf --log /dev/null --no-stats > /dev/null 2>&1 &
SRV=$!
sleep 0.8
kill -0 "$SRV" 2>/dev/null || { echo "proxy failed to start"; exit 1; }

fds()  { ls /proc/"$SRV"/fd 2>/dev/null | wc -l; }
rss()  { awk '/VmRSS/{print $2}' /proc/"$SRV"/status 2>/dev/null; }
thr()  { awk '/Threads/{print $2}' /proc/"$SRV"/status 2>/dev/null; }

printf '%-8s %-10s %-12s %-10s %s\n' round open_fds rss_kb threads note
printf '%-8s %-10s %-12s %-10s %s\n' start "$(fds)" "$(rss)" "$(thr)" "idle baseline"

for r in $(seq 1 "$ROUNDS"); do
  # Mix of connection-per-request and keep-alive, plus deliberately broken
  # requests, so the error/close paths get exercised too -- those are where a
  # missed cleanup usually hides.
  ./benchmark -p "$PORT" -H "127.0.0.1:$ORIGIN_PORT" -t 40 -n 150 -u 60 -i 1    > /dev/null 2>&1
  ./benchmark -p "$PORT" -H "127.0.0.1:$ORIGIN_PORT" -t 40 -n 400 -u 60 -i 1 -k > /dev/null 2>&1
  for _ in $(seq 1 40); do
    printf 'GET / HTTP/1.1\r\n\r\n'      | timeout 2 nc -q 0 127.0.0.1 "$PORT" > /dev/null 2>&1
    printf 'GARBAGE\r\n\r\n'             | timeout 2 nc -q 0 127.0.0.1 "$PORT" > /dev/null 2>&1
    printf 'GET /x HTTP/1.1\r\nHost: blocked.example.com\r\n\r\n' \
                                          | timeout 2 nc -q 0 127.0.0.1 "$PORT" > /dev/null 2>&1
    # half-open: send a partial request then vanish
    (printf 'GET /partial HTTP/1.1\r\nHost: 127.0.0.1'; sleep 0.05) \
                                          | timeout 2 nc -q 0 127.0.0.1 "$PORT" > /dev/null 2>&1
  done
  sleep 1
  printf '%-8s %-10s %-12s %-10s %s\n' "$r" "$(fds)" "$(rss)" "$(thr)" ""
done

echo ""
echo "Reading this: open_fds and threads must be FLAT across rounds. RSS may rise"
echo "early (the cache filling to its 2000-entry bound, allocator arenas warming)"
echo "and must then plateau. A steady climb in any column is a leak."
