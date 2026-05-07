#!/bin/sh

set -u

usage() {
	cat <<'EOF'
Usage: collect-btop-diagnostics.sh [PID]

Collect diagnostics from a hung btop process on macOS or NetBSD and package
them into a compressed tarball. If PID is omitted, the script uses pgrep to
find a running btop process.

Run this before killing the hung process. Some commands may require sudo for
full stack traces or process details.
EOF
}

log() {
	printf '%s\n' "$*" >&2
}

have_cmd() {
	command -v "$1" >/dev/null 2>&1
}

run_capture() {
	label=$1
	outfile=$2
	shift 2
	{
		printf '$'
		for arg in "$@"; do
			printf ' %s' "$arg"
		done
		printf '\n\n'
		"$@"
		status=$?
		printf '\n[exit status: %s]\n' "$status"
	} >"$outfile" 2>&1
}

copy_if_exists() {
	src=$1
	dst=$2
	if [ -f "$src" ]; then
		cp "$src" "$dst"
	fi
}

find_btop_pid() {
	if have_cmd pgrep; then
		pgrep -x btop 2>/dev/null | head -n 1
	fi
}

pid=${1:-}
if [ "${pid:-}" = "-h" ] || [ "${pid:-}" = "--help" ]; then
	usage
	exit 0
fi

if [ -z "${pid:-}" ]; then
	pid=$(find_btop_pid || true)
fi

if [ -z "${pid:-}" ]; then
	log "Could not find a running btop process. Pass the PID explicitly."
	exit 1
fi

case $pid in
	*[!0-9]*|'')
		log "Invalid PID: $pid"
		exit 1
		;;
esac

if ! kill -0 "$pid" 2>/dev/null; then
	log "PID $pid is not accessible. If it is running as another user, rerun with sudo."
	exit 1
fi

os=$(uname -s)
case $os in
	Darwin|NetBSD) ;;
	*)
		log "Unsupported OS: $os (expected Darwin or NetBSD)"
		exit 1
		;;
esac

stamp=$(date '+%Y%m%d-%H%M%S')
host=$(hostname 2>/dev/null || printf unknown)
out_base="btop-diagnostics-${os}-${host}-pid${pid}-${stamp}"
out_dir="${TMPDIR:-/tmp}/${out_base}"
tarball="${PWD}/${out_base}.tar.gz"

mkdir -p "$out_dir" || exit 1

log "Collecting btop diagnostics for PID $pid into $out_dir"

{
	printf 'timestamp=%s\n' "$stamp"
	printf 'host=%s\n' "$host"
	printf 'os=%s\n' "$os"
	printf 'pid=%s\n' "$pid"
	printf 'user=%s\n' "$(id -un 2>/dev/null || printf unknown)"
	printf 'uname='; uname -a 2>/dev/null || true
	printf 'pwd=%s\n' "$PWD"
} >"$out_dir/metadata.txt"

copy_if_exists "$HOME/.local/state/btop.log" "$out_dir/btop.log"
copy_if_exists "$HOME/.config/btop/btop.conf" "$out_dir/btop.conf"

run_capture ps_basic "$out_dir/ps.txt" ps -p "$pid" -o pid,ppid,user,stat,time,command

if have_cmd lsof; then
	run_capture lsof "$out_dir/lsof.txt" lsof -p "$pid"
fi

case $os in
	Darwin)
		if have_cmd sample; then
			run_capture sample "$out_dir/sample.txt" sample "$pid" 5
		else
			log "sample not found; skipping macOS stack sample"
		fi

		if have_cmd lldb; then
			run_capture lldb_bt_all "$out_dir/lldb-bt-all.txt" lldb --batch -p "$pid" -o "bt all" -o "quit"
		fi

		if have_cmd ps; then
			run_capture ps_threads "$out_dir/ps-threads.txt" ps -M -p "$pid"
		fi

		if have_cmd log; then
			run_capture unified_btop "$out_dir/unified-btop-last1h.txt" log show --predicate 'process == "btop"' --last 1h
			run_capture unified_kernel_btop "$out_dir/unified-kernel-btop-last1h.txt" log show --predicate 'sender == "kernel" AND composedMessage CONTAINS "btop"' --last 1h
		fi
		;;

	NetBSD)
		if have_cmd gdb; then
			run_capture gdb_bt_all "$out_dir/gdb-bt-all.txt" gdb -batch -ex "thread apply all bt" -p "$pid"
		else
			log "gdb not found; skipping NetBSD stack trace"
		fi

		if have_cmd ps; then
			run_capture ps_threads "$out_dir/ps-threads.txt" ps -lLp "$pid"
		fi

		if have_cmd procstat; then
			run_capture procstat_threads "$out_dir/procstat-threads.txt" procstat -t "$pid"
		fi

		if have_cmd fstat; then
			run_capture fstat "$out_dir/fstat.txt" fstat -p "$pid"
		fi

		if [ -r "/proc/$pid/status" ]; then
			cp "/proc/$pid/status" "$out_dir/proc-status.txt"
		fi

		if have_cmd dmesg; then
			run_capture dmesg_tail "$out_dir/dmesg-tail.txt" sh -c 'dmesg | tail -100'
		fi
		;;
esac

if have_cmd tar; then
	(
		cd "${out_dir%/*}" && tar -czf "$tarball" "${out_dir##*/}"
	) || exit 1
else
	log "tar not found; diagnostics left in $out_dir"
	exit 1
fi

log "Wrote $tarball"
log "You can now kill PID $pid if needed."
printf '%s\n' "$tarball"
