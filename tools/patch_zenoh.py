"""PlatformIO pre-build hook: fix zenoh-pico's ESP-IDF timed-wait clock bug.

zenoh-pico's background executor sleeps between timed tasks with
`_z_condvar_wait_until(abstime)`, where `abstime` is CLOCK_MONOTONIC
(`z_clock_now`). Its ESP-IDF port asks the condvar to use CLOCK_MONOTONIC via
`pthread_condattr_setclock(CLOCK_MONOTONIC)` -- but ESP-IDF prints
"pthread_condattr_setclock: not yet supported!" and `pthread_cond_timedwait`
interprets `abstime` against CLOCK_REALTIME (`gettimeofday`). The two are
unrelated clock domains; with REALTIME ahead of MONOTONIC (RTC-backed system
time, any SNTP/RTC sync) the deadline reads as already past, so every timed
wait returns ETIMEDOUT instantly and the executor hot-spins at ~90 kHz.

On the dual-core boards this only wastes a core during island bring-up (a
connected peer's blocking select() then throttles the loop, and IDLE lives on
the other core). On the SINGLE-CORE esp32c3-supermini there is no other core:
the prio-12 executor starves prio-1 `board_run` before it can finish standing
the hub up, and the task watchdog fires forever -- the board never islands.
Diagnosed on the wire 2026-07-21 (per-task CPU stats: IDLE 0%, executor 100%;
condvar waits == instant returns). Reported: eclipse-zenoh/zenoh-pico#1270.

Two patches, both idempotent and both fail-LOUD if their anchor stops matching
(so a zenoh-pico bump can't silently reship the hang):

  1. _z_condvar_wait_until -- translate the monotonic deadline into the
     equivalent realtime deadline, re-reading both clocks each call (a later
     SNTP step that jumps REALTIME can't reintroduce the skew). The remainder
     math is 64-bit on purpose: `long` is 32 bits on the C3 (RV32) and would
     overflow above ~2.15 s, and `_z_sync_group_wait_deadline` (blocking z_get)
     routes multi-second user deadlines through this same function.
  2. _z_condvar_init -- drop the pthread_condattr_setclock(CLOCK_MONOTONIC)
     request. It is a no-op today, but if a future ESP-IDF started honoring it
     the condvar would read the translated *realtime* deadline as monotonic and
     wait for decades -- the failure mode inverted. Dropping it keeps the port
     self-consistent (waits resolve against REALTIME, which is what patch 1
     targets).

zenoh-pico is fetched into PROJECT_LIBDEPS_DIR from the pin in platformio.ini
(gitignored), so the fix cannot live in the tree -- this hook reapplies it on
every build. Drop the hook if the pin moves to a release that carries the fix
upstream.
"""
import os

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

REL = os.path.join("zenoh-pico", "src", "system", "espidf", "system.c")

# Each patch: (marker unique to the applied form, marker unique to upstream's
# own equivalent fix, pristine anchor, replacement). zenoh-pico main carries
# both fixes natively since 2026-07-21 (#1270) -- the upstream marker makes this
# hook a verified no-op there, while a 1.9.0 checkout still gets patched and an
# unrecognized rewrite still fails loud.
PATCHES = [
    (
        "monotonic deadline into the equivalent realtime deadline",
        "Convert the remaining monotonic duration",
        """z_result_t _z_condvar_wait_until(_z_condvar_t *cv, _z_mutex_t *m, const z_clock_t *abstime) {
    int error = pthread_cond_timedwait(cv, m, abstime);

    if (error == ETIMEDOUT) {
        return Z_ETIMEDOUT;
    }

    _Z_CHECK_SYS_ERR(error);
}""",
        """z_result_t _z_condvar_wait_until(_z_condvar_t *cv, _z_mutex_t *m, const z_clock_t *abstime) {
    // `abstime` is CLOCK_MONOTONIC (z_clock_now), but ESP-IDF's pthread_cond_timedwait
    // interprets abstime against CLOCK_REALTIME (gettimeofday) -- unrelated clock
    // domains -- and pthread_condattr_setclock(CLOCK_MONOTONIC) is a no-op here (ESP-IDF
    // warns "not yet supported"). With REALTIME ahead of MONOTONIC the deadline reads as
    // already past, so every timed wait returns ETIMEDOUT instantly and the background
    // executor hot-spins -- fatal on a single-core target (starves lower-prio tasks).
    // Translate the monotonic deadline into the equivalent realtime deadline. The
    // remainder is 64-bit: `long` is 32-bit on this target and overflows above ~2.15 s,
    // and _z_sync_group_wait_deadline (blocking z_get) routes multi-second user deadlines
    // through here too. (Patched in by tools/patch_zenoh.py -- see that file.)
    struct timespec mono_now, real_now;
    clock_gettime(CLOCK_MONOTONIC, &mono_now);
    clock_gettime(CLOCK_REALTIME, &real_now);
    long long rem_ns = (long long)(abstime->tv_sec - mono_now.tv_sec) * 1000000000LL +
                       (long long)(abstime->tv_nsec - mono_now.tv_nsec);
    if (rem_ns < 0) {
        rem_ns = 0;
    }
    struct timespec real_abs = real_now;
    real_abs.tv_sec += (time_t)(rem_ns / 1000000000LL);
    real_abs.tv_nsec += (long)(rem_ns % 1000000000LL);
    if (real_abs.tv_nsec >= 1000000000L) {
        real_abs.tv_sec += 1;
        real_abs.tv_nsec -= 1000000000L;
    }
    int error = pthread_cond_timedwait(cv, m, &real_abs);

    if (error == ETIMEDOUT) {
        return Z_ETIMEDOUT;
    }

    _Z_CHECK_SYS_ERR(error);
}""",
    ),
    (
        "intentionally NOT pthread_condattr_setclock",
        "conversion is handled in _z_condvar_wait_until",
        """z_result_t _z_condvar_init(_z_condvar_t *cv) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    _Z_CHECK_SYS_ERR(pthread_cond_init(cv, &attr));
}""",
        """z_result_t _z_condvar_init(_z_condvar_t *cv) {
    // intentionally NOT pthread_condattr_setclock(CLOCK_MONOTONIC): ESP-IDF does not
    // honor it (warns "not yet supported") and resolves timed waits against
    // CLOCK_REALTIME. _z_condvar_wait_until translates monotonic deadlines into that
    // domain; requesting MONOTONIC here would, if a future ESP-IDF honored it, make the
    // condvar read those realtime deadlines as monotonic and wait for decades.
    // (Patched in by tools/patch_zenoh.py -- see that file.)
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    _Z_CHECK_SYS_ERR(pthread_cond_init(cv, &attr));
}""",
    ),
]

path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), REL)  # noqa: F821

if not os.path.isfile(path):
    # Not installed yet on this pass (or a non-espidf env); nothing to do. Verified
    # on a clean `pio run`: the fetch precedes this hook, so a real build reaches the
    # patch below (it logs "applied ..." before any zenoh-pico source compiles).
    print("patch_zenoh: %s not present, skipping" % path)
else:
    with open(path, "r") as f:
        src = f.read()
    applied = []
    upstream = 0
    for marker, upstream_marker, anchor, replacement in PATCHES:
        if marker in src:
            continue  # already patched this checkout
        if upstream_marker in src:
            upstream += 1  # this checkout ships its own fix -- nothing to patch
            continue
        if anchor not in src:
            raise SystemExit(
                "patch_zenoh: FAILED to find an anchor in %s -- zenoh-pico changed. "
                "Re-verify the timed-wait clock fix before shipping (single-core boards "
                "hang on island bring-up without it)." % path
            )
        src = src.replace(anchor, replacement)
        applied.append(marker)
    if applied:
        with open(path, "w") as f:
            f.write(src)
        print("patch_zenoh: applied ESP-IDF condvar monotonic/realtime fix (%d hunk(s))" % len(applied))
    elif upstream:
        print("patch_zenoh: upstream checkout carries its own clock fix (%d hunk(s)) -- nothing to patch" % upstream)
