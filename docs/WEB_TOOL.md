# Web PMX Tool

This is the recommended content workflow. It uses the ESP32 HTTPS page and
upload only; it does not burn through serial, COM20, or Web Serial.

## Open the page

1. Flash the firmware once.
2. Connect the computer or phone to the ESP32 network configured in the local
   `matrix_idf/main/wifi_config.h` file. Use
   `matrix_idf/main/wifi_config.example.h` as the committed template.
3. Open the ESP32 address in a browser.
4. Confirm the page status can ping the device.

## Import content

- Image input: PNG, JPG, BMP, WEBP, and GIF.
- PMX input: existing `.pmx` files can be loaded for preview and direct upload.
- GIF multi-frame decoding depends on browser `ImageDecoder` support. Open the
  ESP32 page through HTTPS, for example `https://192.168.1.218/`, and accept or
  trust the self-signed certificate once. If the browser still shows one frame,
  use Chrome/Edge or export PMX from the desktop tool as a fallback.

## Edit output

- Crop ratio accepts values such as `1:1`, `16:9`, or `4:3`.
- Drag inside the crop rectangle to move it.
- Drag outside the crop rectangle to draw a new crop.
- Width/height control how large the source content is rendered on the 64x64
  canvas.
- X/Y position controls where the output is placed on the canvas.
- Sampling mode chooses nearest-neighbor or 4-point/multipoint downsampling.
- Brightness, contrast, and saturation match the desktop tool's adjustment
  model.

## Export locally

- `导出 PNG` saves the current 64x64 preview frame.
- `导出 GIF` saves the generated animation preview.
- `导出 PMX` saves the exact PMX payload that can later be uploaded or burned
  by another tool.

## Upload to ESP32

Open the ESP32 HTTPS page, accept/trust the self-signed certificate once, then
click `HTTPS 上传到 Flash`.

The browser sends the PMX bytes with:

```http
POST /upload
Content-Type: application/octet-stream
```

The ESP32 receives the HTTPS request, validates the PMX header, pauses playback,
erases the `assets` partition, writes the new PMX into Flash, increments the
content generation counter, and resumes playback from the new content.

This upload path is intentionally independent from the desktop serial flasher.
Do not add COM-port logic to the web page unless the product direction changes.

## Troubleshooting

- Page opens but upload fails: confirm the browser is using the ESP32 page, not a
  local file copy. Relative `/upload` only exists on the ESP32 HTTPS server.
- Import works but GIF is static: confirm the page is opened as
  `https://192.168.1.218/` and the self-signed certificate has been accepted;
  then use a browser with `ImageDecoder` support or generate PMX from
  `PixelMatrixStudio.exe`.
- Upload succeeds but old content remains: refresh the page and upload again;
  the firmware should reload when its generation counter changes.
- File too large: reduce GIF frame count/duration or use a smaller animation.
