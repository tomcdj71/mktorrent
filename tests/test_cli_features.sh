#!/bin/bash
# Tests for JSON summary, exit codes, multi-target, from-file, quiet, symlinks

create_test_file "cli_a.dat" 64
create_test_file "cli_b.dat" 32

echo "Testing JSON summary fields"
JSON_OUT=$("$MKTORRENT" --json -l 20 -o "$TEST_RESULTS_DIR/cli_json.torrent" \
    "$TEST_DATA_DIR/cli_a.dat")
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if echo "$JSON_OUT" | jq -e '.ok == true and (.infoHash|type=="string") and .pieceLength == 1048576 and (.announce|type=="array")' >/dev/null; then
    echo -e "${GREEN}Test passed: JSON summary fields${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: JSON summary fields${NC}"
    echo "$JSON_OUT"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing DHT-only (no announce)"
run_test "DHT-only create" \
    "$MKTORRENT -q -o $TEST_RESULTS_DIR/cli_dht.torrent $TEST_DATA_DIR/cli_a.dat"

echo "Testing multi-target requires -n"
run_test "Multi-target without name fails" \
    "$MKTORRENT -q -o $TEST_RESULTS_DIR/cli_multi_fail.torrent $TEST_DATA_DIR/cli_a.dat $TEST_DATA_DIR/cli_b.dat" \
    1

echo "Testing multi-target with -n"
run_test "Multi-target create" \
    "$MKTORRENT -q -n cli-multi -o $TEST_RESULTS_DIR/cli_multi.torrent $TEST_DATA_DIR/cli_a.dat $TEST_DATA_DIR/cli_b.dat"
validate_torrent "$TEST_RESULTS_DIR/cli_multi.torrent" 0 2

echo "Testing --from-file"
printf '%s\n# comment\n%s\n' \
    "$TEST_DATA_DIR/cli_a.dat" \
    "$TEST_DATA_DIR/cli_b.dat" > "$TEST_RESULTS_DIR/cli_list.txt"
run_test "From-file create" \
    "$MKTORRENT -q -n cli-from -o $TEST_RESULTS_DIR/cli_from.torrent --from-file $TEST_RESULTS_DIR/cli_list.txt"
validate_torrent "$TEST_RESULTS_DIR/cli_from.torrent" 0 2

echo "Testing quiet keeps stdout empty"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
OUT=$("$MKTORRENT" -q -f -o "$TEST_RESULTS_DIR/cli_quiet.torrent" "$TEST_DATA_DIR/cli_a.dat" 2>/dev/null)
if [ -z "$OUT" ]; then
    echo -e "${GREEN}Test passed: quiet stdout empty${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: quiet stdout not empty: $OUT${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing usage exit code"
run_test "Usage exit code" "$MKTORRENT" 1

echo "Testing -h/--help clean exit"
run_test "Help exit code" "$MKTORRENT -h" 0

echo "Testing --no-follow-symlinks skips symlink-only target"
ln -sfn "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_link.dat"
run_test "No-follow-symlinks with file sibling" \
    "$MKTORRENT -q -n cli-sym -o $TEST_RESULTS_DIR/cli_sym.torrent --no-follow-symlinks $TEST_DATA_DIR/cli_link.dat $TEST_DATA_DIR/cli_b.dat"

echo "Testing JSON error object on failure"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
JSON_ERR=$("$MKTORRENT" --json /no/such/cli_missing.dat 2>/dev/null || true)
if echo "$JSON_ERR" | jq -e '.ok == false and (.code|type=="number") and (.error|type=="string")' >/dev/null; then
    echo -e "${GREEN}Test passed: JSON error object${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: JSON error object${NC}"
    echo "$JSON_ERR"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing --stdin path list"
run_test "Stdin create" \
    "printf '%s\\n%s\\n' '$TEST_DATA_DIR/cli_a.dat' '$TEST_DATA_DIR/cli_b.dat' | '$MKTORRENT' -q -n cli-stdin -o '$TEST_RESULTS_DIR/cli_stdin.torrent' --stdin"
validate_torrent "$TEST_RESULTS_DIR/cli_stdin.torrent" 0 2

echo "Testing -0/--null path list"
run_test "NUL stdin create" \
    "printf '%s\\0%s\\0' '$TEST_DATA_DIR/cli_a.dat' '$TEST_DATA_DIR/cli_b.dat' | '$MKTORRENT' -q -n cli-nul -o '$TEST_RESULTS_DIR/cli_nul.torrent' -0 --stdin"
validate_torrent "$TEST_RESULTS_DIR/cli_nul.torrent" 0 2

echo "Testing relative --exclude"
mkdir -p "$TEST_DATA_DIR/cli_tree/subdir"
cp "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_tree/keep.dat"
cp "$TEST_DATA_DIR/cli_b.dat" "$TEST_DATA_DIR/cli_tree/subdir/drop.tmp"
cp "$TEST_DATA_DIR/cli_b.dat" "$TEST_DATA_DIR/cli_tree/subdir/keep2.dat"
run_test "Relative exclude create" \
    "$MKTORRENT -q -e 'subdir/*.tmp' -n cli-ex -o $TEST_RESULTS_DIR/cli_ex.torrent $TEST_DATA_DIR/cli_tree"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if ! grep -aq 'drop.tmp' "$TEST_RESULTS_DIR/cli_ex.torrent" \
    && grep -aq 'keep.dat' "$TEST_RESULTS_DIR/cli_ex.torrent" \
    && grep -aq 'keep2.dat' "$TEST_RESULTS_DIR/cli_ex.torrent"; then
    echo -e "${GREEN}Test passed: relative exclude skips matching path${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: relative exclude${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing --io-uring (parity with buffered when built with liburing)"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
# Help lists --io-uring only when USE_IO_URING; also probe create (CI matrix may lack liburing).
HAS_IOURING=0
if ("$MKTORRENT" -h 2>&1 || true) | grep -q -- '--io-uring'; then
    if "$MKTORRENT" -q -f -d -l 16 --io-uring \
        -o "$TEST_RESULTS_DIR/cli_iou_probe.torrent" \
        "$TEST_DATA_DIR/cli_a.dat" >/dev/null 2>&1; then
        HAS_IOURING=1
    fi
fi
if [ "$HAS_IOURING" -eq 1 ]; then
    H_BUF=$("$MKTORRENT" --json -q -f -d -l 16 -o "$TEST_RESULTS_DIR/cli_iou_buf.torrent" \
        "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
    H_IOU=$("$MKTORRENT" --json -q -f -d -l 16 --io-uring -o "$TEST_RESULTS_DIR/cli_iou.torrent" \
        "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
    if [ -n "$H_BUF" ] && [ "$H_BUF" = "$H_IOU" ]; then
        echo -e "${GREEN}Test passed: --io-uring infohash matches buffered${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        echo -e "${RED}Test failed: --io-uring infohash mismatch ($H_BUF vs $H_IOU)${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
else
    echo -e "${GREEN}Test skipped: --io-uring not usable in this build${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
fi
echo

echo "Testing -t thread bounds"
run_test "Threads -1 rejected" \
    "$MKTORRENT -q -t -1 -o $TEST_RESULTS_DIR/cli_tneg.torrent $TEST_DATA_DIR/cli_a.dat" 1
run_test "Threads 0 rejected" \
    "$MKTORRENT -q -t 0 -o $TEST_RESULTS_DIR/cli_t0.torrent $TEST_DATA_DIR/cli_a.dat" 1
run_test "Threads 65 rejected" \
    "$MKTORRENT -q -t 65 -o $TEST_RESULTS_DIR/cli_t65.torrent $TEST_DATA_DIR/cli_a.dat" 1
run_test "Threads non-numeric rejected" \
    "$MKTORRENT -q -t abc -o $TEST_RESULTS_DIR/cli_tabc.torrent $TEST_DATA_DIR/cli_a.dat" 1

echo "Testing --sha1 backend selection / infohash parity"
HAS_SHA1_OPT=0
if ("$MKTORRENT" -h 2>&1 || true) | grep -q -- '--sha1'; then
    HAS_SHA1_OPT=1
fi
if [ "$HAS_SHA1_OPT" -eq 1 ]; then
    run_test "sha1 invalid value rejected" \
        "$MKTORRENT -q --sha1=bogus -o $TEST_RESULTS_DIR/cli_sha1_bad.torrent $TEST_DATA_DIR/cli_a.dat" 1

    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    H_AUTO=$("$MKTORRENT" --json -q -f -d -l 16 --sha1=auto \
        -o "$TEST_RESULTS_DIR/cli_sha1_auto.torrent" "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
    H_OSSL=$("$MKTORRENT" --json -q -f -d -l 16 --sha1=openssl \
        -o "$TEST_RESULTS_DIR/cli_sha1_ossl.torrent" "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)

    if [ -z "$H_AUTO" ] || [ -z "$H_OSSL" ] || [ "$H_AUTO" != "$H_OSSL" ]; then
        echo -e "${RED}Test failed: --sha1 auto/openssl infohash mismatch ($H_AUTO vs $H_OSSL)${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    elif ("$MKTORRENT" -V 2>&1 || true) | grep -q 'isal available'; then
        H_ISAL=$("$MKTORRENT" --json -q -f -d -l 16 --sha1=isal \
            -o "$TEST_RESULTS_DIR/cli_sha1_isal.torrent" "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
        # Larger fixture exercises multi-buffer waves (up to 16×1MiB pieces).
        dd if=/dev/urandom of="$TEST_DATA_DIR/cli_sha1_batch.dat" bs=1M count=20 status=none
        H_OSSL_B=$("$MKTORRENT" --json -q -f -d -l 20 -t 2 --sha1=openssl \
            -o "$TEST_RESULTS_DIR/cli_sha1_ossl_b.torrent" \
            "$TEST_DATA_DIR/cli_sha1_batch.dat" | jq -r .infoHash)
        H_ISAL_B=$("$MKTORRENT" --json -q -f -d -l 20 -t 2 --sha1=isal \
            -o "$TEST_RESULTS_DIR/cli_sha1_isal_b.torrent" \
            "$TEST_DATA_DIR/cli_sha1_batch.dat" | jq -r .infoHash)
        if [ -n "$H_ISAL" ] && [ "$H_ISAL" = "$H_OSSL" ] \
                && [ -n "$H_ISAL_B" ] && [ "$H_ISAL_B" = "$H_OSSL_B" ]; then
            echo -e "${GREEN}Test passed: --sha1 openssl/isal/auto infohash parity (incl. batch)${NC}"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            echo -e "${RED}Test failed: --sha1 isal infohash mismatch ($H_OSSL vs $H_ISAL; batch $H_OSSL_B vs $H_ISAL_B)${NC}"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        run_test "sha1=isal without USE_ISAL fails" \
            "$MKTORRENT -q --sha1=isal -o $TEST_RESULTS_DIR/cli_sha1_noisal.torrent $TEST_DATA_DIR/cli_a.dat" 1
        echo -e "${GREEN}Test passed: --sha1 auto/openssl parity (no ISA-L in build)${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
    fi
else
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo -e "${GREEN}Test skipped: --sha1 not in this build (no pthreads)${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
fi
echo

echo "Testing -f truncates longer existing output"
dd if=/dev/zero of="$TEST_RESULTS_DIR/cli_trunc.torrent" bs=1024 count=64 status=none
OLD_SZ=$(wc -c < "$TEST_RESULTS_DIR/cli_trunc.torrent")
run_test "Force overwrite create" \
    "$MKTORRENT -q -f -d -l 16 -o $TEST_RESULTS_DIR/cli_trunc.torrent $TEST_DATA_DIR/cli_a.dat"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
NEW_SZ=$(wc -c < "$TEST_RESULTS_DIR/cli_trunc.torrent")
if [ "$NEW_SZ" -lt "$OLD_SZ" ] && [ "$NEW_SZ" -gt 0 ]; then
    echo -e "${GREEN}Test passed: -f truncated longer output ($OLD_SZ -> $NEW_SZ)${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: -f did not truncate ($OLD_SZ -> $NEW_SZ)${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing --max-files rejects oversized trees"
run_test "Max-files 1 with two targets fails" \
    "$MKTORRENT -q --max-files 1 -n mf -o $TEST_RESULTS_DIR/cli_mf.torrent $TEST_DATA_DIR/cli_a.dat $TEST_DATA_DIR/cli_b.dat" 1

echo "Testing overlong --from-file line rejected"
python3 - <<PY
path = "$TEST_RESULTS_DIR/cli_longline.txt"
# PATH_MAX-sized line without newline → silent truncate historically; must fail now
open(path, "w").write("a" * 5000)
PY
run_test "Overlong from-file line fails" \
    "$MKTORRENT -q -n long -o $TEST_RESULTS_DIR/cli_long.torrent --from-file $TEST_RESULTS_DIR/cli_longline.txt" 1

echo "Testing comment control character rejected"
run_test "Comment with NUL-ish control fails" \
    "$MKTORRENT -q -c \$'bad\tline' -o $TEST_RESULTS_DIR/cli_cmt.torrent $TEST_DATA_DIR/cli_a.dat" 1

echo "Testing overlong announce URL rejected"
python3 - <<PY
path = "$TEST_RESULTS_DIR/cli_longurl.txt"
open(path, "w").write("http://example.com/" + ("a" * 8200))
PY
LONG_URL=$(cat "$TEST_RESULTS_DIR/cli_longurl.txt")
run_test "Overlong announce URL fails" \
    "$MKTORRENT -q -a $LONG_URL -o $TEST_RESULTS_DIR/cli_url.torrent $TEST_DATA_DIR/cli_a.dat" 1

echo "Testing --no-follow-symlinks skips nested symlink file"
mkdir -p "$TEST_DATA_DIR/cli_nf/sub"
cp "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_nf/keep.dat"
ln -sfn "$TEST_DATA_DIR/cli_b.dat" "$TEST_DATA_DIR/cli_nf/sub/outside.link"
run_test "No-follow directory walk create" \
    "$MKTORRENT -q --no-follow-symlinks -n nf -o $TEST_RESULTS_DIR/cli_nf.torrent $TEST_DATA_DIR/cli_nf"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if grep -aq 'keep.dat' "$TEST_RESULTS_DIR/cli_nf.torrent" \
    && ! grep -aq 'outside.link' "$TEST_RESULTS_DIR/cli_nf.torrent" \
    && ! grep -aq 'cli_b.dat' "$TEST_RESULTS_DIR/cli_nf.torrent"; then
    echo -e "${GREEN}Test passed: nested symlink skipped under --no-follow-symlinks${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: nested symlink handling${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing multi-segment directory paths in torrent (MK-020)"
mkdir -p "$TEST_DATA_DIR/cli_seg/a/b"
cp "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_seg/a/b/leaf.dat"
run_test "Multi-segment path create" \
    "$MKTORRENT -q -n seg -o $TEST_RESULTS_DIR/cli_seg.torrent $TEST_DATA_DIR/cli_seg"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if grep -aq 'leaf.dat' "$TEST_RESULTS_DIR/cli_seg.torrent"; then
    echo -e "${GREEN}Test passed: multi-segment path encoded${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: multi-segment path missing${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing exclude pattern count cap (MK-018)"
PATS=$(python3 -c 'print(",".join(["p%d"%i for i in range(257)]))')
run_test "Too many exclude patterns fails" \
    "$MKTORRENT -q -e $PATS -o $TEST_RESULTS_DIR/cli_epat.torrent $TEST_DATA_DIR/cli_a.dat" 1

echo "Testing --skip-hidden and --include"
mkdir -p "$TEST_DATA_DIR/cli_inc/.git" "$TEST_DATA_DIR/cli_inc/sub"
cp "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_inc/keep.dat"
cp "$TEST_DATA_DIR/cli_b.dat" "$TEST_DATA_DIR/cli_inc/skip.tmp"
cp "$TEST_DATA_DIR/cli_a.dat" "$TEST_DATA_DIR/cli_inc/.git/secret.dat"
cp "$TEST_DATA_DIR/cli_b.dat" "$TEST_DATA_DIR/cli_inc/sub/nested.dat"
run_test "Include dat exclude tmp" \
    "$MKTORRENT -q --include '*.dat' -e '*.tmp' -n inc -o $TEST_RESULTS_DIR/cli_inc.torrent $TEST_DATA_DIR/cli_inc"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if grep -aq 'keep.dat' "$TEST_RESULTS_DIR/cli_inc.torrent" \
    && grep -aq 'nested.dat' "$TEST_RESULTS_DIR/cli_inc.torrent" \
    && ! grep -aq 'skip.tmp' "$TEST_RESULTS_DIR/cli_inc.torrent"; then
    echo -e "${GREEN}Test passed: include/exclude filter${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: include/exclude filter${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
run_test "Skip-hidden create" \
    "$MKTORRENT -q --skip-hidden -n hid -o $TEST_RESULTS_DIR/cli_hid.torrent $TEST_DATA_DIR/cli_inc"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if grep -aq 'keep.dat' "$TEST_RESULTS_DIR/cli_hid.torrent" \
    && ! grep -aq 'secret.dat' "$TEST_RESULTS_DIR/cli_hid.torrent"; then
    echo -e "${GREEN}Test passed: --skip-hidden drops .git${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: --skip-hidden${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing io_uring knobs require --io-uring"
if ("$MKTORRENT" -h 2>&1 || true) | grep -q -- '--io-uring'; then
    run_test "io-uring-depth without --io-uring fails" \
        "$MKTORRENT -q --io-uring-depth 8 -o $TEST_RESULTS_DIR/cli_iou_d.torrent $TEST_DATA_DIR/cli_a.dat" 1
    run_test "io-uring with depth create" \
        "$MKTORRENT -q --io-uring --io-uring-depth 8 -o $TEST_RESULTS_DIR/cli_iou_d8.torrent $TEST_DATA_DIR/cli_a.dat"
fi

echo "Testing human piece length -l 1M matches -l 20"
H20=$("$MKTORRENT" --json -q -f -d -l 20 -o "$TEST_RESULTS_DIR/cli_l20.torrent" \
    "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
H1M=$("$MKTORRENT" --json -q -f -d -l 1M -o "$TEST_RESULTS_DIR/cli_l1m.torrent" \
    "$TEST_DATA_DIR/cli_a.dat" | jq -r .infoHash)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if [ -n "$H20" ] && [ "$H20" = "$H1M" ]; then
    echo -e "${GREEN}Test passed: -l 1M matches -l 20${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: -l 1M ($H1M) != -l 20 ($H20)${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

run_test "Bad piece length -l 3 fails" \
    "$MKTORRENT -q -l 3 -o $TEST_RESULTS_DIR/cli_lbad.torrent $TEST_DATA_DIR/cli_a.dat" 1
run_test "Bad piece length -l 1000 fails" \
    "$MKTORRENT -q -l 1000 -o $TEST_RESULTS_DIR/cli_lbad2.torrent $TEST_DATA_DIR/cli_a.dat" 1

echo "Testing DHT --node embedded in torrent"
run_test "Create with DHT node" \
    "$MKTORRENT -q --node 127.0.0.1:6881 -o $TEST_RESULTS_DIR/cli_node.torrent $TEST_DATA_DIR/cli_a.dat"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if grep -aq '127.0.0.1' "$TEST_RESULTS_DIR/cli_node.torrent" \
    && grep -aq 'nodes' "$TEST_RESULTS_DIR/cli_node.torrent"; then
    echo -e "${GREEN}Test passed: DHT node in torrent${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: DHT node missing${NC}"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo

echo "Testing JSON escapes invalid UTF-8 name (MK-019)"
BAD_NAME=$(python3 -c 'import sys; sys.stdout.buffer.write(b"n\xffame")')
JSON_UTF=$("$MKTORRENT" --json -q -n "$BAD_NAME" -o "$TEST_RESULTS_DIR/cli_utf.torrent" \
    "$TEST_DATA_DIR/cli_a.dat")
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if echo "$JSON_UTF" | python3 -c 'import sys,json; j=json.load(sys.stdin); assert j["ok"] is True' \
    && echo "$JSON_UTF" | grep -q '\\u00ff'; then
    echo -e "${GREEN}Test passed: invalid UTF-8 escaped in JSON${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}Test failed: expected \\\\u00ff in JSON${NC}"
    echo "$JSON_UTF"
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo
