# Controller compatibility and verification

The source supports original 2015 Steam Controllers over wired USB (`28de:1102`)
and the Valve USB wireless receiver (`28de:1142`), alongside the existing 2026
controller path. Original Bluetooth LE uses a different protocol and is
**not implemented**. SDL lists original Bluetooth variants as `1105`/`1106`;
`1201` is a wired HEADCRAB variant, not the original controller's BLE ID. The
pre-existing discovery aliases `11fc`, `11ff`, `1201`, and `0476` are not evidence
of original-controller support; the original decoder and feature commands are
selected only for `1102` and `1142`.

Original BLE support is feasible but requires its own transport adapter. SDL's
implementation assembles 20-byte reports (18 payload bytes per segment), decodes
optional button/axis/IMU chunks while retaining omitted state, and segments
outgoing feature commands. The implemented USB/receiver adapter instead expects
complete 64-byte state reports and sends 65-byte feature buffers. BLE therefore
needs packet assembly, chunk decoding, feature transport, discovery, and physical
pairing/reconnect checks; enabling another device ID alone would not add it.
See SDL's [Steam Controller driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam.c)
and [controller ID list](https://github.com/libsdl-org/SDL/blob/main/src/joystick/controller_list.h).

The original-controller implementation has host tests and an ARM build. The
installer and tracked binary in this fork are marked as an original-controller
preview. They have not yet passed a physical original-controller playtest.
Pad feel, haptic intensity, gyro direction/gain under real motion, and MiSTer
core mapping remain hardware acceptance gates. A running driver or an accepted
feature ioctl alone does not establish those results. `make arm` writes only the
ignored `obj/` directory. `make package` verifies the candidate and deterministically updates
the tracked ARM binary, self-extracting installer, and checksum manifest.

## Original-controller controls

Ordinary face buttons, shoulders, analog/digital triggers, Start/Menu,
Select/View, Steam, stick axes/click, and the two rear grips retain their gamepad
roles. The left grip maps to `BTN_0`, the right to `BTN_1`. The right pad remains
Orbit's trackball/mouse and the left pad its spinner/number pad by default.
Pad clicks retain those tools' existing click actions. There is no physical
right stick, second pair of grips, quick-access button, or stick touch sensor.

**Hold both rear grips** to adjust settings. The combination temporarily
suppresses gameplay outputs. Release shortcut buttons and return the stick from
its settings direction before gameplay resumes. Press shortcut controls after
holding the modifier; each action fires once per press.

| With both rear grips held | Action |
| --- | --- |
| Left / right shoulder | Spinner speed down / up |
| Left / right trigger fully pressed | Trackball speed down / up |
| Stick or left-pad direction left / right | Trackball friction down / up |
| Stick or left-pad direction down / up | Momentum down / up |
| Y | Reverse spinner direction |
| X | Cycle haptic intensity |
| B | Toggle number pad |
| A | Swap pad roles |
| Spinner-pad click | Change spinner mode, or number-pad layout when enabled |
| Trackball-pad click | Toggle mouse / trackball behavior |
| Start/Menu | Save profile |
| Select/View | Load profile |
| Steam | Restore defaults |

For gyro settings, **hold both rear grips and hold the stick clicked**, then:

| Additional control | Action |
| --- | --- |
| Y | Toggle gyro master |
| X | Cycle gyro inversion |
| Left / right shoulder | Gyro sensitivity down / up |
| A | Arm / disarm gyro assistance on the left stick |

Release the additional control before changing layers to avoid invoking its
other-layer action. During ordinary play, hold the stick clicked to clutch armed
left-stick gyro assistance; the original has no capacitive stick-touch signal.
Its ordinary stick-click gamepad action still passes through. Pad touch continues
to clutch gyro mouse and Gyro Paddle as before. Right-stick assistance is not
available on the original; a shared saved profile may retain that setting but
cannot activate it without a right-stick touch signal.

## Protocol and behavior

- Original state reports must be exactly 64 bytes, with header `01 00 01 3c`.
  They are adapted to the existing internal layout. Newer `42`, `45`, and `47`
  reports use the existing processing path and retain their bytes, lengths,
  feature commands, identities, settings defaults, and haptic output format.
- Original bytes 16–19 alternate between left pad and stick. Touch plus the
  simultaneous-use bit distinguish them. The adapter retains the other surface
  during alternation and clears it when firmware reports it inactive.
- Eight-bit triggers scale to the virtual device's existing 0–32767 range.
  Emitted stick axes saturate to the descriptor's -32767…32767 range, including
  raw -32768 and its Y-inverted +32768. Raw decoding retains the full signed
  16-bit values; this output boundary correction also applies to newer sticks.
- Original accelerometer bytes 28–33 and gyro bytes 34–39 feed the existing rest
  calibration. Raw gyro Y is inverted to match the newer sensor convention,
  saturating the signed minimum. The sequence counter is used for duplicate
  rejection, **not** treated as a microsecond clock. Original gyro timing uses
  monotonic receive time, clamped to the engine's existing 1–16 ms bounds, with
  a 9 ms initial interval. Queueing can affect receive-time estimates; this is
  not a measured original-controller timing or feel guarantee.
- Clear digital mappings (`81`), disable pad mouse modes (`87`, settings 7/8),
  and request raw accel/gyro (`87`, setting 48, value `0018`) use a 65-byte
  original feature buffer including report ID zero. Newer feature reports remain
  64 bytes with the existing report-ID fallback. Original commands never try
  the newer report ID as a fallback.
- Original haptics use feature command `8f`, payload length 8, reversed pad
  selector (left=1, right=0), little-endian on/off microseconds and repeat count,
  and signed gain clamped to -24…+6. Tone pitch becomes a pulse period; duration
  is bounded to one second. Newer LFO/depth and rear channels have no original
  equivalent. Four-channel songs retain only notes scheduled on the two pad
  channels. This is approximate feedback, not an identical sound/feel claim.
- Receiver battery/status packets cannot activate a controller or renew input
  liveness. Explicit disconnect releases all virtual outputs and clears the
  adapter. Duplicate connect notifications are ignored once connected. The loop
  reconfigures on a genuine connect and rescans after disconnect. Two seconds
  without a valid original state report releases outputs and resumes scanning,
  including if a receiver continues sending status chatter. This timeout assumes
  the requested raw IMU stream is running and requires hardware verification.
- This remains one active controller at a time. No transport auto-detection,
  BLE reassembly, multi-controller routing, or firmware changes are included.

## Build and host checks

On a host with a C compiler:

```sh
make test
make test-sanitize
```

The tests include the actual production C file. Only the syscall boundary is
stubbed: decoding, shortcuts, calibration, gamepad/mouse/spinner emission,
configuration/haptic serialization, and the reader/disconnect loop are real
production functions. Cases cover original wired and receiver reports,
malformed/truncated/unrelated packets, multiplexed input, touch/click state,
axis/trigger boundaries, stationary gyro calibration and moving gyro output,
modifier suppression/rearming, repeated receiver status, disconnect/reconnect,
and newer command/report regression. The newer fixture is a real 54-byte USB
`42` report captured before this change, not an original-controller capture.

For ARMv7, install an `arm-linux-gnueabihf-gcc` cross compiler, then:

```sh
make arm
file obj/SC-Orbit-driver.armv7l
```

The Makefile includes the freestanding, static, soft-float ARMv7 flags and the
`_start` entry point. No libc or libgcc dependency is required. Build products
are ignored. If alternating host architectures in one checkout, force a test
rebuild with `make -B test` (or `make clean`) to avoid reusing the other host's
executable. To build the documented toolchain from the checked-in recipe:

```sh
docker build -f Dockerfile.armv7 -t sc-orbit-arm-builder:local .
docker run --rm -v "$PWD:/work" -w /work sc-orbit-arm-builder:local make package
```

The package target writes the binary from `obj/` and embeds that same candidate
in `SC-Orbit.sh`; it refreshes `SHA256SUMS.txt` for the package files present in
this fork. It does not run or install the driver on a MiSTer.

Before release, verify original USB and receiver button/axis/pad/gyro events,
settings and profile actions, actuator side/intensity, original sleep/wake and
receiver reconnect, newer-controller regression, and MiSTer navigation/in-core
mapping. Report host/build, physical-device, and playtest evidence separately.

## MiSTer smoke check (2026-10-11)

The packaged preview was installed on an ARMv7 MiSTer with its existing settings,
startup script, and input mapping preserved byte-for-byte and the stock package
backed up. Its installed binary matched the manifest, stayed running, accepted
the newer wired `28de:1302` controller's raw stream, created all four virtual
devices, and grabbed the physical Valve pointer node after a menu refresh.
A passive eight-second capture saw 1,983 valid newer state reports, compared with
1,984 before installation. The controller was idle, so no button or pad changes
were observed. Original-controller USB/receiver input, physical haptics/gyro,
and navigation/in-core acceptance remain pending.

## Protocol provenance

The adapter is independently written from protocol facts documented in Linux's
[hid-steam.c](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-steam.c):
`steam_do_input_event`, original and Ibex button/IMU mappings,
`steam_haptic_pulse`, `steam_send_report_id`, settings enums, and raw-event
header/status documentation. No GPL implementation was copied into this MIT
project. Reference snapshot inspected for this work: SHA-256
`a48b7304d7eb2ebc2f5e294bbef7abb50d997cebc0acf686e34c0067a9f389f2`.
Linux contributors deserve credit for documenting these hardware protocols;
the upstream SC-Wrapper and SC-Orbit credits and MIT license remain intact.
