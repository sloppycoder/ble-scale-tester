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

## Non-goal for now

No code changes have been made in `../esp-arduino-ble-scales`,
`../references/esp-arduino-ble-scales`, or `../gaggimate` as part of this
research. This file is purely notes to pick back up once the scale is in
hand.
