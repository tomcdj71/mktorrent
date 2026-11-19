#!/bin/bash
# Performance / throughput benches for mktorrent (opt-in).
# Set PERFORMANCE_TESTS=1 to run. Optional:
#   PERFORMANCE_LARGE_TESTS=1  — include 200MB fixture
#   BENCH_DROP_CACHES=1        — drop page cache before cold runs (needs root/sudo -n)

if [ "${PERFORMANCE_TESTS:-0}" != "1" ]; then
    echo "Skipping performance tests. Set PERFORMANCE_TESTS=1 to run them."
    return 0 2>/dev/null || true
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bench_hash.sh
source "$SCRIPT_DIR/bench_hash.sh"

echo "Creating larger test files for performance testing..."
create_test_file "perf_small.dat" 10240    # 10MB
create_test_file "perf_medium.dat" 51200   # 50MB

if [ "${PERFORMANCE_LARGE_TESTS:-0}" = "1" ]; then
    create_test_file "perf_large.dat" 204800  # 200MB
fi

echo "Running JSON hash throughput benches (buffered/direct × threads × cold/warm)..."
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_hash_benches \
    && [ -f "$BENCH_JSONL" ] \
    && grep -q '"mode":"buffered"' "$BENCH_JSONL" \
    && grep -q '"mode":"direct"' "$BENCH_JSONL"; then
    echo -e "${GREEN}Test passed: bench JSONL has buffered and direct records${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: run_hash_benches / bench JSONL${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

# Light piece-length smoke (not timed into JSONL)
echo "Testing different piece lengths with medium file"
run_test "Piece length 16 (64KB)" \
    "$MKTORRENT -q -f -l 16 -o $TEST_RESULTS_DIR/perf_medium_l16.torrent $TEST_DATA_DIR/perf_medium.dat"

run_test "Piece length 18 (256KB)" \
    "$MKTORRENT -q -f -l 18 -o $TEST_RESULTS_DIR/perf_medium_l18.torrent $TEST_DATA_DIR/perf_medium.dat"

run_test "Piece length 20 (1MB)" \
    "$MKTORRENT -q -f -l 20 -o $TEST_RESULTS_DIR/perf_medium_l20.torrent $TEST_DATA_DIR/perf_medium.dat"

run_test "Piece length 22 (4MB)" \
    "$MKTORRENT -q -f -l 22 -o $TEST_RESULTS_DIR/perf_medium_l22.torrent $TEST_DATA_DIR/perf_medium.dat"
