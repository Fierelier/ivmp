#!/bin/sh
set -eu

port=$((42123 + ($$ % 1000)))
config=build/smoke-server.conf
log=build/smoke-server.log

cleanup()
{
	if [ "${server_pid:-}" ]; then
		kill "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	rm -f "$config"
}
trap cleanup EXIT INT TERM

mkdir -p build
printf 'bind_host=127.0.0.1\nbind_port=%s\nmax_players=4\n' "$port" >"$config"
bin/ivmp_server "$config" >"$log" 2>&1 &
server_pid=$!

attempt=0
while [ "$attempt" -lt 50 ]; do
	if build/test-client 127.0.0.1 "$port"; then
		echo "server smoke test passed"
		exit 0
	fi
	attempt=$((attempt + 1))
	sleep 0.05
done

cat "$log" >&2
echo "server smoke test failed" >&2
exit 1
