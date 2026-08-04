# AiM `aimd` MP4 notes

This documents the portions of the format used by the reader. AiM does not appear to publish the embedded-track binary specification publicly; field names below distinguish observed framing from inferred meaning.

## MP4 layer

The telemetry track uses:

- handler type `meta` (handler name observed as `MetaAimHandler`)
- sample-entry FourCC `aimd`
- a millisecond MP4 timescale in the examined recording
- approximately one MP4 sample per 100 ms

The reader identifies the track from `stsd`'s `aimd` sample entry. It supports normal and extended-size MP4 boxes, `stco` and `co64`, constant or per-sample `stsz`, multi-entry `stsc`, and run-length `stts` timing. It does not assume a track index or contiguous data track.

## AiM packet envelope

Every observed MP4 sample begins with:

| Offset | Encoding | Meaning |
|---:|---|---|
| 0 | `u16be` | bytes following this field |
| 2 | byte | observed `0x40` |
| 3 | 3 bytes | reserved/zero in the examined file |
| 6 | ASCII | `amv0` signature |

The first packet additionally contains the observed stream-version suffix `s1` and a configuration/schema section. Data packets carry timestamped scalar records and tagged aggregate records.

## Tagged schema blocks

Blocks use this observed header:

```text
'<' 'h' TAG[3] 0x00 payload_length:u32le 0x01 '>' payload...
```

The parser searches the first packet for every `CHS` block rather than assuming count, order, or byte position. Scalar channel definitions currently use these fields:

| CHS payload offset | Encoding | Use |
|---:|---|---|
| 0 | `u32le` | record ID used by data packets |
| 6 | `u16le` | observed source/logger channel ID (currently not exposed) |
| 20 | `u32le` | scalar encoding kind (`12` is the observed integer timer encoding) |
| 24 | 8-byte C string | short code |
| 32 | 32-byte C string | displayed channel name |
| 68 | `u32le` | observed aggregate-buffer offset; not used for scalar decoding |
| 72 | `u32le` | encoded value width |
| 80 | `u32le` | schema class (`0x1003` is the observed unsigned protocol clock) |

Unknown fields are not treated as calibration or units. This avoids reporting guessed metadata as declared metadata.

## Scalar sample records

Scalar updates are self-delimiting:

```text
'(' 'S' timestamp_ms:u32le record_id:u16le value[declared_width] ')'
```

The record ID is joined to the `CHS` schema. Timestamps are normalized to the first scalar timestamp. Each channel's native period is the modal positive timestamp delta; a gap greater than twice that period starts a new telemetry chunk.

Widths of one and four bytes are currently exposed. One-byte records are unsigned status values. Four-byte ordinary channels use IEEE-754 little-endian values. Schema class `0x1003` selects an unsigned protocol clock, while encoding kind `12` selects signed integer timer ticks. These choices come from `CHS` fields rather than channel names.

## GPS aggregate

AiM's [GPS09c product documentation](https://www.aim-sportline.com/en/products/gps09c/index.htm) identifies its GPS channels and states that they are available at 25 Hz. Each observed `GPS0` definition declares a width of 56 bytes, and data arrives in `<hGPS>` blocks of that declared width.

| GPS payload offset | Encoding | Export | Unit |
|---:|---|---|---|
| 0 | `u32le` | logger timestamp | ms |
| 4 | `u32le` | GPS iTOW | ms |
| 12 | `u16le` | GPS week | count |
| 16, 20, 24 | `i32le` | ECEF X/Y/Z position | cm |
| 28 | `u32le` | position accuracy | cm |
| 32, 36, 40 | `i32le` | ECEF X/Y/Z velocity | cm/s |
| 44 | `u32le` | speed accuracy | cm/s |
| 48, high byte | `u8` | satellite count | count |
| 48, low 24 bits | unsigned | dilution of precision | 0.01 ratio |
| 52 | `u32le` | fix/status flags | raw |

The reader converts ECEF position to WGS84 latitude, longitude and ellipsoid altitude, and ECEF velocity to speed and true heading. The first packet resolves to `43.797816°, -87.989895°, 291.74 m`, at Road America. Velocity magnitude reaches `72.46 m/s` and agrees with the independent wheel-speed channel after converting that channel from km/h. These checks are included to distinguish decoded fields from plausible-looking guesses.

## Lap packet

`LapPk` declares a 20-byte aggregate width. All five available recordings contain the definition but zero `LapPk` data blocks or scalar records. Their useful lap information is instead recorded through `Lap_Number_001`, `Current_Lap_Time`, `Delta_Lap_Time`, `Previous_LT` and `Ref_Lap_Time`. No 20-byte layout is claimed without a real payload.

## Compatibility policy

- Read ISO-BMFF and AiM lengths, IDs, names, widths, and timestamps from the file.
- Do not depend on the observed 40-channel order or on MP4 track 3.
- Accept additional unknown tagged blocks.
- Reject malformed sample tables, out-of-file offsets, missing `amv0` packet signatures, duplicate record IDs, and MP4 files without an `aimd` sample entry.
- Assign units to GPS fields only where the payload scale is validated; leave ordinary scalar units unknown until their CHS quantity codes are decoded reliably.
