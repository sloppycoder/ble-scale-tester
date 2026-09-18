# Timemore Basic 3.0 Link — research notes

Goal: add Timemore Basic 3.0 Link support to GaggiMate's BLE scale plugin
(`../gaggimate`, via `../esp-arduino-ble-scales`). These are research notes
from comparing existing implementations, written **before touching any
code**. Nothing here has been verified against a real Basic 3.0 Link scale
yet — that's the next step once the hardware arrives. Treat every claim
below, including everything sourced from Beanconqueror, as "best guess
pending hardware test," not ground truth.

## Sources compared

| Source | Path | Commit |
|---|---|---|
| gaggimate's scale library (what GaggiMate actually builds against) | `../esp-arduino-ble-scales`, `src/scales/dot.cpp`/`dot.h` (class `TimemoreDotScales`) | `c18e6b1` |
| gaggiuino fork | `../references/esp-arduino-ble-scales`, `src/scales/timemore_new.cpp`/`timemore_new.h` (class `TimemoreNewScales`) | `7a1a00c` |
| Beanconqueror (mobile app, TypeScript, Capacitor BLE plugin) | cloned to scratchpad, `src/classes/devices/timemoreDotScale.ts` and `timemoreBasicScale.ts` | `0ec826a` |

Beanconqueror is the only one of the three that ships an explicit
`TimemoreBasicScale` class distinct from `TimemoreDotScale` — so it's the
only direct evidence (so far) that Basic 3.0 Link support exists anywhere in
the wild. It has not been checked against real hardware by us.

## Finding 1: Beanconqueror's Dot and Basic3 drivers are identical protocol-wise

Diffing `timemoreDotScale.ts` against `timemoreBasicScale.ts` line-by-line:
the only differences are the class name, the `DEVICE_NAME` constant
(`'dot'` vs `'basic3'`), the log label, and the device-name matcher:

```ts
// Dot
device.name.toLowerCase().includes('dot') ||
device.name.toLowerCase().includes('tes017')

// Basic 3.0 Link
device.name.toLowerCase().includes('basic3') ||
device.name.toLowerCase().includes('basic 3') ||
(device.name.toLowerCase().includes('timemore') && device.name.toLowerCase().includes('basic'))
```

Everything else — `SERVICE_UUID`/`CHAR_UUID`/`CMD_UUID` (`FFF0`/`FFF1`/`FFF2`),
`buildCommand()`, the CRC function, `parseStatusUpdate()`, the connect
sequence, tare, and timer commands — is byte-for-byte the same.

**If this holds up against real hardware**, it means Basic 3.0 Link needs no
new driver class in `esp-arduino-ble-scales` — just a wider device-name match
on the existing Dot plugin (`TimemoreDotScalesPlugin::handles()` in
`dot.h`). This was the original question that prompted this research; we're
deferring the actual code change until hardware confirms it.

## Finding 2: CRC16 init value disagreement between the two C++ ports

Both C++ drivers implement (or hardcode) a CRC16 with polynomial `0xA001`
(reflected), but they disagree on the init value:

- `timemore_new.cpp` (`calculateCRC16`): inits `crc = 0x0000`.
- Beanconqueror (`crc16Ibm`, both Dot and Basic3): inits `crc = 0xFFFF`.
- `dot.cpp` doesn't compute CRC at all — it hardcodes two captured frames:
  `TARE_CMD = {0xA5,0x5A,0x02,0x04,0x00,0x00,0x9A,0x00}` and
  `HANDSHAKE_CMD = {0xA5,0x5A,0x03,0x0D,0x00,0x00,0x64,0xD1}`.

Recomputing the CRC over each hardcoded payload with both init values:

```
payload = A5 5A 03 0D 00 00 (dot.cpp's "HANDSHAKE_CMD")
  init=0x0000 -> CRC=0x7FD1
  init=0xFFFF -> CRC=0x64D1   <- matches dot.cpp's hardcoded trailer (0x64,0xD1)

payload = A5 5A 02 04 00 00 (dot.cpp's "TARE_CMD")
  init=0x0000 -> CRC=0x8100
  init=0xFFFF -> CRC=0x9A00   <- matches dot.cpp's hardcoded trailer (0x9A,0x00)
```

`init=0xFFFF` reproduces both of `dot.cpp`'s hardcoded, presumably
sniffed-from-a-real-device trailers exactly. `init=0x0000` (what
`timemore_new.cpp` actually uses) does not. This is a reasonably strong signal
that `timemore_new.cpp`'s CRC function has the wrong init value and would
produce invalid CRCs for any newly-constructed command — but "reasonably
strong signal from cross-referencing two secondhand implementations" is not
the same as "verified against the scale," hence no code change yet.

## Finding 3: dot.cpp's two hardcoded frames appear to be mislabeled

Decoding `dot.cpp`'s hardcoded bytes against Beanconqueror's
`opcode`/`cmdId` scheme (`buildCommand(opcode, cmdId, payload)`):

- `dot.cpp`'s `HANDSHAKE_CMD` = `opcode=0x03, cmdId=0x0D, payload=[]` — this
  is exactly Beanconqueror's **tare** frame: `buildCommand(0x03, 0x0d, [])`.
- `dot.cpp`'s `TARE_CMD` = `opcode=0x02, cmdId=0x04` — does not correspond to
  any command Beanconqueror sends (Beanconqueror's tare is `0x03/0x0D`, not
  `0x02/0x04`).

If Beanconqueror's mapping is right, the practical implication is that
`dot.cpp::connect()` calls `sendHandshake()` immediately after subscribing,
which under this reading is actually sending a **real tare command** on every
successful connect — i.e., connecting or reconnecting to the scale would
silently zero it. That would be a bug worth fixing independent of Basic 3.0
Link, but it rests entirely on trusting Beanconqueror's opcode/cmdId
semantics, which is exactly what we're not doing yet.

## Finding 4: Beanconqueror's connect handshake differs from both C++ ports

Beanconqueror's `connect()` (identical in Dot and Basic3):

```
attachNotification (subscribe FFF1)
sleep 500ms
setWeightUnitToGram   -> buildCommand(0x03, 0x06, [0x00])
sleep 200ms
setCurrentModeToStandard -> buildCommand(0x03, 0x08, [0x01, 0x00])
```

Neither `dot.cpp` (subscribes, then sends the frame described in Finding 3 as
`sendHandshake()`) nor `timemore_new.cpp` (subscribes, then sends 6 query
codes: `19, 8, 5, 2, 6, 12`, 160ms apart) matches this sequence.

## Finding 5: weight/battery byte offsets agree; one possible gap

- Weight: all three sources read a big-endian int32 at payload bytes `[0..3]`
  (i.e. `dataBuffer[6..9]` once the 6-byte frame header is skipped),
  `/10.0` for grams. Consistent everywhere.
- Battery: `dot.cpp` and Beanconqueror agree — `cmdId=0x05`, battery percent
  at payload byte `[1]` (`dataBuffer[7]`). `timemore_new.cpp` doesn't parse
  battery at all (and its base class, `RemoteScales` in the gaggiuino repo,
  has no battery field to store it in even if it wanted to).
- Possible gap: Beanconqueror treats a weight notification as valid under
  **either** `opcode 0x01` or `opcode 0x02` (both with `cmdId 0x01`). Both
  C++ drivers only check `opcode/cls == 0x01`. Unconfirmed whether the real
  scale ever emits weight under `0x02` — worth watching for during hardware
  testing.

## Finding 6: timer control commands are known but unimplemented

Beanconqueror's `setTimer()`: `buildCommand(0x03, 0x02, [state])` where
`state` is `0x01` (start), `0x02` (stop), `0x03` (reset). Neither `dot.cpp`
nor `timemore_new.cpp` implements scale timer control today (`dot.h` doesn't
even declare `startTimer`/`stopTimer`/`resetTimer` overrides). Not a bug,
just an unexploited capability if it turns out to be worth adding.

## Open questions for when the hardware arrives

1. What device name does the Basic 3.0 Link actually advertise? (Confirms or
   denies whether widening `TimemoreDotScalesPlugin::handles()` is even the
   right shape of fix.)
2. Does the CRC16 need `init=0xFFFF` (as re-derived from `dot.cpp`'s hardcoded
   trailers) or something else — test by sending a command with a
   freshly-computed CRC and see if the scale accepts it.
3. Is `dot.cpp`'s `HANDSHAKE_CMD` really a tare (Finding 3)? Test by watching
   the displayed weight on the scale immediately after connecting.
4. Does Beanconqueror's shorter handshake (unit-to-gram + mode-to-standard)
   work, or is the fuller query sequence in `timemore_new.cpp` actually
   required for something (e.g. to unlock notifications at all)?
5. Does weight ever arrive under `opcode 0x02` in addition to `0x01`?
6. Do the timer commands (Finding 6) actually work if sent?

## Hardware confirmed (2026-09-19)

The scale arrived and was bench-tested end to end against the real
`esp-arduino-ble-scales` driver stack, using this repo's `ble_scale_tester`
program on an M5Stack AtomS3R. Findings below supersede the "best guess"
framing above where they overlap.

### Advertised identity

- Advertises as **`Basic3 Link`** — confirmed via `bleak` on macOS and via
  the AtomS3R's own NimBLE scan log. Not `dot`/`tes017`/anything matching the
  old `TimemoreDotScalesPlugin::handles()` prefix check.
- GATT Device Information service reports Manufacturer=`TIMEMORE`,
  Model=`TES016`, Firmware=`v1.0.3`.
- Confirms **Finding 1**: it exposes exactly the Dot service/characteristic
  layout — `FFF0` (service), `FFF1` notify, `FFF2` write-without-response.
  No new driver class needed, only a wider name match.
- One thing not previously documented: it also exposes a second, unknown
  vendor service `5833ff01-9b8b-5191-6142-22a4536ef123` with a write char
  (`5833ff02`) and a notify char (`5833ff03`) — purpose unconfirmed, possibly
  OTA/firmware-update or an extended vendor channel. Not touched by the
  Dot driver and not needed for weight/tare/timer.

### The scale needed an explicit factory reset + mode switch

Out of the box, neither Bean Conqueror, Timemore's own iOS app, nor a plain
macOS `bleak` GATT connect could talk to it — the device advertised (visible
in generic scanners) but refused/timed out on every GATT connection attempt,
from three independent BLE stacks. A **factory reset**, followed by moving
the power switch to **M** and holding it 5s, fixed this: the scale then
advertised as `Basic3 Link` (previously it advertised with **no local
name at all**, which is also why the phone apps' name-based matching never
found it) and connections started succeeding. This turned out to be
scale-side state, not a bug in any of the three clients that failed against
it.

### Two real bugs found and fixed in `esp-arduino-ble-scales` (this repo's fork)

1. **`dot.h`: `TimemoreDotScalesPlugin::handles()`** only matched a literal
   `"TIMEMORE_Dot"` name prefix. Widened to also match (case-insensitive)
   `basic3`, `basic 3`, or `timemore`+`basic`, mirroring Beanconqueror's
   `TimemoreBasicScale.test()`. Confirmed this is enough — no other Dot code
   needed to change for weight streaming to work over the real Basic 3.0
   Link.
2. **`remote_scales.cpp`: `RemoteScalesScanner::onResult()`** had a dedupe
   race, unrelated to any specific scale driver. It marked a BLE address as
   "already seen" (LRU cache) on the **first** `onResult()` callback for that
   address, before checking whether any plugin's `handles()` matched. Many
   peripherals (this scale included, apparently) send their local name in a
   separate scan-response packet rather than the primary advertisement; the
   first callback for such a device arrives nameless, fails
   `containsPluginForDevice()`, but still gets cached as "seen" — silently
   blacklisting that address for the rest of the scan session even once the
   scan-response packet with the real name arrives on a later callback for
   the same `NimBLEAdvertisedDevice`. Fixed by only inserting into the LRU
   cache once a plugin has actually matched. Reproduced this in the wild:
   one test session scanned continuously for 45s, logged hundreds of
   `Duplicate; updated` events at the NimBLE layer for the scale's address,
   and never once surfaced a `[main] Found 'Basic3 Link'` line — until the
   fix went in.

Also needed, in `ble_scale_tester`'s own `main.cpp` (not the driver): an
explicit `NimBLEDevice::init()`/`setPower()`/`setMTU(256)` call in `setup()`.
GaggiMate's `BLEScalePlugin` never calls this itself — it relies on
`BleClientTransport::init()` (`gaggimate/lib/NanoPbComm/src/ble/BleClientTransport.cpp`)
having already brought up the NimBLE stack for the display↔controller link
first. This standalone tester has no such transport, so without an explicit
init call the scanner silently saw zero advertisements of anything, from any
device.

### Live weight streaming confirmed working

With both fixes in place, `ble_scale_tester` discovers `Basic3 Link`,
connects, performs the handshake, and streams accurate weight: a clean
put/settle/remove cycle (cup, 95.7g) tracked smoothly
0 → 1.7 → 16.2 → 37.9 → 80.1 → 96.4 → settling at 95.7 → back down to a
stable 0.00g, matching the scale's own display throughout.

### Still open

- **Unhandled frames** seen post-connect: `cls=03 type=0D len=1` (the
  handshake response — see Finding 3's tare/handshake mislabeling theory,
  still unconfirmed) and `cls=01 type=04 len=5` (likely timer or flow-rate
  data; not needed since GaggiMate computes flow rate itself rather than
  reading it from the scale).
- **One observed stuck-weight episode**: during a session with rapid
  weight swings (up to ~20g back and forth, including negative dips), the
  reported weight froze at exactly `-16.00 g` for roughly 80 consecutive
  notifications while the scale's own display showed `0.0`. Not reproduced
  on a later, gentler put/remove test, so root cause is still unknown — raw
  notification bytes were not captured during the actual stuck episode
  (only during the later, clean repro attempt). Worth another pass with the
  `[raw]` hex-dump instrumentation left in for that purpose if it recurs
  (temporarily re-added to `dot.cpp`'s `notifyCallback()` during this
  session, then reverted since it didn't catch a repeat).
- Open questions 2–4 and 6 from the original research (CRC16 init value,
  whether the handshake frame is really a tare, whether Beanconqueror's
  shorter handshake sequence is sufficient — it appears to be, since
  `dot.cpp` already only sends the short sequence and weight streams fine —
  and whether timer commands work) remain unverified; only weight streaming
  and basic connect/reconnect were exercised so far, not `tare`/`start`/
  `stop`/`reset`.

## Next steps

- Try to force-reproduce the stuck-weight bug (rapid oscillation, or a
  `tare` command mid-swing) with the raw hex-dump instrumentation active.
- Exercise `tare`/`start`/`stop`/`reset` via `ble_scale_tester`'s serial
  commands against the real scale.
- Raise a PR upstream for the `dot.h` name-match widening and the
  `remote_scales.cpp` scanner dedupe fix.
