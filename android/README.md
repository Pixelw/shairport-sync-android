# Android ARMv7 / API 27

This fork ports Shairport Sync 5.6.2 and NQPTP 1.2.9 to Android 8.1 using public
NDK AAudio. The native build produces two PIE executables. An optional Java Companion App
under `app/` displays playback metadata and provides AirPlay 2 controls; see
[Companion App](COMPANION.md) for its separate Gradle build and deployment.
The port targets Bionic ARM EABI (softfp calling convention), with static public
dependencies and no glibc, private AudioTrack ABI, or shared C++ runtime.

The root is the actual receiver source. `nqptp/` is the matching timing daemon;
`port/` holds their shared implementations and `include/` their shared headers. Build scripts compile these files
directly. No upstream patch generator, other checkout, or historical test output
is required. Source origins and licenses are recorded in [NOTICE.md](NOTICE.md).

## Build

Use Python 3.12 or newer, Xcode Command Line Tools on macOS (or a C/C++ compiler
and standard build utilities on Linux), Perl, curl, xxd and Android NDK r27d
(revision `27.3.13750724`). The build host used for this import is Apple Silicon macOS;
Linux host selection is supported by the scripts but has not been validated.
Install NDK using the Android CLI, for example:

```sh
android sdk install ndk/27.3.13750724
export ANDROID_NDK_ROOT=/absolute/path/to/Android/sdk/ndk/27.3.13750724
python3 android/scripts/build.py
```

`ANDROID_NDK_HOME` is also accepted. Without either variable, the script looks
in `android/work/toolchains/android-ndk-r27d`. The build checks the NDK revision
and uses its ARMv7/API27 compiler and API stubs.

The first build downloads the public archives from `dependencies.lock.json`,
verifies their sizes and SHA256, and builds local make, Autotools, pkgconf and
host-native plistutil. Target libraries are static PIC builds, including AAC and
ALAC decoding. It never executes an ARM code generator on the host.

For an offline build, first populate `android/work/downloads/` with the exact
locked archives, then run:

```sh
python3 android/scripts/build.py --offline
```

Results are `android/outputs/armv7-api27/shairport-sync` and `nqptp`, with ELF
reports and a source/binary hash manifest. Logs and caches stay in
`android/work/`. Both directories are ignored by Git. Keep the checkout path
stable while reusing Autotools caches; remove `android/work/` before relocating
a previously built checkout. NDK can remain outside the repository.

Each build checks ELF32/ARM/PIE, RELRO/NOW, API27 imports, allowed public shared
libraries and absence of `pthread_cancel`, private AudioTrack imports, hard-float
ABI, text relocations and runtime search paths. This is a build/ABI check,
not device playback or acoustic synchronization evidence.

## Runtime

The runtime requires access to the audio service, a compatible Android `mdnsd`
socket at `/dev/socket/mdnsd`, network multicast/PTP sockets (including UDP
319/320), and a private IPC directory. Both processes must run as the same UID.
The program does not create that directory, start `mdnsd`, or configure SELinux.
The IPC directory is fixed at `/dev/lx04-ap2-ipc` (0700, owned by the runtime UID);
IPC files are 0600 and validated for type, owner, link count and lifecycle.

On a suitably configured device, pre-create that directory as the runtime UID,
place both binaries and a configuration in an application-owned location, then
start NQPTP before the receiver. An illustrative on-device command sequence is:

```sh
umask 077
mkdir -m 700 /dev/lx04-ap2-ipc
./nqptp &
nqptp_pid=$!
./shairport-sync -c ./shairport-sync.conf
kill -TERM "$nqptp_pid"
wait "$nqptp_pid"
```

Use a supervisor for persistent operation. Stop the receiver before NQPTP, using
SIGTERM and waiting for cleanup. If a crashed earlier run left a stale IPC file,
NQPTP supports `--reset-ipc`; it validates the fixed objects and obtains the
single-writer lock before resetting. Do not delete files belonging to a running
instance. Only one `/lx04` IPC client is supported; the SMI11 structure layout is
unchanged and contains no pthread mutex.

Start with [the standard example](config/shairport-sync.conf.example). It uses
AAudio, dns-sd, flat volume with a 30 dB range and a 0.35 s player queue. The
AAudio buffer capacity is 12000 frames. The global latency offset remains zero;
[the LX04 + Zephyr example](config/shairport-sync.lx04-zephyr.conf.example) applies
an optional -20 ms offset for that combination only.

Android logs default to logcat tags `SPS-main` and `SPS-nqptp`. Either executable
accepts `--log-output=stderr` before normal options. Upstream message text is
retained; receiver verbosity defaults to 1 with playback statistics enabled.
Configuration can override the diagnostic settings. Volume remains software
attenuation: Android system-volume synchronization and AudioFocus ducking are
not implemented. External command hooks, FIFO output and shell-based config
display are deliberately disabled in this port. The optional Companion bridge uses
a UID-checked local Unix socket and remains disabled until configured.

## Validation boundaries

Prior LX04 temporary root/su playback demonstrated buffered AAC 48 kHz and
realtime ALAC 44.1 kHz, with volume, pause, mute and reconnect. The user confirmed
long playback and coexistence with XiaoAi audio; no precise duration or resource
metrics are inferred from that confirmation. These results do not validate a
dedicated low-privilege SELinux domain.

With the optional -20 ms LX04/Zephyr setting, the previous listening session
accumulated 30 min 36 s of valid playback and three listening confirmations
without audible echo. NetEase group leave/rejoin still had an unresolved case
where both outputs became silent; an Apple Music retest worked. The complete
application/codec/rate matrix, uninterrupted reliability and objective acoustic
synchronization remain unverified. This repository import runs offline builds
only and does not repeat device playback.

Inherited upstream Linux/macOS workflows do not validate this Android build.
The upstream Docker publishing workflow is restricted to its original repository.
