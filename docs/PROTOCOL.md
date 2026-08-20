# Familybox wire protocol v1

All requests carry `Authorization: Bearer <FAMILYBOX_TOKEN>`.
Transport is HTTPS in production (via Cloudflare Tunnel / Tailscale);
plain HTTP is allowed only on localhost during development.

Base path: `/api/v1`

## Design notes

**Photos are shipped as raw RGB565, not JPEG.** The relay does the decode,
EXIF-rotate, cover-crop and pixel packing. The device receives exactly
368 x 448 x 2 = 329,728 bytes and pushes them straight at the panel.
No JPEG decoder in the firmware, no PSRAM decode scratch space, no
progressive-JPEG edge cases. Bandwidth is irrelevant on a home LAN.

**Audio is 16 kHz mono 16-bit PCM WAV**, which is what the ES8311 codec
wants natively in both directions. No codec in the firmware either.
~32 KB/sec, so a 30-second note is under 1 MB.

Byte order for RGB565 is little-endian, which is what LVGL on the ESP32 reads
natively. If colours come out wrong on
hardware, flip `RGB565_BIG_ENDIAN` in the relay config rather than
byte-swapping on the device.

## Endpoints

### POST /api/v1/send
Called by the iOS Shortcut. `multipart/form-data`:

| field   | required | notes                                        |
|---------|----------|----------------------------------------------|
| photo   | no       | HEIC/JPEG/PNG. Cover-cropped to 368x448.     |
| audio   | no       | Any ffmpeg-readable. Transcoded to 16k mono. |
| sender  | no       | Display name, default "dad"                  |

At least one of photo/audio must be present.
Returns `201 {"id": 42, "ts": 1755...}`.

### GET /api/v1/inbox?since=<id>
Called by the device every ~20s. Returns messages with id > since:

```json
{"messages": [
  {"id": 42, "ts": 1755500000, "sender": "dad",
   "photo": {"path": "/api/v1/media/42/photo.rgb565", "bytes": 329728},
   "audio": {"path": "/api/v1/media/42/audio.wav", "bytes": 480044,
             "ms": 15000}}
], "latest": 42}
```

`photo` or `audio` may be null. `latest` lets the device store a single
watermark in NVS.

### GET /api/v1/media/{id}/photo.rgb565
### GET /api/v1/media/{id}/audio.wav
Raw bytes. Supports `Range` so the device can stream audio in chunks
rather than buffering a whole file in PSRAM.

### POST /api/v1/reply
Called by the device. Body is `audio/wav` (16k mono), raw, not multipart --
multipart encoding on an MCU is needless work. Optional header
`X-Duration-Ms`. Relay transcodes to m4a and pushes it to the phone.
Returns `201 {"id": 7}`.

### GET /api/v1/health
Unauthenticated. `{"ok": true, "messages": N}` for the device to probe
connectivity before attempting a poll.
