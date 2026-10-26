# OneUI 8.x–9: rootless Termux / Termux-X11 scheduler experiment

## Reproduced on S26U (SM-S948N, Android 17 / OneUI 9)

Termux and Termux-X11 share Linux UID 10400, but when the X11 Activity is visible:

| Process tree | Android cpuset | Allowed cores |
|---|---|---|
| Termux-X11 Activity, native XFCE4 / xfwm4 | /top-app | 0–7 |
| Termux app, existing native shell / DevSpace | /foreground | 0–5 |

In the background, both processes were placed in /moderate, allowing 0–1 and 4–5.
This S26U's fastest reported cores are 6–7. Unprivileged taskset requests for 6–7
failed with EINVAL; direct writing to /dev/cpuset/top-app/tasks was denied.

The existing custom $PREFIX/bin/tx11start sets use_topapp=true and broadcasts
ACTION_RUN_TERMUX_COMMAND to the X11 Activity. The Activity creates XFCE4 through
ProcessBuilder and the session inherits its /top-app group. Independent native
Termux shell workloads (including FFmpeg and MLT), however, still run as children
of the separate Termux application process.

## Proposed structural fix (experimental)

Android supports global shared processes across packages with the **same Linux
UID and signing certificate**. This experimental X11 APK gives its application
and all Android components processName=com.termux, so the on-screen X11 Activity
can promote the actual Termux Android process and its shell descendants to
/top-app. This removes the need to spawn individual workloads from X11.

This is **not yet proven on S24+**. The Android process group transitions and
performance must be measured there; matching a manifest attribute alone does
not prove big-core availability. Termux and X11 will also share the Android UI
thread, JVM and crash domain. A crash of one can disrupt both apps. Therefore
normal builds stay unchanged unless the explicit test flag is supplied.

The application-level manifest attribute is deliberate: moving only the main
Activity would leave Preferences, Accessibility and keep-alive components in a
different process, breaking MainActivity.getInstance() users.

## Build variants

Normal build (separate Android processes):

    ./gradlew assembleDebug

Experimental cohost build:

    ./gradlew assembleDebug -Ptx11CohostTermux=true

The GitHub Actions manual workflow adds a cohost_termux checkbox and clearly
labels its ARM64 APK artifact as COHOST-EXPERIMENT. The merged manifest is
checked by tools/oneui-cohost/verify-manifest.py after build.

## S24+ testing (do not install on the S26U DevSpace host)

1. Preserve the known-good Termux-X11 APK for rollback. Update *only* X11 with
   the signed experimental APK, without uninstalling or clearing Termux app data.
   If Android reports a signing mismatch, stop; never uninstall Termux to bypass it.
2. Launch using tx11start --no-topapp. This deliberately starts XFCE4 from the
   native Termux shell rather than from X11's prior ProcessBuilder workaround.
3. Bring the Termux-X11 Activity to the foreground. Run:

       bash tools/oneui-cohost/inspect-scheduling.sh

   The OneUiProcessHost runtime log should report cohost=true process=com.termux
   and its PID should equal pidof com.termux. Expect that process, XFCE4, and
   native Termux shells to be /top-app and to allow the big cores.
4. Start a CPU-bound Termux-native FFmpeg/MLT workload *outside the X11 runner*
   and verify its own /proc/PID/cgroup and Cpus_allowed_list. Compare actual
   throughput with X11 foreground vs Termux foreground under similar thermals.
5. If the shared-process build passes full GUI/session/keyboard/CPU validation,
   the optional disable-spawn-default.sh install safely backs up your custom
   tx11start and changes the default use_topapp=true to false. Use the restore
   action to restore your previous script.

Do **not** install this test APK on the remote S26U DevSpace server. Installing
an X11 APK hosted in com.termux can terminate its Termux Android process and
disconnect that server. The first dynamic shared-process test belongs on a
separate device with local recovery access.

## Limits

No root, ADB privilege, cgroup modification, governor override or per-command
CPU scheduling wrapper is introduced. If OneUI refuses the shared-process layout,
ordinary unprivileged apps cannot force arbitrary existing processes into
/top-app. Retain the original APK or use an authorized privileged system-level
process-management solution instead. The native Vulkan/XRender performance
experiments are not changed by this patch.
