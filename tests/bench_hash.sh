#!/bin/bash
# JSON hash throughput bench for mktorrent.
# Expects (from test_framework or env): MKTORRENT, TEST_DATA_DIR, TEST_RESULTS_DIR
# Optional: BENCH_DROP_CACHES=1, PERFORMANCE_LARGE_TESTS=1, BENCH_JSONL=path

# Do not `set -e` here — this file is sourced into the test framework.

BENCH_JSONL="${BENCH_JSONL:-${TEST_RESULTS_DIR:-.}/bench.jsonl}"
PIECE_EXP="${BENCH_PIECE_EXP:-20}"

bench_nproc_capped() {
	local n
	n=$(nproc 2>/dev/null || echo 4)
	if [ "$n" -gt 8 ]; then
		n=8
	fi
	if [ "$n" -lt 1 ]; then
		n=1
	fi
	echo "$n"
}

bench_try_drop_caches() {
	# Returns 0 if cold cache was requested and succeeded; 1 if skipped.
	if [ "${BENCH_DROP_CACHES:-0}" != "1" ]; then
		return 1
	fi
	if [ ! -w /proc/sys/vm/drop_caches ]; then
		# Try sudo non-interactive
		if command -v sudo >/dev/null 2>&1; then
			if echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null 2>&1; then
				return 0
			fi
		fi
		return 1
	fi
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || return 1
	return 0
}

# Emit one JSONL record. cold: true | false | null (JSON null when unknown/skipped)
bench_one() {
	local file_mb="$1"
	local threads="$2"
	local mode="$3"   # buffered | direct
	local input="$4"
	local out_torrent="$5"
	local cold_flag="$6"  # true | false | null

	local extra=()
	case "$mode" in
		direct) extra+=(--direct-io) ;;
		iouring) extra+=(--io-uring) ;;
		iouring+direct) extra+=(--io-uring --direct-io) ;;
		iouring-d8) extra+=(--io-uring --io-uring-depth=8) ;;
		iouring-d32) extra+=(--io-uring --io-uring-depth=32) ;;
		iouring-d64) extra+=(--io-uring --io-uring-depth=64) ;;
		iouring-sqpoll) extra+=(--io-uring --io-uring-sqpoll) ;;
		sha1-openssl) extra+=(--sha1=openssl) ;;
		sha1-isal) extra+=(--sha1=isal) ;;
	esac

	local start end seconds mbps ok="true"
	start=$(date +%s.%N)
	if ! "$MKTORRENT" -q -f -l "$PIECE_EXP" -t "$threads" "${extra[@]}" \
		-o "$out_torrent" "$input" >/dev/null 2>&1; then
		ok="false"
		end=$(date +%s.%N)
		seconds=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.6f", e - s }')
		mbps="0"
	else
		end=$(date +%s.%N)
		seconds=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.6f", e - s }')
		mbps=$(awk -v mb="$file_mb" -v s="$seconds" 'BEGIN {
			if (s <= 0) print "0";
			else printf "%.2f", mb / s
		}')
	fi

	local cold_json
	if [ "$cold_flag" = "null" ]; then
		cold_json="null"
	else
		cold_json="$cold_flag"
	fi

	printf '{"fileMB":%s,"threads":%s,"mode":"%s","pieceExp":%s,"cold":%s,"seconds":%s,"MBps":%s,"ok":%s}\n' \
		"$file_mb" "$threads" "$mode" "$PIECE_EXP" "$cold_json" "$seconds" "$mbps" "$ok" \
		| tee -a "$BENCH_JSONL"
}

bench_pair_cold_warm() {
	local file_mb="$1"
	local threads="$2"
	local mode="$3"
	local input="$4"
	local tag="$5"

	local cold_label="null"
	if bench_try_drop_caches; then
		cold_label="true"
	fi

	bench_one "$file_mb" "$threads" "$mode" "$input" \
		"$TEST_RESULTS_DIR/bench_${tag}_${mode}_t${threads}_cold.torrent" "$cold_label"

	# Warm: immediate second pass (file should be in page cache when possible)
	bench_one "$file_mb" "$threads" "$mode" "$input" \
		"$TEST_RESULTS_DIR/bench_${tag}_${mode}_t${threads}_warm.torrent" "false"
}

run_hash_benches() {
	mkdir -p "$TEST_RESULTS_DIR"
	: > "$BENCH_JSONL"

	# Ensure fixtures exist (caller may have created them)
	[ -f "$TEST_DATA_DIR/perf_small.dat" ] || return 1
	[ -f "$TEST_DATA_DIR/perf_medium.dat" ] || return 1

	local threads_hi
	threads_hi=$(bench_nproc_capped)
	local help_txt
	help_txt=$("$MKTORRENT" -h 2>&1 || true)

	local has_threads=0
	if printf '%s\n' "$help_txt" | grep -q -- "-t,"; then
		has_threads=1
	fi

	# Help lists --io-uring only when USE_IO_URING; probe create to be sure.
	local has_iouring=0
	if printf '%s\n' "$help_txt" | grep -q -- "--io-uring"; then
		if "$MKTORRENT" -q -f -l "$PIECE_EXP" -t 1 --io-uring \
			-o "$TEST_RESULTS_DIR/bench_iouring_probe.torrent" \
			"$TEST_DATA_DIR/perf_small.dat" >/dev/null 2>&1; then
			has_iouring=1
		fi
	fi

	echo "Writing bench results to $BENCH_JSONL"

	local thread_list=(1)
	if [ "$has_threads" -eq 1 ] && [ "$threads_hi" -ne 1 ]; then
		thread_list+=( "$threads_hi" )
	fi

	local modes=(buffered direct)
	if [ "$has_iouring" -eq 1 ]; then
		modes+=(iouring iouring+direct)
		if [ "${BENCH_IOURING_KNOBS:-0}" = "1" ]; then
			modes+=(iouring-d8 iouring-d32 iouring-d64 iouring-sqpoll)
		fi
	fi

	local has_sha1_isal=0
	if printf '%s\n' "$help_txt" | grep -q -- "--sha1"; then
		modes+=(sha1-openssl)
		if ("$MKTORRENT" -V 2>&1 || true) | grep -q 'isal available'; then
			if "$MKTORRENT" -q -f -l "$PIECE_EXP" -t 1 --sha1=isal \
				-o "$TEST_RESULTS_DIR/bench_sha1_isal_probe.torrent" \
				"$TEST_DATA_DIR/perf_small.dat" >/dev/null 2>&1; then
				has_sha1_isal=1
				modes+=(sha1-isal)
			fi
		fi
	fi

	local t mode
	for t in "${thread_list[@]}"; do
		for mode in "${modes[@]}"; do
			bench_pair_cold_warm 10 "$t" "$mode" \
				"$TEST_DATA_DIR/perf_small.dat" "small"
			bench_pair_cold_warm 50 "$t" "$mode" \
				"$TEST_DATA_DIR/perf_medium.dat" "medium"
		done
	done

	if [ "${PERFORMANCE_LARGE_TESTS:-0}" = "1" ] && [ -f "$TEST_DATA_DIR/perf_large.dat" ]; then
		for t in "${thread_list[@]}"; do
			for mode in "${modes[@]}"; do
				bench_pair_cold_warm 200 "$t" "$mode" \
					"$TEST_DATA_DIR/perf_large.dat" "large"
			done
		done
	fi

	# Optional CPU-bound tmpfs protocol (compare --sha1=openssl vs --sha1=isal).
	if [ "${BENCH_SHA1_TMPFS:-0}" = "1" ] && [ "$has_sha1_isal" -eq 1 ]; then
		local tmpfs_dir="${BENCH_TMPFS_DIR:-/dev/shm}"
		local tmpfs_mb="${BENCH_TMPFS_MB:-2048}"
		local tmpfs_file="$tmpfs_dir/mktorrent_bench_sha1_${tmpfs_mb}m.dat"
		if [ -d "$tmpfs_dir" ] && [ -w "$tmpfs_dir" ]; then
			if [ ! -f "$tmpfs_file" ]; then
				dd if=/dev/urandom of="$tmpfs_file" bs=1M count="$tmpfs_mb" status=none \
					|| dd if=/dev/urandom of="$tmpfs_file" bs=1M count=512 status=none
			fi
			if [ -f "$tmpfs_file" ]; then
				local real_mb
				real_mb=$(du -m "$tmpfs_file" | awk '{print $1}')
				local nproc_n
				nproc_n=$(nproc 2>/dev/null || echo 4)
				local tt
				for tt in 1 2 4 "$nproc_n"; do
					bench_one "$real_mb" "$tt" sha1-openssl "$tmpfs_file" \
						"$TEST_RESULTS_DIR/bench_tmpfs_ossl_t${tt}.torrent" "false"
					bench_one "$real_mb" "$tt" sha1-isal "$tmpfs_file" \
						"$TEST_RESULTS_DIR/bench_tmpfs_isal_t${tt}.torrent" "false"
				done
			fi
		else
			echo "BENCH_SHA1_TMPFS=1 but $tmpfs_dir not writable; skipping tmpfs SHA-1 modes" >&2
		fi
	fi

	echo "Bench JSONL written ($BENCH_JSONL):"
	wc -l < "$BENCH_JSONL" | awk '{print $1 " records"}'
}

# Allow: bash tests/bench_hash.sh (after sourcing framework) or source + run_hash_benches
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	echo "Source this script from test_performance.sh, or call run_hash_benches after setting env." >&2
	exit 1
fi
