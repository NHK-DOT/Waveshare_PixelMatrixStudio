# Waveshare PixelMatrixStudio / MatrixWithTerm

A browser-first ESP32 HUB75 64x64 LED matrix tool for Waveshare RGB-Matrix-P2-64x64-B with ESP32. It converts pictures/GIFs to PMX content and uploads PMX to the ESP32 over HTTPS.

The current primary workflow is browser based: the ESP32 serves a local HTTPS page, the browser converts images or GIFs into PMX content, and the browser uploads that PMX to the ESP32 over HTTPS. The secure origin enables browser APIs such as `ImageDecoder`. The web path does not use serial or COM-port burning.

## What is included

- `matrix_idf/` - ESP-IDF firmware for the ESP32 HUB75 player.
- `matrix_idf/main/web_index.html` - embedded browser editor and HTTPS uploader.
- `PixelMatrixStudio/` - Windows desktop editor/exporter kept as an offline fallback workflow.
- `pmxs/` - sample PMX display-content files.
- `image/` - sample source images/GIFs.
- `docs/` - operating notes and full pipeline documentation.

## Main workflow

1. Build and flash the ESP32 firmware once from `matrix_idf`.
2. Connect to the ESP32 network and open `https://192.168.1.218/`.
3. Import a PNG/JPG/BMP/WEBP/GIF or an existing PMX file in the browser.
4. Crop, size, position, choose sampling, and tune brightness/contrast/saturation.
5. Preview the 64x64 output, export PNG/GIF/PMX if needed, then click `HTTPS 上传到 Flash`.
6. The ESP32 pauses playback, erases the `assets` partition, writes the PMX, reloads content from Flash, and resumes playback.

See [docs/WORKFLOW.md](docs/WORKFLOW.md) for the full pipeline and [docs/WEB_TOOL.md](docs/WEB_TOOL.md) for browser operation.

## Build firmware

Copy the Wi-Fi template once and edit the local credentials before building:

```powershell
cd C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf\main
Copy-Item wifi_config.example.h wifi_config.h
notepad wifi_config.h
..\tools\generate_https_cert.ps1 -IpAddress 192.168.1.218
```

The generated `https_server.crt` and `https_server.key` are local build inputs
and are ignored by Git. Install `https_server.crt` into the client machine's
trusted root store, then fully restart Chrome/Edge before opening
`https://192.168.1.218/`.

```powershell
cd C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf

$env:IDF_PATH='D:\espidf\v6.0.2\esp-idf'
$env:IDF_TOOLS_PATH='D:\espidf\v6.0.2\.espressif'
$env:IDF_PYTHON_ENV_PATH='D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env'
$env:ESP_IDF_VERSION='6.0.2'
$env:PATH='D:\espidf\v6.0.2\.espressif\tools\cmake\4.0.3\bin;D:\espidf\v6.0.2\.espressif\tools\ninja\1.12.1;D:\espidf\v6.0.2\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env\Scripts;' + $env:PATH

& 'D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe' 'D:\espidf\v6.0.2\esp-idf\tools\idf.py' build
```

Flash the firmware from ESP-IDF or VS Code ESP-IDF extension. After the first firmware flash, normal content replacement should use the web HTTPS uploader.

## Desktop fallback

`PixelMatrixStudio.exe` still provides the PC editor/exporter path and can launch `matrix_idf/flash_pmx.ps1` to write a PMX to the `assets` partition over COM20. Treat this as a backup/offline path. The web page does not call this script and does not depend on serial.

## Limits

- Display content partition: `assets`, offset `0x110000`, size `0x2F0000`.
- PMX format: `PMX1`, RGB332, 64x64 pixels, one delay/offset entry per frame.
- Browser GIF multi-frame import uses `ImageDecoder` on the trusted HTTPS page; otherwise the browser falls back to static image decoding.
- The web page is embedded into firmware, so larger UI changes increase app binary size.
