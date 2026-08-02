#!/usr/bin/env bash
# Reproducible benchmark harness. Every number in RESULTS.md comes from this
# script, so any claim about the proxy can be re-derived by running it.
#
#   ./run_benchmarks.sh            # full run
#   ./run_benchmarks.sh smoke      # correctness checks only
set -u

PORT=${PORT:-18080}
ORIGIN_PORT=${ORIGIN_PORT:-19000}
MODE=${1:-full}
OUT=RESULTS_raw.txt

SRV_PID=""
ORIGIN_PID=""

cleanup() {
  [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
  [ -n "$ORIGIN_PID" ] && kill "$ORIGIN_PID" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

log() { echo "$@" | tee -a "$OUT"; }

# Raising our own soft fd limit: the load generators need ~10k descriptors and
# the default soft limit is usually lower. The hard limit is what actually
# constrains us and it does not need root.
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

: > "$OUT"
log "==============================================================="
log " proxy benchmark run: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
log "==============================================================="
log "kernel        : $(uname -srm)"
log "cpu           : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //') ($(nproc) cores)"
log "memory        : $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
log "fd limit      : soft=$(ulimit -n) hard=$(ulimit -Hn)"
log "ports         : $(cat /proc/sys/net/ipv4/ip_local_port_range | tr '\t' '-')"
log "somaxconn     : $(cat /proc/sys/net/core/somaxconn)"
log "compiler      : $(g++ --version | head -1)"
log ""
log "NOTE: client and server share one host, so the load generator competes"
log "      with the proxy for the same cores. These are lower bounds."
log ""

need() { [ -x "./$1" ] || { echo "missing ./$1 -- run: make all"; exit 1; }; }
for b in server_phase5 origin_server benchmark concurrency_test test_http test_lru_cache; do need "$b"; done

port_free() { ! ss -ltn "sport = :$1" | tail -n +2 | grep -q .; }
for p in $PORT $ORIGIN_PORT; do
  port_free "$p" || { echo "port $p already in use -- pick another with PORT=/ORIGIN_PORT="; exit 1; }
done

start_stack() {
  local extra="$*"
  ./origin_server -p "$ORIGIN_PORT" -w 8 -b 512 > origin.log 2>&1 &
  ORIGIN_PID=$!
  sleep 0.7
  kill -0 "$ORIGIN_PID" 2>/dev/null || { echo "origin failed to start:"; cat origin.log; exit 1; }

  # shellcheck disable=SC2086
  ./server_phase5 "$PORT" --acl access_control.conf --log access.log $extra > server.log 2>&1 &
  SRV_PID=$!
  sleep 0.7
  kill -0 "$SRV_PID" 2>/dev/null || { echo "proxy failed to start:"; cat server.log; exit 1; }
}

stop_stack() {
  [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; SRV_PID=""
  [ -n "$ORIGIN_PID" ] && kill "$ORIGIN_PID" 2>/dev/null; ORIGIN_PID=""
  sleep 0.4
}

# hit rate as reported by the proxy itself
cache_line() { grep -o 'cache=[0-9.]*%[^|]*' server.log | tail -1; }
last_stats() { grep '\[stats\]' server.log | tail -1; }

req() { # req <host-header> <path> [extra-header]
  printf 'GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s\r\n' "$2" "$1" "${3:-}" \
    | timeout 6 nc -q 1 127.0.0.1 "$PORT"
}

# ---------------------------------------------------------------------------
log "###############  1. UNIT TESTS  ###############"
./test_http 2>&1 | tail -3 | tee -a "$OUT"
log ""
./test_lru_cache 2>&1 | tee -a "$OUT"
log ""

# ---------------------------------------------------------------------------
log "###############  2. CORRECTNESS SMOKE  ###############"
rm -f access.log
start_stack --log-immediate

pass=0; fail=0
check() { # check <description> <expected-substring> <actual>
  if echo "$3" | grep -q "$2"; then log "  PASS  $1"; pass=$((pass+1));
  else log "  FAIL  $1 (expected '$2', got: $(echo "$3" | head -1))"; fail=$((fail+1)); fi
}

check "forwards to origin via Host header"   "200 OK"    "$(req "127.0.0.1:$ORIGIN_PORT" /resource/1)"
check "second identical request served"      "200 OK"    "$(req "127.0.0.1:$ORIGIN_PORT" /resource/1)"
check "named route origin.test -> upstream"  "200 OK"    "$(req "origin.test" /resource/2)"
check "ACL denies blocked host"              "403"       "$(req "blocked.example.com" /x)"
check "ACL denies wildcard subdomain"        "403"       "$(req "ads.doubleclick.net" /x)"
check "ACL denies /admin path"               "403"       "$(req "127.0.0.1:$ORIGIN_PORT" /admin/users)"
check "unroutable host -> 502"               "502"       "$(req "no.such.host.invalid" /x)"
check "missing Host -> 400"                  "400"       "$(printf 'GET / HTTP/1.1\r\n\r\n' | timeout 6 nc -q 1 127.0.0.1 $PORT)"
check "garbage request line -> 400"          "400"       "$(printf 'NOT-HTTP\r\n\r\n' | timeout 6 nc -q 1 127.0.0.1 $PORT)"

log ""
log "  -- access log: every request above must appear --"
sleep 0.5
log "  lines logged: $(wc -l < access.log)"
log "  cache states: $(grep -o 'cache=[A-Z]*' access.log | sort | uniq -c | tr '\n' ' ')"
log "  acl states  : $(grep -o 'acl=[A-Z]*' access.log | sort | uniq -c | tr '\n' ' ')"
log "  statuses    : $(grep -o 'status=[0-9]*' access.log | sort | uniq -c | tr '\n' ' ')"
log ""
log "  sample lines:"
head -4 access.log | sed 's/^/    /' | tee -a "$OUT"
log "  a HIT line (proves cache state is recorded per request):"
grep -m1 'cache=HIT' access.log | sed 's/^/    /' | tee -a "$OUT"
log "  a DENY line (proves the matched rule is recorded):"
grep -m1 'acl=DENY' access.log | sed 's/^/    /' | tee -a "$OUT"
log ""
log "  smoke result: $pass passed, $fail failed"
stop_stack
log ""

if [ "$MODE" = "smoke" ]; then log "(smoke mode: stopping here)"; exit $((fail > 0)); fi

# ---------------------------------------------------------------------------
log "###############  3. THROUGHPUT  ###############"
log "Working set 100 URLs vs 2000-entry cache => cache-hit-dominated,"
log "which is the path a caching proxy is supposed to be fast on."
log ""

run_bench() { # run_bench <label> <args...>
  local label="$1"; shift
  rm -f access.log server.log
  start_stack --no-stats
  local out
  out=$(./benchmark -p "$PORT" -H "127.0.0.1:$ORIGIN_PORT" "$@" 2>&1)
  local rps lat p99 okc failc
  rps=$(echo "$out" | awk '/THROUGHPUT/{print $3}')
  # "latency ms : mean X p50 X p95 X p99 X max X" -> mean=$5, p99=$11
  lat=$(echo "$out" | awk '/latency ms/{print $5}')
  p99=$(echo "$out"  | awk '/latency ms/{print $11}')
  okc=$(echo "$out" | awk '/^requests/{print $3}')
  failc=$(echo "$out" | awk '/^requests/{print $5}')
  sleep 0.3
  local hr
  hr=$(grep -c 'cache=HIT' access.log 2>/dev/null || echo 0)
  local tot
  tot=$(wc -l < access.log 2>/dev/null || echo 1)
  local pct="n/a"
  [ "$tot" -gt 0 ] && pct=$(awk "BEGIN{printf \"%.1f\", $hr*100/$tot}")
  printf "  %-34s %10s req/s  mean %7s ms  p99 %7s ms  ok %8s fail %6s  hit %5s%%\n" \
    "$label" "$rps" "$lat" "$p99" "$okc" "$failc" "$pct" | tee -a "$OUT"
  stop_stack
}

log "  All runs use -i 1 (no explicit client source-address bind). Measured on"
log "  this kernel, bind()ing a source address before connect() costs ~20ms and"
log "  serialises across threads, which pins the CLIENT at ~94 req/s and tells"
log "  you nothing about the server. -i >1 is only for the concurrency test,"
log "  where the ephemeral port range genuinely is the limit."
log ""
log "  connection-per-request (a new TCP handshake for every request):"
run_bench "50 threads"   -t 50  -n 200 -u 100 -i 1
run_bench "100 threads"  -t 100 -n 200 -u 100 -i 1
run_bench "200 threads"  -t 200 -n 150 -u 100 -i 1
log ""
log "  keep-alive (connections reused, which is what real clients do):"
run_bench "50 threads,  keep-alive"  -t 50  -n 2000 -u 100 -i 1 -k
run_bench "100 threads, keep-alive"  -t 100 -n 2000 -u 100 -i 1 -k
run_bench "200 threads, keep-alive"  -t 200 -n 1000 -u 100 -i 1 -k
run_bench "400 threads, keep-alive"  -t 400 -n 500  -u 100 -i 1 -k
log ""

# ---------------------------------------------------------------------------
log "###############  4. CACHE HIT RATE, END TO END  ###############"
log "Measured at the proxy under a Zipf request stream. C = cache capacity,"
log "N = working set. The ratio C/N and the skew alpha are what set the"
log "achievable hit rate -- no amount of cache engineering beats the math."
log ""
printf "  %-6s %-6s %-7s %-8s %-11s %-11s %s\n" "N" "C" "C/N" "alpha" "hit rate" "upstream" "requests" | tee -a "$OUT"

hitrate_case() { # hitrate_case <N> <C> <alpha>
  local N=$1 C=$2 A=$3
  rm -f access.log server.log
  start_stack --no-stats --cache "$C"
  ./benchmark -p "$PORT" -H "127.0.0.1:$ORIGIN_PORT" -t 50 -n 800 -u "$N" -a "$A" -i 1 -k \
    > /dev/null 2>&1
  sleep 0.4
  local hits misses total
  hits=$(grep -c 'cache=HIT' access.log 2>/dev/null || echo 0)
  misses=$(grep -c 'cache=MISS' access.log 2>/dev/null || echo 0)
  total=$((hits + misses))
  local pct="n/a"
  [ "$total" -gt 0 ] && pct=$(awk "BEGIN{printf \"%.2f\", $hits*100/$total}")
  printf "  %-6s %-6s %-7s %-8s %-11s %-11s %s\n" "$N" "$C" \
    "$(awk "BEGIN{printf \"%.2f\", $C/$N}")" "$A" "$pct%" "$misses" "$total" | tee -a "$OUT"
  stop_stack
}

hitrate_case 1000 100  1.0
hitrate_case 1000 500  1.0
hitrate_case 1000 1000 1.0
hitrate_case 2000 1000 1.0
hitrate_case 2000 2000 1.0
hitrate_case 5000 2000 1.0
hitrate_case 5000 2000 1.2
hitrate_case 1000 100  0.0
log ""

# ---------------------------------------------------------------------------
log "###############  5. CONCURRENT CONNECTIONS  ###############"
log "Run separately: ./run_concurrency.sh"
log "Kept out of this script because the ramp to 10k connections takes minutes"
log "(the client's per-socket source-address bind costs ~20ms), which would"
log "dominate the runtime of every other section."
log ""

log "==============================================================="
log " done -- raw output in $OUT"
log "==============================================================="
