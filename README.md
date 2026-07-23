# Waveshare PixelMatrixStudio / MatrixWithTerm

[English documentation](README_EN.md) | 中文文档

这是一个面向经典 ESP32 与单块 Waveshare `RGB-Matrix-P2-64x64-B` HUB75 LED 矩阵的完整内容制作、存储和播放项目。它包含浏览器编辑器、ESP-IDF 播放固件、HTTPS 上传服务、PMX 动画容器以及 Windows 桌面备用工具。

项目的主要工作流是：浏览器读取图片或 GIF，在本机完成裁剪、缩放、调色和 RGB332 量化，生成 PMX 二进制数据，然后通过局域网 HTTPS 把 PMX 写入 ESP32 的独立 `assets` Flash 分区。日常更换动画不需要串口，也不需要重新编译固件。

> 当前已验证版本：ESP-IDF 6.0.2、经典 ESP32-D0WD-V3、4 MiB Flash、64x64 1/32 扫描 HUB75 面板。固件目标是 `esp32`，不是 `esp32s3`。

## 目录

- [项目组成](#项目组成)
- [系统架构与数据流](#系统架构与数据流)
- [硬件与接线](#硬件与接线)
- [Flash 分区与数据类型](#flash-分区与数据类型)
- [PMX1 文件格式](#pmx1-文件格式)
- [首次部署](#首次部署)
- [浏览器使用流程](#浏览器使用流程)
- [ImageDecoder 机制](#imagedecoder-机制)
- [ESP32 HTTPS 传输管线](#esp32-https-传输管线)
- [播放、RAM 缓存与频闪控制](#播放ram-缓存与频闪控制)
- [串口备用流程](#串口备用流程)
- [故障排查](#故障排查)
- [安全与限制](#安全与限制)

## 项目组成

| 路径 | 作用 |
| --- | --- |
| `matrix_idf/` | ESP-IDF 固件工程、分区表、证书脚本和 PMX 串口写入脚本 |
| `matrix_idf/main/matrix_idf.c` | HUB75 刷新、PMX 校验/播放、Wi-Fi、HTTPS 和 Flash 上传逻辑 |
| `matrix_idf/main/web_index.html` | 编译进固件的浏览器编辑器与 HTTPS 上传器 |
| `matrix_idf/main/wifi_config.example.h` | 不包含真实密码的 Wi-Fi 配置模板 |
| `matrix_idf/tools/generate_https_cert.ps1` | 为指定 IPv4 地址生成自签名 HTTPS 证书和私钥 |
| `matrix_idf/flash_pmx.ps1` | 通过串口只写 `assets` 分区的备用脚本 |
| `PixelMatrixStudio/` | Windows 原生编辑/导出工具，作为离线备用路径 |
| `pmxs/` | PMX 示例内容 |
| `image/` | 图片和 GIF 示例素材 |
| `docs/` | 工作流和网页工具的补充说明 |
| `ESP32/` | 早期 Arduino 示例和第三方库；不是当前主要固件入口 |

补充文档：

- [硬件基线与接线说明](BOARD_AND_WIRING.md)
- [PMX 内容工作流](PMX_CONTENT_WORKFLOW.md)
- [完整数据管线](docs/WORKFLOW.md)
- [网页工具说明](docs/WEB_TOOL.md)

## 系统架构与数据流

```text
PNG/JPG/BMP/WEBP/GIF/PMX
            |
            v
浏览器 HTTPS 页面（页面本身由 ESP32 提供）
            |
            +-- GIF: File -> ArrayBuffer -> ImageDecoder -> VideoFrame/RGBA
            +-- 静态图: createImageBitmap -> Canvas RGBA
            +-- PMX: 校验后可直接上传
            |
            v
裁剪 -> 缩放 -> X/Y 布局 -> 采样 -> 亮度/对比度/饱和度
            |
            v
64x64 RGBA 帧 -> RGB332（每像素 1 字节）-> PMX1
            |
            v
fetch(POST /upload, PMX 原始字节)
            |
        TLS/HTTPS :443
            |
            v
ESP32 校验 PMX 头 -> 暂停播放 -> 擦除/写入 assets 分区
            |
            v
Flash PMX -> RAM 帧缓存 -> 双帧缓冲 -> HUB75 GPIO 扫描输出
```

桌面备用路径最终也生成相同的 PMX1，因此浏览器和桌面工具共用同一个固件播放管线。

## 硬件与接线

### 已验证硬件基线

- MCU：经典 ESP32，30 针 DevKit 风格开发板，目标 `esp32`。
- USB 串口桥：CH340；本机示例端口为 `COM20`。
- 面板：单块 64x64、1/32 扫描 HUB75 RGB 面板。
- Flash：4 MiB。
- 网络：2.4 GHz Wi-Fi STA 模式；固件不会创建热点。

### HUB75 信号映射

下表必须与 [matrix_idf.c](matrix_idf/main/matrix_idf.c) 中的宏一致。

| HUB75 信号 | ESP32 GPIO | 说明 |
| --- | ---: | --- |
| R1 | 25 | 上半屏红色数据 |
| G1 | 26 | 上半屏绿色数据 |
| B1 | 27 | 上半屏蓝色数据 |
| R2 | 14 | 下半屏红色数据 |
| G2 | 18 | 下半屏绿色数据 |
| B2 | 13 | 下半屏蓝色数据 |
| A | 23 | 行地址 bit 0 |
| B | 22 | 行地址 bit 1 |
| C | 19 | 行地址 bit 2 |
| D | 17 | 行地址 bit 3 |
| E | 32 | 行地址 bit 4，用于 1/32 扫描 |
| LAT / STB | 21 | 锁存脉冲 |
| OE | 33 | 输出使能，低电平点亮 |
| CLK | 16 | 像素移位时钟 |
| GND | GND | ESP32、逻辑电平转换器和面板电源必须共地 |

这些 GPIO 避开了常见启动绑带脚 GPIO0、GPIO2、GPIO4、GPIO5、GPIO12 和 GPIO15。不要使用 GPIO34 至 GPIO39 驱动 HUB75，因为它们只能输入。

### 推荐物理连接

```text
ESP32 GPIO
    |
    v
74AHCT245 / 74HCT245 逻辑电平转换器（3.3 V -> 5 V）
    |
    v
HUB75 排线 -> 面板 IN 接口

独立稳压 5 V 电源 --------> 面板 5 V
独立电源 GND -------------+-> 面板 GND
ESP32 GND ----------------+
电平转换器 GND -----------+
```

### 供电要求

- 不要使用 ESP32 开发板的 5 V 引脚给 64x64 面板供电。
- 使用独立、稳压的 5 V 电源，建议至少 3 A；高亮全白画面可能需要更大余量。
- 必须共地，否则数据电平没有共同参考，可能随机闪烁或完全不显示。
- 推荐使用 `74AHCT245` 或 `74HCT245`。面板有时能直接识别 3.3 V，但不应把这种边缘电平当成可靠部署方案。
- HUB75 排线连接面板的 `IN`，不要接 `OUT`。
- 首次上电前用万用表确认 5 V/GND 极性，避免反接损坏面板。

### 面板方向

当前安装方向需要在固件中把逻辑画面逆时针旋转 90 度后写入扫描缓冲。该映射由 `panel_copy_display_frame()` 完成。如果换了面板方向，应修改映射函数，而不是在每个素材中手工旋转。

## Flash 分区与数据类型

ESP32 使用 4 MiB Flash，地址范围为 `0x000000` 至 `0x3FFFFF`。项目分区表位于 [partitions.csv](matrix_idf/partitions.csv)。

| 地址范围 | 大小 | 名称 | 类型/子类型 | 存放内容 | 谁会写入 |
| --- | ---: | --- | --- | --- | --- |
| `0x001000-0x007FFF` | 28 KiB 区域 | Bootloader | 引导程序 | ESP-IDF 二级引导程序 | `idf.py flash` |
| `0x008000-0x008FFF` | 4 KiB 区域 | Partition table | 分区表 | `partitions.csv` 生成的二进制表 | `idf.py flash` |
| `0x009000-0x00EFFF` | `0x6000` / 24 KiB | `nvs` | data/nvs | NVS、Wi-Fi/系统运行数据；SSID/密码本身编译在应用中，Wi-Fi 配置设置为 RAM storage | 固件/NVS 驱动 |
| `0x00F000-0x00FFFF` | `0x1000` / 4 KiB | `phy_init` | data/phy | 射频 PHY 初始化/校准相关数据 | PHY/Wi-Fi 驱动 |
| `0x010000-0x10FFFF` | `0x100000` / 1 MiB | `factory` | app/factory | 主程序、嵌入网页、HTTPS 证书和私钥、HUB75/网络/播放器代码 | `idf.py flash` |
| `0x110000-0x3FFFFF` | `0x2F0000` / 3008 KiB | `assets` | data/0x40 | 一个完整 PMX1 文件，从分区偏移 0 开始 | HTTPS `/upload` 或 `flash_pmx.ps1` |

关键结论：

- `idf.py flash` 默认写 Bootloader、分区表和 `factory` 应用，不会自动覆盖 `assets`。
- 因此重新烧录固件后，旧动画通常仍然存在。
- 网页上传只擦写 `assets`，不会破坏固件、分区表、NVS 或证书。
- 修改 `partitions.csv` 后必须重新烧录分区表，并确认新布局不会覆盖已有数据。
- `assets` 正好结束于 4 MiB Flash 末尾 `0x400000`。

## PMX1 文件格式

PMX 是浏览器工具、Windows 工具和 ESP32 固件之间的显示内容协议。所有整数都使用小端序。

### 文件头：24 字节

| 偏移 | 大小 | 字段 | 当前值/含义 |
| ---: | ---: | --- | --- |
| `0` | 4 | `magic` | ASCII `PMX1` |
| `4` | 1 | `version` | `1` |
| `5` | 1 | `format` | `1`，RGB332 |
| `6` | 1 | `width` | `64` |
| `7` | 1 | `height` | `64` |
| `8` | 4 | `frame_count` | 帧数 |
| `12` | 4 | `directory_offset` | 帧目录起点，网页生成器使用 `24` |
| `16` | 4 | `data_offset` | 像素数据起点，通常为 `24 + frame_count * 8` |
| `20` | 4 | `file_size` | PMX 总字节数，必须等于 HTTP 请求体长度 |

### 帧目录：每帧 8 字节

| 相对偏移 | 大小 | 字段 | 含义 |
| ---: | ---: | --- | --- |
| `0` | 4 | `delay_ms` | 当前帧显示时间；固件最小按 20 ms 处理 |
| `4` | 4 | `data_offset` | 当前帧 4096 字节像素数据在文件中的绝对偏移 |

### 帧像素：每帧 4096 字节

64x64 一共有 4096 个像素，每个像素一个 RGB332 字节：

```text
bit 7..5 = RRR
bit 4..2 = GGG
bit 1..0 = BB
```

文件大小计算：

```text
PMX 字节数 = 24 + 帧数 * 8 + 帧数 * 4096
```

示例：

```text
1 帧  = 24 + 1*8  + 1*4096  = 4,128 字节
23 帧 = 24 + 23*8 + 23*4096 = 94,416 字节
26 帧 = 24 + 26*8 + 26*4096 = 106,728 字节
```

## 首次部署

### 1. 安装软件

- Windows 10/11。
- ESP-IDF 6.0.2，目标工具链为 `xtensa-esp-elf`。
- VS Code ESP-IDF 扩展可选；命令行是最可复现的方式。
- Chrome 或 Edge，用于支持 `ImageDecoder` 的 HTTPS 网页。
- OpenSSL，或者带 `cryptography` 包的 Python，用于生成证书。

### 2. 配置 Wi-Fi

仓库只提交模板，不提交真实密码：

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf\main
Copy-Item wifi_config.example.h wifi_config.h
notepad wifi_config.h
```

填写 2.4 GHz 网络：

```c
#define MATRIX_WIFI_SSID "YOUR_2_4G_WIFI_SSID"
#define MATRIX_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

`wifi_config.h` 被 Git 忽略。当前固件要求至少 WPA2-PSK，不适用于只开放 5 GHz、需要网页登录的企业/访客网络或 WPA3-only 配置。

### 3. 固定设备地址

网页和证书示例使用 `192.168.1.218`。建议在路由器 DHCP 中按 ESP32 MAC 地址设置固定租约。证书 Subject Alternative Name 绑定 IP；如果地址变化，应重新生成证书。

### 4. 生成 HTTPS 证书

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
.\tools\generate_https_cert.ps1 -IpAddress 192.168.1.218
```

生成：

- `main/https_server.crt`：自签名服务端证书。
- `main/https_server.key`：未加密私钥，必须保密。

两者是本地构建输入并被 Git 忽略。`CMakeLists.txt` 使用 `target_add_binary_data()` 把网页、证书和私钥嵌入 `factory` 应用镜像。

浏览器首次打开时可以手动接受自签名警告；更稳定的部署方式是在受控客户端上把证书导入“受信任的根证书颁发机构”，然后完全重启 Chrome/Edge。不要把私钥导入客户端或提交到仓库。

### 5. 统一 ESP-IDF 环境

不要在同一个 `build` 目录混用 `C:\Espressif` 和 `D:\espidf` 两套 Python。项目当前使用：

```powershell
$env:IDF_TOOLS_PATH = 'D:\espidf\v6.0.2\.espressif'
$env:IDF_PATH = 'D:\espidf\v6.0.2\esp-idf'
$IdfPython = 'D:\espidf\v6.0.2\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe'

. 'D:\espidf\v6.0.2\esp-idf\export.ps1'
```

如果看到“active Python 与 configured Python 不一致”，清理的只是本地构建目录：

```powershell
& $IdfPython "$env:IDF_PATH\tools\idf.py" fullclean
```

### 6. 编译

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
& $IdfPython "$env:IDF_PATH\tools\idf.py" build
```

主要产物：

| 文件 | 烧录地址 | 说明 |
| --- | ---: | --- |
| `build/bootloader/bootloader.bin` | `0x1000` | Bootloader |
| `build/partition_table/partition-table.bin` | `0x8000` | 分区表 |
| `build/matrix_idf.bin` | `0x10000` | 应用、网页、证书、固件逻辑 |

### 7. 首次烧录

```powershell
& $IdfPython "$env:IDF_PATH\tools\idf.py" -p COM20 flash monitor
```

退出串口监视器：`Ctrl+]`。

成功启动会显示项目版本、从 `assets` 读取到的 PMX、Wi-Fi 地址以及 HTTPS 443 端口。若 `assets` 已经有旧 PMX，首次刷新应用后仍会继续播放它，这是分区隔离的预期行为。

## 浏览器使用流程

1. 确保电脑和 ESP32 位于同一局域网。
2. 打开 `https://192.168.1.218/`。
3. 首次访问接受或信任自签名证书。
4. 固件升级后按 `Ctrl+F5`，避免浏览器继续使用旧网页脚本。
5. 选择 PNG、JPG、BMP、WEBP、GIF 或现有 PMX。
6. 检查导入信息。动画 GIF 必须显示大于 1 的帧数。
7. 设置裁剪比例，例如 `1:1`、`16:9`、`4:3`。
8. 在源图中拖动裁剪框；框内拖动移动，框外拖动重画。
9. 设置输出宽高以及在 64x64 画布中的 X/Y 坐标。
10. 选择最近邻或多点采样，调整亮度、对比度和饱和度。
11. 检查 64x64 预览。
12. 可选导出首帧 PNG、动画 GIF 或 PMX。
13. 点击“HTTPS 上传到 Flash”。上传按钮会暂时禁用，`/ping` 轮询也会暂停。
14. 页面显示实际 PMX 帧数和字节数；ESP32 写入完成后显示 RAM cache 或 Flash streaming 状态。

载入现有 PMX 时，网页保留原始 PMX 字节并直接上传/下载，避免只把前 120 帧的预览重新编码。PMX 预览最多显示前 120 帧，但直接上传仍保留完整文件。

## ImageDecoder 机制

### 为什么需要它

`createImageBitmap()` 很适合静态图，但对 GIF 通常只得到一个静态画面。`ImageDecoder` 属于浏览器 WebCodecs 图像解码接口，可以读取 GIF 的轨道信息、总帧数、逐帧像素和帧持续时间，因此是浏览器端生成多帧 PMX 的核心。

### 安全上下文

浏览器通常只在安全上下文中开放相关能力。页面由 ESP32 通过 HTTPS 提供，因此：

```js
self.isSecureContext === true
typeof ImageDecoder === 'function'
```

这两个条件应同时成立。仅从磁盘打开 `web_index.html` 不能访问 ESP32 的相对 `/upload`，也不是推荐工作流。

### 接受的数据类型

`ImageDecoderInit.data` 接受：

- `ArrayBuffer`。
- `ArrayBufferView`，例如 `Uint8Array`、`DataView`。
- `ReadableStream`。

它不直接接受 DOM `File`/`Blob` 对象。正确管线是：

```js
const bytes = await file.arrayBuffer();
const decoder = new ImageDecoder({
  data: bytes,
  type: file.type || 'image/gif'
});
```

直接传 `data: file` 会抛出：

```text
The provided value is not of type
'(ReadableStream or ArrayBuffer or ArrayBufferView)'
```

### 项目中的逐帧管线

```text
<input type=file>
    -> File
    -> await File.arrayBuffer()
    -> new ImageDecoder({data, type: 'image/gif'})
    -> await decoder.tracks.ready
    -> selectedTrack.frameCount
    -> decoder.decode({frameIndex})
    -> VideoFrame
    -> CanvasRenderingContext2D.drawImage()
    -> getImageData() 得到 RGBA
    -> 记录 duration（微秒转毫秒，最少 20 ms）
    -> 裁剪/缩放/调色
    -> RGB332
    -> PMX1
```

网页最多读取 720 帧，防止极长 GIF 占用过多浏览器内存。解码完成后会调用 `VideoFrame.close()` 和 `decoder.close()` 释放浏览器底层资源。

如果 `ImageDecoder` 不存在或解码失败，网页会退回 `createImageBitmap()`。该路径只能保证静态画面，因此 GIF 很可能变成 1 帧 PMX。诊断时查看：

```js
console.log(self.isSecureContext, typeof ImageDecoder);
console.log(srcFrames.length, canvasFrames.length);
```

刚导入时 `canvasFrames.length === 1` 可能只是单帧预览，关键是 `srcFrames.length` 和上传状态中显示的帧数。上传前 `makePmx()` 会对所有 `srcFrames` 重新完整渲染。

## ESP32 HTTPS 传输管线

### 服务启动

1. 固件初始化 NVS、网络接口和默认事件循环。
2. ESP32 进入 Wi-Fi STA 模式并连接 2.4 GHz 路由器。
3. DHCP 获得 IP 后触发 `IP_EVENT_STA_GOT_IP`。
4. 固件把嵌入的证书/私钥交给 `esp_https_server`。
5. HTTPS 服务监听 TCP 443。

注册端点：

| 方法 | URI | 作用 |
| --- | --- | --- |
| `GET` | `/` | 返回编译进固件的 `web_index.html` |
| `GET` | `/ping` | 返回上传状态、PMX generation 和缓存帧数 JSON |
| `POST` | `/upload` | 接收完整 PMX 原始请求体并写入 `assets` |

### 浏览器发送

```text
canvasFrames
    -> makePmx()
    -> Uint8Array
    -> fetch('/upload', {method:'POST', body: pmx})
    -> 浏览器 TLS 加密
    -> TCP 443
```

页面在上传期间设置 `uploadBusy`，禁用按钮并暂停 `/ping`，减少重复上传和并发 TLS 会话带来的内存压力。

### 固件接收和写入

`http_upload_handler()` 的顺序：

1. 拒绝并发上传。
2. 定位类型 `data`、子类型 `0x40`、标签 `assets` 的分区。
3. 检查 HTTP `Content-Length` 至少为 24 且不超过分区容量。
4. 先读取 24 字节 PMX 头。
5. 校验 magic、版本、格式、64x64 尺寸、帧数、目录/数据偏移和总长度。
6. 设置 `s_http_uploading`、`s_assets_updating`，增加 generation，让播放器退出旧循环。
7. 等待播放器停下，然后设置 `s_display_paused`。
8. 按 4 KiB 对齐擦除需要覆盖的 `assets` 范围，而不是擦除整个 Flash。
9. 先写 PMX 头，再用 1024 字节缓冲循环接收并写入剩余请求体。
10. 从新写入的 Flash 读取所有帧并准备 RAM 缓存。
11. 恢复显示、增加 generation、发送 HTTPS 响应。
12. 响应发送完后清除上传标志，播放器开始新 PMX。

失败路径会恢复显示和状态标志。写入途中断线可能使 `assets` 中的新 PMX 不完整，但旧 RAM 缓存在可能的情况下仍保留到重启前。

### 为什么 HTTPS 会消耗较多 RAM

TLS 握手需要证书解析、加密上下文、收发缓冲和套接字。浏览器还可能为页面、`/ping`、favicon 或预连接并发建立多个会话。在 ESP32 无 PSRAM 的环境中，这会造成堆碎片；少量 `mbedtls_ssl_setup` 或 handshake 失败可能是并发连接内存不足。关闭重复标签页、等待旧连接释放并避免上传时轮询可以减轻问题。

## 播放、RAM 缓存与频闪控制

### 启动顺序

1. 初始化 HUB75 GPIO。
2. 显示内置测试图案。
3. 创建高优先级刷新任务和 PMX 播放任务。
4. 播放任务在 Wi-Fi/TLS 启动前读取 `assets`。
5. 至少申请 26 帧，即 `26 * 4096 = 106496` 字节连续 RAM。
6. 缓存建立后才初始化 Wi-Fi，降低 TLS 导致大块内存碎片化的风险。

### 缓存限制

- 编译限制：`PMX_MAX_CACHE_BYTES = 240 KiB`，最多 60 帧。
- 稳定预留：26 帧。
- 新动画不超过已有缓存容量时直接复用同一块内存。
- 大于 26 帧时固件会尝试申请更大的连续块；成功即可 RAM 播放，失败则 Flash streaming。
- 超过 60 帧不会尝试 RAM 缓存，但 PMX 仍可存入 `assets` 并流式播放。

Flash streaming 每帧都调用 `esp_partition_read()`，可能与 HUB75 实时刷新竞争并出现频闪。RAM 播放只在帧切换时复制 4096 字节到双帧缓冲，稳定性更高。

### 双帧缓冲与扫描

- `s_framebuffers[2]` 是前后两个 4096 字节扫描帧。
- 播放任务把下一帧复制到后缓冲，然后原子式切换 `s_front_buffer`。
- HUB75 刷新任务在 CPU1 上运行，逐行、逐 PWM bit plane 输出。
- 播放任务在 CPU0 上按每帧延时切换画面。
- Flash 擦写期间 OE 被关闭，允许一次受控黑屏，避免擦写时持续输出损坏数据。

上传成功后的理想日志：

```text
PMX cached in RAM: 26 frames, 106496 bytes, capacity 26 frames (reused)
HTTPS Flash upload complete: 106728 bytes, 26 frames; RAM cache ready
Playing RAM PMX: 26 frames
```

## 串口备用流程

网页不可用时，可以用 Windows 桌面工具生成 PMX，然后只写 `assets`：

```powershell
Set-Location C:\Users\hanjuncheng\Desktop\MatrixWithTerm\matrix_idf
.\flash_pmx.ps1 -ContentPath "C:\path\animation.pmx" -Port COM20
```

脚本会检查 PMX1 magic、版本、RGB332、64x64、声明长度和 3008 KiB 容量，然后使用 esptool 写到 `0x110000`。它不会重编译或覆盖应用程序。

使用串口备用流程时必须先关闭 ESP-IDF monitor、串口终端和其他占用 COM20 的程序。

## 故障排查

### 启动版本仍是旧提交

观察：

```text
App version: 9a354f5
```

如果预期是更新提交，说明烧录了旧 `build/matrix_idf.bin`。执行 `fullclean`、`build`、`flash`，并确认串口中打印新的 Git 短哈希。

### C: 与 D: ESP-IDF Python 冲突

错误会说明 active Python 和 configured Python 不同。不要继续混用环境；显式使用 D 盘 Python 执行 `fullclean`，再重新构建。

### GIF 仍然只有一帧

1. 使用原始 `.gif`，不要误选之前导出的单帧 PMX/PNG。
2. 页面必须通过 HTTPS 打开并按 `Ctrl+F5`。
3. 导入信息必须显示多帧。
4. 检查 `typeof ImageDecoder === 'function'`。
5. `ImageDecoder` 的 `data` 必须是 `await file.arrayBuffer()`，不能直接传 `File`。
6. 上传状态若显示 `1 帧 / 4128 字节`，问题在浏览器生成阶段；若显示多帧但固件收到 1 帧，再检查传输。

### 上传多帧后频闪

检查是否有：

```text
RAM cache ready
Playing RAM PMX: N frames
```

如果出现 `RAM cache unavailable` 或 `Falling back to flash streaming`，动画超出缓存能力或连续堆内存不足。减少帧数到 26 帧以内通常最稳定。

### TLS 会话创建失败

- 关闭同一设备的重复浏览器标签页。
- 等待数秒让旧 TLS 会话释放。
- 上传时不要重复点击。
- 确保证书 IP SAN 与当前设备 IP 一致。
- 如果页面仍能打开且上传成功，偶发浏览器预连接失败不代表主会话失败。

### 页面打不开

- 确认 ESP32 和客户端在同一网段。
- 串口必须出现 `Server listening on port 443`。
- 使用 `https://`，不是 `http://`。
- 检查路由器客户端隔离、Windows 防火墙和固定 IP。

### 固件升级后还是旧网页

网页嵌入应用镜像，必须重新烧录 `matrix_idf.bin`。烧录后按 `Ctrl+F5` 或清除该 IP 的站点缓存。

## 安全与限制

- 这是局域网设备，不包含用户认证。能访问设备 443 端口的客户端可以替换 `assets` 内容。
- 自签名私钥嵌入应用镜像；不要把当前方案当作互联网公开服务的生产级身份体系。
- `wifi_config.h`、证书和私钥被 Git 忽略，禁止提交真实凭据。
- 固件不执行 OTA；应用升级仍通过串口烧录。
- 网页最大解码 720 帧；固件保证的 RAM 预留为 26 帧。
- PMX 使用 RGB332，颜色精度为红 3 bit、绿 3 bit、蓝 2 bit。
- 应用分区只有 1 MiB，嵌入网页增大也会增加 `matrix_idf.bin`。
- 当前只针对一块 64x64 1/32 扫描面板和固定 GPIO 映射；级联、多面板、不同扫描率需要修改固件。

## 典型日常流程

首次只做一次：接线、配置 Wi-Fi、生成证书、编译并串口烧录固件。

之后每次换内容：

```text
打开 https://192.168.1.218/
    -> 导入图片/GIF
    -> 确认 GIF 帧数
    -> 裁剪和预览
    -> HTTPS 上传
    -> ESP32 写 assets
    -> RAM 缓存播放
```

这条日常路径不需要 COM20、不需要 VS Code、不需要重新编译，也不会覆盖播放器固件。
