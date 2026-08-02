#!/usr/bin/env bash
# Stand-alone 10k-concurrent-connection test.
#
# Split out from run_benchmarks.sh because it has a very different cost profile:
# binding an explicit source address costs ~20ms per socket on this kernel, so
# the ramp to 10k connections takes minutes. That cost is the load generator's,
# not the proxy's -- see RESULTS.md.
set -u
PORT=${PORT:-18080}
ORIGIN_PORT=${ORIGIN_PORT:-19000}
TARGET=${TARGET:-10000}
SRC_IPS=${SRC_IPS:-8}

SRV=""; ORIGIN=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; [ -n "$ORIGIN" ] && kill "$ORIGIN" 2>/dev/null; }
trap cleanup EXIT

ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

echo "fd limit soft : $(ulimit -n)"
echo "port range    : $(tr '\t' '-' < /proc/sys/net/ipv4/ip_local_port_range)"
echo "TIME_WAIT now : $(($(ss -tan state time-wait 2>/dev/null | wc -l) - 1))"
echo ""

./origin_server -p "$ORIGIN_PORT" -w 8 -b 512 > origin_conc.log 2>&1 &
ORIGIN=$!
sleep 0.7
./server_phase5 "$PORT" --log access_conc.log > server_conc.log 2>&1 &
SRV=$!
sleep 0.8
kill -0 "$SRV" 2>/dev/null || { echo "proxy failed to start:"; cat server_conc.log; exit 1; }

echo "proxy startup:"
sed 's/^/  /' server_conc.log
echo ""

./concurrency_test -p "$PORT" -H "127.0.0.1:$ORIGIN_PORT" -n "$TARGET" -i "$SRC_IPS" -s 3
RC=$?

sleep 5.5
echo ""
echo "proxy's own counters:"
grep 'peak=' server_conc.log | tail -1 | sed 's/^/  /'
echo ""
echo "access log lines written during the test: $(wc -l < access_conc.log 2>/dev/null || echo 0)"
exit $RC
