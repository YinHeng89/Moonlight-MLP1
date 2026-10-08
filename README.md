# Moonlight for MLP1

Moonlight Embedded (moonlight-stream) cross-compiled for the **Miniloong Pocket 1
(MLP1 / RK3566, aarch64)**, packaged as a Leaf tool pak.

If you only want to use it, download `Moonlight.mlp1.pak.zip` from the
[latest release](https://github.com/YinHeng89/Moonlight-MLP1/releases/latest)
and unzip it — no toolchain needed. Every push builds it on CI; a `v*` tag also
publishes it. Building from source is described further down.

## What is in here

```
build/mlp1/moonlight           the client, 7.9 MB stripped, one self-contained file
build/mlp1/moonlight-notice    fullscreen message program (shows the pairing PIN)
build/package/Moonlight.pak    the assembled pak, ready to copy to the SD card
build/dist/Moonlight.mlp1.pak.zip  the same pak, zipped
```

## Why the Embedded flavour and not Qt

The pinned MLP1 toolchain sysroot has **no Qt at all** and no ffmpeg; it does
have SDL2 2.28.5 — the exact version the device ships — plus ALSA, OpenSSL 3
and libcurl. moonlight-qt would have meant cross-building Qt 5 and bundling
tens of megabytes of it. moonlight-embedded is plain C, SDL for video/audio and
input, and the same shape as the reference paks this was modelled on
(DSperate-pak, Fun-Drastic-standalone): one lean binary linking libraries the
device already carries.

## Install

Copy `build/package/Moonlight.pak` to the SD card:

```
/mnt/SDCARD/Tools/mlp1/Moonlight.pak
```

## First run

1. Edit `Tools/mlp1/Moonlight.pak/moonlight-user.conf` on the card:
   - `HOST="192.168.1.20"` — your PC running Sunshine. Leave it empty and
     moonlight finds the first host that answers on the network (mDNS).
   - `APP="Steam"` — the name as it appears on the host.
2. Set `MODE="pair"` and open the pak. The PIN appears on the MLP1's screen.
   Sunshine does not pop up a dialog the way GeForce Experience does: open
   `https://<HOST>:47990` on the PC, log in, and type the PIN into its PIN
   page while the PIN is still on the handheld's screen.
3. On success the pak sets `MODE="stream"` back for you; open it again and it
   streams. (If the card is mounted read-only it says so and you edit it by
   hand.)

Everything the launch prints goes to the shared log, `moonlight.txt`.

### When nothing appears on screen

Set `MODE="diag"` and open the pak. It does not stream; it measures whether
this device can reach `HOST` at all — ping the gateway, ping the PC, connect to
ports 47989 and 47984, then one real `moonlight list` request — and puts the
result on screen. It also proves the screen works: if the panel shows up, every
other "black screen" is about the network, not about rendering.

A failed `stream` now says why on screen too (not paired / cannot reach host /
app not on host) instead of ending in an empty screen.

### Settings

| Variable | Meaning | Default |
|---|---|---|
| `HOST` | PC address; empty = mDNS autodiscovery | empty |
| `APP` | application name on the host | `Steam` |
| `MODE` | `stream` / `pair` / `list` / `diag` / `probe` / `quit` | `stream` |
| `WIDTH`/`HEIGHT`/`FPS` | stream geometry — the panel is 960x720 4:3, and 30 is what software decode keeps up with | `960`/`720`/`30` |
| `BITRATE` | Kbps | `5000` |
| `CODEC` | `h264` or `hevc` | `h264` |
| `PACKETSIZE` | max video packet, kept under the 1500-byte MTU | `1024` |
| `QUIT_COMBO` | SDL key names to hold together to end a stream | empty |
| `EXTRA` | extra moonlight arguments | empty |

Any of them can also be given as an `ML_`-prefixed environment variable
(`ML_MODE`, `ML_HOST`, ...), which wins over the file — that is how a launcher
or a test drives the pak without editing the card.

### Ending a stream

Upstream offers two ways out and neither works here: `Ctrl+Alt+Shift+Q` wants
modifier keys a handheld does not have, and the gamepad combination wants
Start, Select, LB and RB — there is no Select, and if SDL does not recognise
the device as a game controller there are no controller events at all.

So the combination is configurable. `QUIT_COMBO` is a comma-separated list of
up to four SDL key names, and holding all of them at once ends the stream:

```sh
MODE="probe"     # open the pak: press every button, names go to
                 # moonlight-probe.txt on the card
QUIT_COMBO="x,b" # then set this from the names reported there
```

The probe stops on its own after a minute — a program whose job is to find out
which buttons exist cannot ask for a button that does not exist to stop it.

## Reproducing the binary

Docker is the whole toolchain. Nothing else is needed on the host.

```sh
docker run -d --name mlp1-build \
  -v "$PWD:/host" \
  --entrypoint sleep \
  ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain@sha256:66aac16fb8b07e663c9b4d66970f272df195a6eba98dfad8286eabbaa617faf9 \
  infinity

# 1. the libraries the SDK sysroot does not carry (static, installed into it)
docker exec mlp1-build bash /host/scripts/build-deps.sh
# 2. moonlight itself, plus the notice program, plus verification
docker exec mlp1-build bash /host/scripts/build-moonlight.sh
# 3. assemble the pak
./scripts/package-pak.sh
```

`scripts/verify-binary.sh` fails the build unless the binary is AArch64 and
stripped, carries no RPATH/RUNPATH, needs no glibc symbol above 2.38, and every
NEEDED library is one the device provides. Pinned inputs are in
`scripts/upstream.lock.json`.

## Continuous integration

`.github/workflows/build.yml` runs the same three steps on every push and pull
request, and uploads the pak as a build artifact. Pushing a `v*` tag also
publishes it as a GitHub release with the verification report attached.

The job runs on a **native aarch64 runner**, because the toolchain image is
arm64-only — running it under emulation on x86_64 works, but turns the ffmpeg
cross-build from minutes into hours. Native arm64 runners are free for public
repositories; on a private one, make the repository public or fall back to
`ubuntu-latest` plus `docker/setup-qemu-action`, as the workflow comments note.

CI covers compilation and the compliance check. It cannot cover rendering —
see below.

## Repository layout

```
scripts/                 the build: toolchain cmake file, dependency
                         cross-compilation, moonlight build, verification,
                         pak assembly, and the pinned input manifest
pak/                     what goes on the device: pak.json, launch.sh,
                         the user config, and the icon
patches/                 every local change to vendored upstream, as patches
thirdparty/              upstream moonlight-embedded, vendored verbatim
.github/workflows/       CI: build, verify, and release the pak
LICENSE, NOTICE          GPL-3.0-or-later, and the licence of everything
                         bundled or linked in
```

`thirdparty/moonlight-embedded` is a verbatim copy of upstream at the commit
pinned in `scripts/upstream.lock.json`, with the two files listed in
`patches/` modified. It is vendored rather than referenced as a submodule so a
clone builds offline and the exact bytes that produced a given binary are in
the same history as the binary's recipe. Local changes to it are kept as
patches too, so an upstream upgrade is a re-apply rather than an archaeology
exercise.

`build/` is gitignored — it is reproducible from the scripts and the pinned
toolchain image.

## Implementation notes

**Static where the device has nothing to offer.** libudev, libevdev, libuuid,
opus, expat, libcurl, OpenSSL and ffmpeg are linked statically, so the binary's
`NEEDED` list is six device libraries and nothing else:

```
ld-linux-aarch64.so.1  libc.so.6  libm.so.6
libSDL2-2.0.so.0       libasound.so.2  libz.so.1
```

**ffmpeg is minimal on purpose.** Only `libavcodec` and `libavutil`, with the
h264 and hevc decoders and their parsers — that is everything
`src/video/ffmpeg.c` asks for by name. Rendering goes through SDL's YUV texture
path, so there is no swscale, no avformat, no ffmpeg CLI. As well as the
software decoders, the binary carries ffmpeg's `h264_v4l2m2m` and
`hevc_v4l2m2m`, which it tries first; if the kernel exposes a stateful V4L2
decoder the stream uses it instead of the CPU.

**avahi is gone, not stubbed.** `libgamestream/discover.c` was rewritten to ask
the network for `_nvstream._tcp.local` directly over multicast DNS — about 300
lines of C, no dbus, no daemon. It answers the same two calls the avahi version
did, so nothing above it changed. Verified against a scripted mDNS responder:
the client resolves the host, generates its certificate, and reports the
connection failure it should.

**Two toolchain quirks are handled rather than worked around.** ffmpeg's
`.pc` files put `-latomic` in `Libs`, which would put `libatomic.so.1` on the
`NEEDED` list for a handful of 64-bit counters the device does not ship; the
build strips it and links `libatomic.a` statically at the end of the link line
instead. And `libcurl.a` needs zlib for gzip responses, which cmake's `FindCURL`
does not carry across when handed an archive directly.

**What is not verified.** The toolchain's SDL has no video driver usable inside
a container, so real rendering is only provenable on the device — the binary
links the device's own Wayland-capable SDL2 at runtime, and the YUV texture
path it uses is the one upstream's SDL backend has always used. Everything
before that point (discovery, pairing, TLS, decoding) is verified here.

## Licence

GPL-3.0-or-later, because the binary is a derivative of moonlight-embedded.
See `LICENSE`. `NOTICE` lists the licence of every vendored or statically
linked component, and `scripts/upstream.lock.json` pins their exact versions —
that is the written offer corresponding to the source of this build.
