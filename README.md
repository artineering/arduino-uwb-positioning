# Arduino UWB Positioning

UWB real-time location system (RTLS) firmware for **4 anchors x 12 tags** using multicast Two-Way Ranging (TWR) sessions.

## Hardware

| Device | Board | UWB Module | Role |
|--------|-------|------------|------|
| Anchor | Arduino Portenta C33 + UWB Shield | Truesense DCU150 (SR150) | Controller (one-to-many) |
| Tag | Arduino Stella | Truesense DCU040 (nRF52840) | Responder (controlee) |

## Sketches

| File | Description |
|------|-------------|
| `UWB_MulticastAnchor.ino` | Anchor firmware — runs 2 multicast sessions (6 tags each), prints range measurements to Serial |
| `UWB_MulticastTag.ino` | Tag firmware — joins 4 sessions (one per anchor), broadcasts raw distances via BLE |

## Addressing Scheme

- **Anchor MAC:** `{0xA0, ANCHOR_ID}` where ANCHOR_ID is 0-3
- **Tag MAC:** `{0x00, TAG_ID}` where TAG_ID is 1-12
- **Session ID:** `ANCHOR_ID * 100 + GROUP_INDEX + 1`

Tags are split into two groups:
- Group 0: Tags 1-6
- Group 1: Tags 7-12

## BLE Distance Advertising

Each tag broadcasts raw UWB distance measurements over BLE advertising (no pairing required). Position computation is deferred to the receiver so the tag firmware stays independent of anchor geometry.

- **Local name:** `UWB-T<id>` (e.g., `UWB-T1`)
- **Manufacturer data (11 bytes):**

| Byte | Content |
|------|---------|
| 0-1 | Company ID `0xFFFF` (BLE SIG test/dev, little-endian) |
| 2 | TAG_ID |
| 3-4 | Distance to A0 in cm (`uint16`, little-endian, `0xFFFF` = invalid) |
| 5-6 | Distance to A1 in cm (`uint16`, little-endian, `0xFFFF` = invalid) |
| 7-8 | Distance to A2 in cm (`uint16`, little-endian, `0xFFFF` = invalid) |
| 9-10 | Distance to A3 in cm (`uint16`, little-endian, `0xFFFF` = invalid) |

The advertisement is only updated when any distance value changes to minimize BLE churn. Distances that are stale (no update for 2.5 s) are reported as `0xFFFF`.

## Configuration

### Anchor

Set `ANCHOR_ID` (0-3) in `UWB_MulticastAnchor.ino`:

```cpp
#define ANCHOR_ID  0   // 0, 1, 2, or 3
```

### Tag

Set `TAG_ID` (1-12) in `UWB_MulticastTag.ino`:

```cpp
#define TAG_ID  1   // 1 through 12
```

## Flashing

1. Flash each Portenta C33 with `UWB_MulticastAnchor.ino`, changing `ANCHOR_ID` for each (0-3)
2. Flash each Stella with `UWB_MulticastTag.ino`, changing `TAG_ID` for each (1-12)
3. Power on **anchors first**, then tags — anchors drive the ranging schedule

## Session Budget

- 2 sessions per anchor (limit: 5 on SR150)
- 4 sessions per tag (limit: 5 on DCU040)

## License

Apache 2.0 — see [LICENSE](LICENSE).
