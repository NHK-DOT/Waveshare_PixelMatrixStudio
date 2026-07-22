# PMX 内容烧录说明

当前推荐方式是网页 HTTP 上传：网页端依赖 ESP32 自己提供的 HTTP 服务，不走串口、不调用 COM20、不使用 Web Serial。

## 推荐流程：网页 HTTP 上传

1. 先把 `matrix_idf` 固件烧录到 ESP32。
2. 连接到本机 `wifi_config.h` 中配置的 ESP32 网络；仓库里只提交无密码的 `wifi_config.example.h` 模板。
3. 在浏览器打开 ESP32 的地址。
4. 导入图片、GIF、WEBP 或已有 PMX。
5. 调整裁剪、尺寸、位置、采样、亮度、对比度和饱和度。
6. 点击 `HTTP 上传到 Flash`。

上传时浏览器会把 PMX 文件作为 HTTP 请求体发送到 `/upload`。ESP32 固件收到后会校验 PMX 头、暂停播放、擦写 `assets` 分区、写入新 PMX，然后从 Flash 重新加载播放。

## 网页端能做什么

- 导入图片、GIF、WEBP 和 PMX。
- 拖动裁剪框，或按比例重新画裁剪区域。
- 设置输出宽高和 X/Y 位置。
- 选择近邻采样或多点采样。
- 调整亮度、对比度、饱和度。
- 预览 64x64 LED 效果。
- 导出 PNG、GIF、PMX。
- 通过 HTTP 把 PMX 上传到 ESP32 Flash。

## 桌面 EXE 备份流程

`PixelMatrixStudio.exe` 仍保留串口备份流程。只有在网页不可用、需要离线烧录时才使用：

```powershell
cd C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
.\flash_pmx.ps1 -ContentPath "C:\路径\你的动画.pmx" -Port COM20
```

这条路径会通过 `flash_pmx.ps1` 写入 `assets` 内容分区。它不会覆盖 ESP32 播放固件，但需要串口可用。

## 常见问题

- 网页上传失败：确认打开的是 ESP32 页面，不是本地 `web_index.html` 文件。
- GIF 只能预览一帧：换用支持 `ImageDecoder` 的 Chrome/Edge，或用桌面 EXE 导出 PMX。
- 上传后仍是旧内容：刷新网页后再上传一次；固件会在上传完成后通过 generation 计数重新加载。
- 文件过大：减少 GIF 帧数、缩短动画，或降低内容复杂度。PMX 最大不能超过 `assets` 分区容量。
- COM20 被占用：这只影响桌面 EXE/PowerShell 备份流程，不影响网页 HTTP 上传。
