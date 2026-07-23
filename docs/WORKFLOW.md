# MatrixWithTerm Workflow Pipeline

This document describes the full content pipeline from source media to HUB75
refresh. The browser HTTPS path is the primary path.

## 1. Source media

Input can start from:

- PNG/JPG/BMP/WEBP/GIF source images.
- Existing PMX files in `pmxs/`.
- PMX files exported by `PixelMatrixStudio.exe`.

The browser and desktop tools both convert source media into the same PMX
content format, so the firmware playback path stays identical no matter which
editor created the file.

## 2. Browser conversion

`matrix_idf/main/web_index.html` runs entirely in the browser. It provides:

- File import for images, GIFs, WEBP, and PMX.
- Crop ratio parsing and draggable crop selection.
- Output width/height and X/Y placement on a 64x64 canvas.
- Nearest-neighbor and multipoint sampling.
- Brightness, contrast, and saturation adjustment.
- 64x64 LED preview.
- PNG, GIF, and PMX export.
- HTTPS upload to the ESP32 `/upload` endpoint.

The page is embedded into firmware by `target_add_binary_data` in
`matrix_idf/main/CMakeLists.txt`. The root HTTPS handler serves those embedded
bytes directly.

## 3. PMX format

PMX is the display-content container used by both the web tool and the desktop
tool.

- Magic: `PMX1`.
- Version: `1`.
- Canvas: `64x64`.
- Pixel encoding: RGB332, one byte per LED pixel.
- Frame data: `4096` bytes per frame.
- Header contains frame count, directory offset, data offset, and total file
  size.
- Frame directory stores each frame delay and frame-data offset.

Static images become one-frame PMX files. GIFs become multi-frame PMX files
with source frame delays preserved where available.

## 4. HTTPS upload

The browser uploads PMX bytes to:

```http
POST /upload
```

Firmware handling in `matrix_idf/main/matrix_idf.c`:

1. Rejects empty or oversized requests.
2. Reads and validates the PMX header before Flash erase.
3. Marks HTTPS upload and assets update as active.
4. Stops playback while preserving the existing RAM allocation when it can hold the new PMX. The player reserves at least 26 frames before Wi-Fi/TLS starts, avoiding a large fragmented allocation after upload.
5. Pauses display output during Flash erase/write.
6. Erases only the `assets` data partition.
7. Streams the PMX body into Flash in small blocks.
8. Reads the new PMX into the reusable RAM cache before resuming playback. If cache allocation is unavailable, it falls back to Flash streaming.
9. Sends the HTTPS response, clears upload flags, and lets the player switch to the new PMX generation.

This is why the web page does not need serial. The ESP32 writes its own content
partition after receiving bytes over HTTPS.

## 5. Flash storage

`matrix_idf/partitions.csv` defines the content storage:

```csv
assets, data, 0x40, 0x110000, 0x2F0000,
```

The firmware application and the display content are separate. Normal PMX
replacement only touches `assets`; it does not rebuild or overwrite the player
firmware.

## 6. Playback

The playback loop reads PMX from `assets`, caches frame data in RAM when
possible, and renders frames to the HUB75 panel. It reserves 26 frame slots
before Wi-Fi/TLS startup and reuses that allocation for later uploads whenever
capacity permits. When `/upload` finishes, the generation counter changes, so
playback exits the old content loop and starts the new cached PMX. Flash
streaming remains a fallback for content that cannot fit in contiguous RAM.

If no valid PMX exists yet, the firmware falls back to its built-in display
pattern.

## 7. HUB75 refresh

The ESP32 firmware drives the 64x64 HUB75 matrix using the local parallel/I2S
matrix driver files under `matrix_idf/main/`. PMX playback produces the logical
64x64 frame buffer; the driver handles timed refresh output to the LED panel.

## 8. Desktop fallback path

`PixelMatrixStudio.exe` still exists for Windows-side editing and backup
burning:

1. Import source image/GIF.
2. Adjust crop, placement, sampling, and color.
3. Export PMX.
4. Optionally call `matrix_idf/flash_pmx.ps1` to write PMX to COM20.

This path is separate from the browser path. It is useful when the ESP32 web
page is unavailable, but it is not part of the browser upload flow.
