#!/bin/sh
# Judges which renderer backend actually drew the screen, from the X server's diagnostics lines (see
# renderer-status.sh) - the counters only the backend's own successful API calls move. Sums each counter's
# per-report increase over the last N reports (5 s each) and checks:
#
#   --expect vulkan   active=VULKAN, ownership=per_access and the ownership self-test result=pass in every one
#                     of the N reports, and over them
#                     ahb_import_success (total) > 0, imported_source_draws > 0, queue_submits > 0,
#                     queue_present_success > 0, frames_presented > 0,
#                     gles_copy_frames = 0, egl_swap_buffers = 0, fallback total = 0
#   --expect gles     active=GLES and ownership=persistent (3c98ffa's) in every one of the N reports, and
#                     over them gles_copy_frames > 0, egl_swap_buffers > 0,
#                     queue_present_success = 0, frames_presented = 0, fallback total = 0
#
# Prints every check with its numbers. Exit 0 PASS, 1 FAIL, 2 not enough reports to judge.
#
# usage: renderer-proof.sh --expect vulkan|gles [--reports N] [--file LOG]
#   --reports N  how many of the latest reports to judge (default 3, i.e. 15 s)
#   --file LOG   read a saved log instead of `logcat -d`
set -e
usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }
EXPECT= FILE= N=3
while [ $# -gt 0 ]; do
    case "$1" in
        --expect) EXPECT=$2; shift 2 ;;
        --reports) N=$2; shift 2 ;;
        --file) FILE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "renderer-proof: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
case "$EXPECT" in vulkan|gles) ;; *) echo "renderer-proof: --expect vulkan or --expect gles" >&2; usage >&2; exit 2 ;; esac
case "$N" in ''|*[!0-9]*|0) echo "renderer-proof: --reports takes a positive number" >&2; exit 2 ;; esac
if [ -n "$FILE" ]; then
    [ -r "$FILE" ] || { echo "renderer-proof: cannot read $FILE" >&2; exit 2; }
    SRC() { cat "$FILE"; }
else
    command -v logcat >/dev/null 2>&1 || { echo "renderer-proof: no logcat here; pass --file" >&2; exit 2; }
    SRC() { logcat -d -s LorieNative:I 2>/dev/null; }
fi
set +e
SRC | awk -v expect="$EXPECT" -v N="$N" '
    /XlorieBackend: / { nb++; B[nb] = $0 }
    /XlorieGles: /    { ng++; Gl[ng] = $0 }
    /XlorieVk: /      { nv++; V[nv] = $0 }
    /XlorieFallback: / { nf++; F[nf] = $0 }
    /XlorieVkSelftest: / { nst++; ST[nst] = $0 }
    function take(line, prefix,    n, i, tok, k, rest, tot, d) {
        n = split(line, tok, " ")
        for (i = 1; i <= n; i++) {
            if (tok[i] ~ /^[a-z_]+=[0-9]+\(\+[0-9]+\)$/) {
                k = tok[i]; sub(/=.*/, "", k)
                rest = tok[i]; sub(/^[a-z_]+=/, "", rest)
                tot = rest; sub(/\(.*/, "", tot)
                d = rest; sub(/^[0-9]+\(\+/, "", d); sub(/\)$/, "", d)
                total[prefix k] = tot + 0; delta[prefix k] += d + 0
            }
        }
    }
    function check(name, ok, detail) {
        printf "%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail
        if (!ok) failed++
    }
    END {
        if (nb < N || ng < N || nv < N || nf < N) {
            printf "RESULT: INSUFFICIENT - %d report(s) found, %d wanted (Backend %d, Gles %d, Vk %d, Fallback %d lines)\n", \
                   (nb < ng ? nb : ng), N, nb, ng, nv, nf
            exit 2
        }
        want = expect == "vulkan" ? "VULKAN" : "GLES"
        wantown = expect == "vulkan" ? "per_access" : "persistent"
        allactive = 1; seen = ""; allown = 1; seenown = ""
        for (i = nb - N + 1; i <= nb; i++) {
            a = B[i]; sub(/.*active=/, "", a); sub(/ .*/, "", a)
            seen = seen (seen ? "," : "") a
            if (a != want) allactive = 0
            o = B[i]
            if (o ~ / ownership=/) { sub(/.* ownership=/, "", o); sub(/ .*/, "", o) } else o = "none"
            seenown = seenown (seenown ? "," : "") o
            if (o != wantown) allown = 0
        }
        allst = nst >= N; seenst = ""
        for (i = nst - N + 1; i >= 1 && i <= nst; i++) {
            q = ST[i]; sub(/.*XlorieVkSelftest: result=/, "", q); sub(/ .*/, "", q)
            seenst = seenst (seenst ? "," : "") q
            if (q != "pass") allst = 0
        }
        if (nst < N) seenst = seenst (seenst ? "," : "") "missing"
        for (i = ng - N + 1; i <= ng; i++) take(Gl[i], "gles.")
        for (i = nv - N + 1; i <= nv; i++) take(V[i], "vk.")
        for (i = nf - N + 1; i <= nf; i++) take(F[i], "fb.")
        check("active=" want " in all " N " reports", allactive, seen)
        check("X server AHB ownership=" wantown " in all " N " reports", allown, seenown)
        if (expect == "vulkan") {
            check("ownership self-test result=pass in all " N " reports", allst, seenst)
            check("vk ahb_import_success total > 0", total["vk.ahb_import_success"] > 0, total["vk.ahb_import_success"])
            check("vk imported_source_draws > 0", delta["vk.imported_source_draws"] > 0, delta["vk.imported_source_draws"])
            check("vk queue_submits > 0", delta["vk.queue_submits"] > 0, delta["vk.queue_submits"])
            check("vk queue_present_success > 0", delta["vk.queue_present_success"] > 0, delta["vk.queue_present_success"])
            check("vk frames_presented > 0", delta["vk.frames_presented"] > 0, delta["vk.frames_presented"])
            check("gles_copy_frames = 0", delta["gles.gles_copy_frames"] == 0, delta["gles.gles_copy_frames"])
            check("egl_swap_buffers = 0", delta["gles.egl_swap_buffers"] == 0, delta["gles.egl_swap_buffers"])
        } else {
            check("gles_copy_frames > 0", delta["gles.gles_copy_frames"] > 0, delta["gles.gles_copy_frames"])
            check("egl_swap_buffers > 0", delta["gles.egl_swap_buffers"] > 0, delta["gles.egl_swap_buffers"])
            check("vk queue_present_success = 0", delta["vk.queue_present_success"] == 0, delta["vk.queue_present_success"])
            check("vk frames_presented = 0", delta["vk.frames_presented"] == 0, delta["vk.frames_presented"])
            printf "info vk instance_created total: %d\n", total["vk.instance_created"]
        }
        check("fallback total = 0 over the reports", delta["fb.total"] == 0, delta["fb.total"])
        printf "info imported_destination_draws %d, render_commands %d, copy_commands %d, blit_commands %d\n", \
               delta["vk.imported_destination_draws"], delta["vk.render_commands"], delta["vk.copy_commands"], delta["vk.blit_commands"]
        print (failed ? "RESULT: FAIL" : "RESULT: PASS")
        exit failed ? 1 : 0
    }'
exit $?
