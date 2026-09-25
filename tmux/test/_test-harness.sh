#!/bin/bash
set -e

TESTNAME=$(basename -- $0)
TESTDIR=$(dirname $(readlink -f -- $0))
BINDIR=$(dirname $TESTDIR)

pids=()

run() {
    setsid "$@" </dev/null >"$TESTDIR"/log-"$(basename $1)".txt 2>&1 & pids+=("$!")
}

kill_all() {
    for p in "${pids[@]}"; do
        # Try process group first, then the PID itself.
        { kill -"$1" -- "-$p"; kill -"$1" "$p"; } 2>/dev/null ||:
    done
}

cleanup() {
    ((${#pids[@]})) || return
    kill_all TERM
    sleep .2
    kill_all KILL
    wait "${pids[@]}" 2>/dev/null ||:
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

## spawn dependencies, then u-tmux itself

unset DISPLAY
#run Xvfb :2 -screen 0 640x480x16 -nolisten tcp
run Xvnc :2 -geometry 640x480 -depth 16 -SecurityTypes None
sleep .1
export DISPLAY=:2
run "$TESTDIR/_zerowm.py"
sleep .1
run $BINDIR/tmux
sleep .1

## done with setup, caller can now perform tests
