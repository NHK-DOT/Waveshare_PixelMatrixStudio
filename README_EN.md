# Waveshare PixelMatrixStudio / MatrixWithTerm

English documentation | [中文文档](README.md)

This project is a complete content-authoring, storage, and playback system for a classic ESP32 and one Waveshare `RGB-Matrix-P2-64x64-B` HUB75 LED matrix. It includes a browser editor, native ESP-IDF player firmware, an HTTPS upload service, the PMX animation container, and a Windows desktop fallback tool.

The primary workflow is browser-first: the browser reads an image or GIF, performs crop, scale, color adjustment, and RGB332 quantization locally, builds a PMX binary, and uploads it over LAN HTTPS into the ESP32's dedicated `assets` Flash partition. Normal animation replacement does not require a serial port or a firmware rebuild.

> Verified baseline: ESP-IDF 6.0.2, classic ESP32-D0WD-V3, 4 MiB Flash, and one 64x64 1/32-scan HUB75 panel. The firmware target is `esp32`, not `esp32s3`.

## Contents

- [Repository layout](#repository-layout)
- [Architecture and data flow](#architecture-and-data-flow)
- [Hardware and wiring](#hardware-and-wiring)
- [Flash layout and stored data](#flash-layout-and-stored-data)
- [PMX1 file format](#pmx1-file-format)
- [First deployment](#first-deployment)
- [Browser workflow](#browser-workflow)
- [How ImageDecoder works](#how-imagedecoder-works)
- [ESP32 HTTPS pipeline](#esp32-https-pipeline)
- [Playback, RAM cache, and flicker control](#playback-ram-cache-and-flicker-control)
- [Serial fallback](#serial-fallback)
- [Troubleshooting](#troubleshooting)
- [Security and limits](#security-and-limits)

## Repository layout

| Path | Purpose |
| --- | --- |
| `matrix_idf/` | ESP-IDF firmware, partition table, certificate generator, and serial PMX writer |
| `matrix_idf/main/matrix_idf.c` | HUB75 refresh, PMX validation/playback, Wi-Fi, HTTPS, and Flash upload logic |
| `matrix_idf/main/web_index.html` | Browser editor and HTTPS uploader embedded in the firmware image |
| `matrix_idf/main/wifi_config.example.h` | Wi-Fi template without real credentials |
| `matrix_idf/tools/generate_https_cert.ps1` | Generates a self-signed certificate and key for a selected IPv4 address |
| `matrix_idf/flash_pmx.ps1` | Serial fallback that writes only the `assets` partition |
| `PixelMatrixStudio/` | Native Windows editor/exporter retained as an offline fallback |
| `pmxs/` | Sample PMX content |
| `image/` | Sample source images and GIFs |
| `docs/` | Additional workflow and browser-tool notes |
| `ESP32/` | Earlier Arduino examples and third-party libraries, not the primary firmware |

Additional documents:

- [Hardware baseline and wiring](BOARD_AND_WIRING.md)
- [PMX content workflow](PMX_CONTENT_WORKFLOW.md)
- [End-to-end pipeline](docs/WORKFLOW.md)
- [Web tool operation](docs/WEB_TOOL.md)

## Architecture and data flow

```text
PNG/JPG/BMP/WEBP/GIF/PMX
            |
            v
Browser HTTPS page served by the ESP32
            |
            +-- GIF: File -> ArrayBuffer -> ImageDecoder -> VideoFrame/RGBA
            +-- Still image: createImageBitmap -> Canvas RGBA
            +-- PMX: validate and upload directly
            |
            v
Crop -> scale -> X/Y placement -> sampling -> brightness/contrast/saturation
            |
            v
64x64 RGBA frames -> RGB332 (one byte per pixel) -> PMX1
            |
            v
fetch(POST /upload, raw PMX bytes)
            |
        TLS/HTTPS :443
            |
            v
Validate header -> pause playback -> erase/write assets partition
            |
            v
Flash PMX -> RAM frame cache -> double frame buffer -> HUB75 GPIO scan
```

The desktop fallback produces the same PMX1 container, so both editors share the same firmware playback path.

## Hardware and wiring

### Verified baseline

- MCU: classic ESP32, 30-pin DevKit-style board, target `esp32`.
- USB bridge: CH340; the local example port is `COM20`.
- Panel: one 64x64, 1/32-scan HUB75 RGB matrix.
- Flash: 4 MiB.
- Network: 2.4 GHz Wi-Fi station mode; the firmware does not create an access point.

### HUB75 mapping

This table must match the macros in [matrix_idf.c](matrix_idf/main/matrix_idf.c).

| HUB75 | ESP32 GPIO | Function |
| --- | ---: | --- |
| R1 | 25 | Upper-half red data |
| G1 | 26 | Upper-half green data |
| B1 | 27 | Upper-half blue data |
| R2 | 14 | Lower-half red data |
| G2 | 18 | Lower-half green data |
| B2 | 13 | Lower-half blue data |
| A | 23 | Row address bit 0 |
| B | 22 | Row address bit 1 |
| C | 19 | Row address bit 2 |
| D | 17 | Row address bit 3 |
| E | 32 | Row address bit 4 for 1/32 scan |
| LAT / STB | 21 | Latch pulse |
| OE | 33 | Output enable, active low |
| CLK | 16 | Pixel shift clock |
| GND | GND | ESP32, level shifter, and panel PSU must share ground |

The mapping avoids common ESP32 bootstrap pins GPIO0, GPIO2, GPIO4, GPIO5, GPIO12, and GPIO15. GPIO34 through GPIO39 are input-only and must not drive HUB75 signals.

### Recommended physical path

```text
ESP32 GPIO
    |
    v
74AHCT245 / 74HCT245 level shifter (3.3 V -> 5 V)
    |
    v
HUB75 ribbon cable -> panel IN connector

Regulated external 5 V supply ---> panel 5 V
External supply GND -------------+-> panel GND
ESP32 GND -----------------------+
Level shifter GND ---------------+
```

### Power requirements

- Never power a 64x64 panel from the ESP32 board's 5 V pin.
- Use a regulated external 5 V supply rated for at least 3 A; high-brightness white content may require more headroom.
- All grounds must be connected.
- A `74AHCT245` or `74HCT245` is recommended. Direct 3.3 V signaling may work but is not a reliable deployment target.
- Connect the ribbon cable to panel `IN`, not `OUT`.
- Verify 5 V/GND polarity with a meter before first power-up.

### Orientation

The installed panel requires the logical image to be rotated counterclockwise by 90 degrees before entering the scan buffer. `panel_copy_display_frame()` implements this mapping. Change that function for another mounting orientation instead of rotating every source asset.

## Flash layout and stored data

The ESP32 uses 4 MiB Flash (`0x000000-0x3FFFFF`). The custom table is [partitions.csv](matrix_idf/partitions.csv).

| Address range | Size | Name | Type/subtype | Stored data | Writer |
| --- | ---: | --- | --- | --- | --- |
| `0x001000-0x007FFF` | 28 KiB region | Bootloader | boot | ESP-IDF second-stage bootloader | `idf.py flash` |
| `0x008000-0x008FFF` | 4 KiB region | Partition table | table | Binary generated from `partitions.csv` | `idf.py flash` |
| `0x009000-0x00EFFF` | 24 KiB | `nvs` | data/nvs | NVS and Wi-Fi/system runtime data; credentials are compiled into the app and Wi-Fi storage is configured as RAM | Firmware/NVS driver |
| `0x00F000-0x00FFFF` | 4 KiB | `phy_init` | data/phy | RF PHY initialization/calibration data | PHY/Wi-Fi driver |
| `0x010000-0x10FFFF` | 1 MiB | `factory` | app/factory | Program, embedded web page, HTTPS certificate/key, HUB75, networking, and player code | `idf.py flash` |
| `0x110000-0x3FFFFF` | 3008 KiB | `assets` | data/0x40 | One complete PMX1 file starting at partition offset zero | HTTPS `/upload` or `flash_pmx.ps1` |

Important consequences:

- `idf.py flash` writes the bootloader, partition table, and factory app but normally leaves `assets` unchanged.
- An old animation therefore survives an application reflash.
- Web upload erases only `assets`; it does not modify firmware, NVS, the partition table, or certificates.
- Changing `partitions.csv` requires reflashing the partition table and checking that the new layout does not overlap existing data.
- `assets` ends exactly at the 4 MiB boundary `0x400000`.

## PMX1 file format

PMX is the shared display-content protocol used by the browser, Windows tool, and ESP32 firmware. All integers are little-endian.

### 24-byte header

| Offset | Size | Field | Current meaning |
| ---: | ---: | --- | --- |
| `0` | 4 | `magic` | ASCII `PMX1` |
| `4` | 1 | `version` | `1` |
| `5` | 1 | `format` | `1`, RGB332 |
| `6` | 1 | `width` | `64` |
| `7` | 1 | `height` | `64` |
| `8` | 4 | `frame_count` | Number of frames |
| `12` | 4 | `directory_offset` | Frame-directory offset, normally `24` |
| `16` | 4 | `data_offset` | Pixel-data offset, normally `24 + frame_count * 8` |
| `20` | 4 | `file_size` | Total PMX length; must match the HTTPS body length |

### Frame directory: 8 bytes per frame

| Relative offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| `0` | 4 | `delay_ms` | Frame duration; firmware clamps to at least 20 ms |
| `4` | 4 | `data_offset` | Absolute PMX offset of this frame's 4096-byte pixel block |

### Pixels: 4096 bytes per frame

Each of the 64x64 pixels is one RGB332 byte:

```text
bits 7..5 = RRR
bits 4..2 = GGG
bits 1..0 = BB
```

```text
PMX size = 24 + frame_count * 8 + frame_count * 4096

1 frame  = 4,128 bytes
23 frames = 94,416 bytes
26 frames = 106,728 bytes
```

## First deployment

### 1. Install prerequisites

- Windows 10/11.
- ESP-IDF 6.0.2 with the `xtensa-esp-elf` toolchain.
- Optional VS Code ESP-IDF extension; command-line builds are the reproducible reference.
- Current Chrome or Edge for browser `ImageDecoder` support.
- OpenSSL, or Python with `cryptography`, for certificate generation.

### 2. Configure Wi-Fi

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf\main
Copy-Item wifi_config.example.h wifi_config.h
notepad wifi_config.h
```

```c
#define MATRIX_WIFI_SSID "YOUR_2_4G_WIFI_SSID"
#define MATRIX_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

`wifi_config.h` is ignored by Git. The current firmware requires at least WPA2-PSK and is not intended for 5-GHz-only, captive-portal, or WPA3-only networks.

### 3. Reserve the device IP

Examples use `192.168.1.218`. Create a DHCP reservation for the ESP32 MAC. The certificate Subject Alternative Name contains the IP, so regenerate the certificate if the address changes.

### 4. Generate the HTTPS certificate

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
.\tools\generate_https_cert.ps1 -IpAddress 192.168.1.218
```

Outputs:

- `main/https_server.crt`: self-signed server certificate.
- `main/https_server.key`: unencrypted private key; keep it private.

Both are local, Git-ignored build inputs. `target_add_binary_data()` embeds the page, certificate, and private key in the factory app image.

The browser can accept the self-signed warning manually. For controlled clients, installing the certificate into the trusted-root store and fully restarting Chrome/Edge is more stable. Never install the private key on the client or commit it.

### 5. Use one ESP-IDF environment

Do not mix the `C:\Espressif` and `D:\espidf` Python environments in one `build` directory.

```powershell
$env:IDF_TOOLS_PATH = 'D:\espidf\v6.0.2\.espressif'
$env:IDF_PATH = 'D:\espidf\v6.0.2\esp-idf'
$IdfPython = 'D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe'

. 'D:\espidf\v6.0.2\esp-idf\export.ps1'
```

If the active and configured Python paths differ, remove only the local build cache:

```powershell
& $IdfPython "$env:IDF_PATH\tools\idf.py" fullclean
```

### 6. Build

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
& $IdfPython "$env:IDF_PATH\tools\idf.py" build
```

| Output | Flash offset | Purpose |
| --- | ---: | --- |
| `build/bootloader/bootloader.bin` | `0x1000` | Bootloader |
| `build/partition_table/partition-table.bin` | `0x8000` | Partition table |
| `build/matrix_idf.bin` | `0x10000` | App, web page, certificate, and firmware logic |

### 7. First flash

```powershell
& $IdfPython "$env:IDF_PATH\tools\idf.py" -p COM20 flash monitor
```

Exit the monitor with `Ctrl+]`. A successful boot prints the app version, PMX loaded from `assets`, DHCP address, and HTTPS port 443. Existing PMX content remains after an app reflash by design.

## Browser workflow

1. Put the client and ESP32 on the same LAN.
2. Open `https://192.168.1.218/`.
3. Accept or trust the self-signed certificate.
4. After a firmware update, press `Ctrl+F5` to discard an old embedded page.
5. Select PNG, JPG, BMP, WEBP, GIF, or an existing PMX.
6. Verify that an animated GIF reports more than one frame.
7. Set a crop ratio such as `1:1`, `16:9`, or `4:3`.
8. Drag inside the crop rectangle to move it or outside it to create a new rectangle.
9. Set output width/height and X/Y position on the 64x64 canvas.
10. Choose nearest-neighbor or multipoint sampling and adjust brightness, contrast, and saturation.
11. Inspect the 64x64 preview.
12. Optionally export PNG, GIF, or PMX.
13. Click the HTTPS Flash upload command. The button and `/ping` polling pause during upload.
14. Confirm the actual PMX frame count/size and the RAM-cache or Flash-streaming response.

When loading an existing PMX, the page preserves its original bytes for direct upload/download. Preview is limited to 120 frames, but direct PMX upload retains the full file.

## How ImageDecoder works

### Purpose

`createImageBitmap()` is suitable for still images but usually exposes only one static image from a GIF. The WebCodecs `ImageDecoder` API provides GIF track metadata, total frame count, individual frames, and timing, which makes browser-side animated PMX generation possible.

### Secure context

The API should be available on the ESP32 HTTPS page:

```js
self.isSecureContext === true
typeof ImageDecoder === 'function'
```

Opening `web_index.html` directly from disk is not the supported flow and cannot reach the ESP32-relative `/upload` endpoint.

### Accepted input types

`ImageDecoderInit.data` accepts:

- `ArrayBuffer`.
- `ArrayBufferView`, such as `Uint8Array` or `DataView`.
- `ReadableStream`.

It does not directly accept a DOM `File`/`Blob`. The correct conversion is:

```js
const bytes = await file.arrayBuffer();
const decoder = new ImageDecoder({
  data: bytes,
  type: file.type || 'image/gif'
});
```

Passing `data: file` produces a type error requiring a `ReadableStream`, `ArrayBuffer`, or `ArrayBufferView`.

### Per-frame pipeline

```text
file input
    -> File
    -> File.arrayBuffer()
    -> ImageDecoder({data, type:'image/gif'})
    -> tracks.ready / selectedTrack.frameCount
    -> decode({frameIndex})
    -> VideoFrame
    -> drawImage to Canvas
    -> getImageData RGBA
    -> convert duration from microseconds to milliseconds, minimum 20 ms
    -> crop/scale/color adjustment
    -> RGB332
    -> PMX1
```

The browser limits import to 720 frames. Each `VideoFrame` and the decoder are explicitly closed after use. If `ImageDecoder` is absent or decoding fails, the page falls back to `createImageBitmap()`, which only guarantees a still frame.

Immediately after import, one `canvasFrames` preview frame is normal. `srcFrames` is the decoded source count, and `makePmx()` fully rerenders every source frame immediately before export or upload.

## ESP32 HTTPS pipeline

### Server startup

1. Initialize NVS, TCP/IP, and the default event loop.
2. Start 2.4 GHz Wi-Fi in station mode.
3. Wait for DHCP and `IP_EVENT_STA_GOT_IP`.
4. Pass the embedded certificate/key to `esp_https_server`.
5. Listen on TCP 443.

| Method | URI | Purpose |
| --- | --- | --- |
| `GET` | `/` | Serve embedded `web_index.html` |
| `GET` | `/ping` | Return upload state, PMX generation, and cached-frame count as JSON |
| `POST` | `/upload` | Receive the complete raw PMX body and write `assets` |

### Browser transmission

```text
canvasFrames -> makePmx() -> Uint8Array
    -> fetch('/upload', {method:'POST', body:pmx})
    -> browser TLS encryption -> TCP 443
```

`uploadBusy` disables duplicate uploads and suspends `/ping` while the transfer is active, reducing concurrent TLS memory pressure.

### Firmware receive/write sequence

1. Reject concurrent uploads.
2. Locate the `assets` data/0x40 partition.
3. Validate `Content-Length` against the 24-byte minimum and partition capacity.
4. Receive the 24-byte PMX header first.
5. Validate magic, version, format, dimensions, frame count, offsets, and total size.
6. Set upload/update flags and increment generation so playback leaves the old loop.
7. Wait for playback to stop, then pause display output.
8. Erase only the required 4-KiB-aligned range in `assets`.
9. Write the header, then receive/write the remaining body through a 1024-byte buffer.
10. Read the newly written frames back into the RAM cache.
11. Resume display, increment generation, and send the HTTPS response.
12. Clear the upload flag after the response, allowing new playback to start.

An interrupted transfer may leave an invalid PMX in Flash, but the old RAM cache is retained when possible until reset.

### TLS memory pressure

TLS needs certificate parsing, crypto state, socket buffers, and record buffers. Browsers may open several sessions for the page, `/ping`, preconnection, or additional tabs. On a no-PSRAM ESP32 this can fragment heap memory. Close duplicate tabs, avoid repeated clicks, and allow stale sessions to close if `mbedtls_ssl_setup` or handshake failures appear.

## Playback, RAM cache, and flicker control

### Boot sequence

1. Initialize HUB75 GPIO and show the built-in pattern.
2. Start the high-priority refresh task and PMX player task.
3. Load `assets` before Wi-Fi/TLS starts.
4. Reserve at least 26 contiguous frames (`106496` bytes).
5. Initialize Wi-Fi only after this cache is established.

### Cache limits

- Compile-time cache ceiling: 240 KiB, or 60 frames.
- Guaranteed early reserve: 26 frames.
- New PMX files within current capacity reuse the same allocation.
- Files above 26 frames attempt a larger allocation; failure falls back to Flash streaming.
- Files above 60 frames skip RAM caching and stream from Flash.

Flash streaming calls `esp_partition_read()` for every frame and can visibly contend with HUB75 refresh. RAM playback only copies 4096 bytes into the double frame buffer at frame boundaries.

### Double buffering and scan

- `s_framebuffers[2]` holds front/back 4096-byte scan frames.
- The player copies the next frame into the back buffer and switches `s_front_buffer`.
- The HUB75 refresh task runs on CPU1 and scans rows/PWM planes.
- The PMX player runs on CPU0 and advances according to frame delays.
- Output enable is disabled during Flash erase/write, producing one controlled blackout instead of ongoing corrupted refresh.

Expected successful upload log:

```text
PMX cached in RAM: 26 frames, 106496 bytes, capacity 26 frames (reused)
HTTPS Flash upload complete: 106728 bytes, 26 frames; RAM cache ready
Playing RAM PMX: 26 frames
```

## Serial fallback

When the web service is unavailable, export PMX from the Windows tool and write only `assets`:

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
.\flash_pmx.ps1 -ContentPath "C:\path\animation.pmx" -Port COM20
```

The script validates PMX1, RGB332, 64x64, declared length, and the 3008-KiB capacity before writing offset `0x110000`. It does not rebuild or overwrite the player application. Close ESP-IDF monitor and any other COM20 user first.

## Troubleshooting

### Old app version after flashing

If the boot log shows an old Git hash, the old `build/matrix_idf.bin` was flashed. Run `fullclean`, `build`, and `flash`, then verify the new `App version` line.

### C: and D: Python mismatch

Use the explicit D-drive Python to run `fullclean`, then rebuild. Do not alternate between two ESP-IDF installations in the same build directory.

### GIF imports as one frame

1. Select the original GIF, not a previously exported static PMX/PNG.
2. Open the page via HTTPS and press `Ctrl+F5`.
3. Confirm the import UI reports multiple frames.
4. Check `typeof ImageDecoder === 'function'`.
5. Ensure decoder input is `await file.arrayBuffer()`, not the `File` object.
6. `1 frame / 4128 bytes` in the browser means the failure happened before transmission.

### Flicker after multi-frame upload

Look for `RAM cache ready` and `Playing RAM PMX`. `RAM cache unavailable` or `Falling back to flash streaming` means the file exceeded available contiguous cache memory. Keeping animations at or below 26 frames is the most reliable configuration.

### TLS session errors

- Close duplicate device tabs.
- Wait for stale connections to release.
- Do not click upload repeatedly.
- Ensure the certificate IP SAN matches the current ESP32 IP.
- An occasional failed browser preconnection does not mean the successful main session failed.

### Page is unreachable

- Confirm client and ESP32 are on the same subnet.
- The serial log must show `Server listening on port 443`.
- Use `https://`, not `http://`.
- Check router client isolation, firewall rules, and DHCP reservation.

### Firmware updated but page is old

The page is embedded in `matrix_idf.bin`. Reflash the application, then press `Ctrl+F5` or clear site data for the IP.

## Security and limits

- The LAN service has no user authentication. Any client that can reach port 443 can replace `assets`.
- The self-signed private key is embedded in the app image; this is not a production public-Internet identity design.
- `wifi_config.h`, certificate, and key are Git-ignored and must never contain committed secrets.
- There is no OTA update path; application updates still use serial flashing.
- Browser decoding is capped at 720 frames; guaranteed early RAM reserve is 26 frames.
- RGB332 provides 3 red bits, 3 green bits, and 2 blue bits.
- The app partition is 1 MiB, and the embedded page contributes to app size.
- The current firmware targets one 64x64 1/32-scan panel with a fixed GPIO map. Chaining, other scan rates, or other panel sizes require firmware changes.

## Typical daily workflow

First deployment only: wire hardware, configure Wi-Fi, generate the certificate, build, and flash over serial.

For every later content update:

```text
Open https://192.168.1.218/
    -> import image/GIF
    -> verify GIF frame count
    -> crop and preview
    -> HTTPS upload
    -> ESP32 writes assets
    -> RAM-cached playback
```

The daily path does not require COM20, VS Code, or firmware compilation, and it does not overwrite the player firmware.
