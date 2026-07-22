# PMX Content Workflow

The ESP32 firmware occupies the factory application partition. Display content
uses a separate `assets` partition at offset `0x110000`, size `0x2F0000`.

## Primary path: browser HTTP upload

1. Flash the `matrix_idf` firmware once.
2. Open the ESP32 web page in a browser.
3. Import an image, GIF, WEBP, or existing PMX file.
4. Adjust crop, output size, position, sampling, brightness, contrast, and
   saturation.
5. Preview the 64x64 output.
6. Click `HTTP 上传到 Flash`.

The browser sends PMX bytes to the ESP32 `/upload` endpoint. The ESP32 then
validates the PMX file and writes it to the `assets` partition itself. This web
flow does not use serial, COM20, or `flash_pmx.ps1`.

## PMX format

The PMX1 file stores RGB332 pixels. A static image contains one 4096-byte frame.
GIF frames retain their individual source delay in milliseconds when available.

## First firmware flash

Build and flash `matrix_idf` from ESP-IDF or the ESP-IDF VS Code extension.
This writes the application and the partition table. Until a PMX file is
written, the panel shows its built-in RGB test pattern.

## Desktop fallback

The Windows desktop tool can still export PMX and use the serial flasher:

```powershell
.\flash_pmx.ps1 -ContentPath "C:\path\to\content.pmx" -Port COM20
```

Use this only as a backup/offline path. It updates only the `assets` partition
and does not rebuild or overwrite the HUB75 player firmware.
