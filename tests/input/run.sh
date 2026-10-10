#!/bin/sh
# Plain JVM test of the held-button drag motion (HeldDragMotion.java + a model of how TouchInputHandler
# and GestureDetector drive it). Needs only a JDK; Android classes are not involved.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../app/src/main/java/com/termux/x11/input"
OUT=${OUT:-${TMPDIR:-/tmp}/lorie-input-test}
JAVAC=${JAVAC:-javac}
JAVA=${JAVA:-java}
rm -rf "$OUT" && mkdir -p "$OUT"
"$JAVAC" --release 9 -Xlint:all -d "$OUT" "$SRC/HeldDragMotion.java" "$HERE/HeldDragMotionTest.java"
"$JAVA" -cp "$OUT" com.termux.x11.input.HeldDragMotionTest
