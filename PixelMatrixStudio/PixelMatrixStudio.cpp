#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define GIF_IMPL
#include "gif.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Gdiplus;

namespace {
constexpr int kCanvas = 64;
enum ControlId { ID_LOAD = 100, ID_GIF, ID_PNG, ID_RESET_ADJUST, ID_PMX, ID_FLASH_PMX, ID_ASPECT, ID_WIDTH, ID_HEIGHT, ID_X, ID_Y, ID_MODE, ID_RESET_CROP, ID_BRIGHTNESS, ID_CONTRAST, ID_SATURATION, ID_BRIGHTNESS_INPUT, ID_CONTRAST_INPUT, ID_SATURATION_INPUT, ID_STATUS };

struct Frame {
    std::vector<uint8_t> rgba;
    UINT delayMs = 100;
};

std::vector<Frame> g_source;
std::vector<Frame> g_canvas;
std::vector<uint8_t> g_sourceBgr;
std::wstring g_inputName;
HWND g_status = nullptr;
int g_sourceWidth = 0;
int g_sourceHeight = 0;
RECT g_crop{};
RECT g_sourceView{};
bool g_dragging = false;
bool g_movingCrop = false;
POINT g_dragAnchor{};
POINT g_moveOffset{};
size_t g_previewFrame = 0;
HFONT g_chineseFont = nullptr;
HFONT g_latinFont = nullptr;
bool g_syncingAdjustments = false;
HBRUSH g_darkBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_inputBrush = nullptr;
HBRUSH g_buttonBrush = nullptr;

constexpr COLORREF kBackground = RGB(26, 31, 52);
constexpr COLORREF kPanel = RGB(37, 48, 73);
constexpr COLORREF kInput = RGB(48, 62, 88);
constexpr COLORREF kText = RGB(244, 226, 184);
constexpr COLORREF kAccent = RGB(239, 151, 132);
constexpr COLORREF kMint = RGB(117, 204, 190);
constexpr COLORREF kLavender = RGB(157, 124, 185);
constexpr uint32_t kPmxHeaderBytes = 24;
constexpr uint32_t kPmxFrameDirectoryBytes = 8;
constexpr uint32_t kPmxFrameBytes = kCanvas * kCanvas;
constexpr uint32_t kPmxAssetCapacity = 0x2F0000;

void InvalidatePanels(HWND hwnd, bool source = true, bool canvas = true) {
    if (source) { RECT sourceRect{ 235, 55, 590, 585 }; InvalidateRect(hwnd, &sourceRect, FALSE); }
    if (canvas) { RECT canvasRect{ 620, 70, 920, 500 }; InvalidateRect(hwnd, &canvasRect, FALSE); }
}

bool ContainsChinese(const wchar_t* text) {
    for (const wchar_t* p = text; *p; ++p) if (*p >= 0x4E00 && *p <= 0x9FFF) return true;
    return false;
}

BOOL CALLBACK ApplyFontToChild(HWND child, LPARAM) {
    wchar_t text[256]{};
    GetWindowTextW(child, text, 255);
    bool useChinese = ContainsChinese(text) || GetDlgCtrlID(child) == ID_MODE;
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(useChinese ? g_chineseFont : g_latinFont), TRUE);
    return TRUE;
}

void DrawBackdrop(HDC hdc, const RECT& client) {
    FillRect(hdc, &client, g_darkBrush);
    HBRUSH lavender = CreateSolidBrush(RGB(73, 63, 103));
    POINT plain[] = { { 225, 675 }, { 330, 615 }, { 430, 658 }, { 515, 602 }, { 610, 675 } };
    Polygon(hdc, plain, 5); DeleteObject(lavender);
    HPEN fine = CreatePen(PS_SOLID, 1, RGB(111, 151, 157));
    HPEN accent = CreatePen(PS_SOLID, 1, kMint);
    HGDIOBJ oldPen = SelectObject(hdc, fine);
    Arc(hdc, 700, 500, 1080, 880, 190, 720, 730, 520);
    Arc(hdc, 710, 510, 1070, 870, 195, 710, 735, 525);
    MoveToEx(hdc, 235, 610, nullptr); LineTo(hdc, 920, 610);
    SelectObject(hdc, accent);
    POINT ridge[] = { { 650, 585 }, { 705, 540 }, { 750, 575 }, { 810, 520 }, { 900, 585 } };
    Polyline(hdc, ridge, 5);
    for (int i = 0; i < 8; ++i) Ellipse(hdc, 625 + i * 35, 625 + (i % 3) * 11, 629 + i * 35, 629 + (i % 3) * 11);
    SelectObject(hdc, oldPen); DeleteObject(fine); DeleteObject(accent);
}

void DrawArtFrame(HDC hdc, const RECT& rect) {
    HPEN outer = CreatePen(PS_SOLID, 2, kAccent);
    HPEN inner = CreatePen(PS_SOLID, 1, kMint);
    HGDIOBJ old = SelectObject(hdc, outer); HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, inner); Rectangle(hdc, rect.left + 4, rect.top + 4, rect.right - 4, rect.bottom - 4);
    SelectObject(hdc, oldBrush); SelectObject(hdc, old); DeleteObject(outer); DeleteObject(inner);
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

void SetStatus(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }

int ReadInt(HWND hwnd, int id, int fallback) {
    wchar_t buffer[32]{};
    GetWindowTextW(GetDlgItem(hwnd, id), buffer, 31);
    int value = _wtoi(buffer);
    return value > 0 || id == ID_X || id == ID_Y ? value : fallback;
}

double ReadAspect(HWND hwnd) {
    wchar_t text[64]{};
    GetWindowTextW(GetDlgItem(hwnd, ID_ASPECT), text, 63);
    std::wstring value(text);
    size_t colon = value.find(L':');
    if (colon != std::wstring::npos) {
        double a = _wtof(value.substr(0, colon).c_str());
        double b = _wtof(value.substr(colon + 1).c_str());
        if (a > 0.0 && b > 0.0) return a / b;
    }
    double ratio = _wtof(value.c_str());
    return ratio > 0.0 ? ratio : 1.0;
}

bool HasCompleteAspect(HWND hwnd) {
    wchar_t text[64]{};
    GetWindowTextW(GetDlgItem(hwnd, ID_ASPECT), text, 63);
    std::wstring value(text);
    size_t colon = value.find(L':');
    if (colon != std::wstring::npos) return colon > 0 && colon + 1 < value.size() && _wtof(value.substr(0, colon).c_str()) > 0.0 && _wtof(value.substr(colon + 1).c_str()) > 0.0;
    return _wtof(value.c_str()) > 0.0;
}

void ResetCrop(double aspect) {
    if (g_sourceWidth <= 0 || g_sourceHeight <= 0) return;
    int width = g_sourceWidth, height = g_sourceHeight;
    if (static_cast<double>(width) / height > aspect) width = std::max(1, static_cast<int>(height * aspect));
    else height = std::max(1, static_cast<int>(width / aspect));
    g_crop = RECT{ (g_sourceWidth - width) / 2, (g_sourceHeight - height) / 2,
                   (g_sourceWidth - width) / 2 + width, (g_sourceHeight - height) / 2 + height };
}

void FitPixelContentToAspect(HWND hwnd, double aspect) {
    int width = kCanvas, height = kCanvas;
    if (aspect >= 1.0) height = std::clamp(static_cast<int>(kCanvas / aspect + 0.5), 1, kCanvas);
    else width = std::clamp(static_cast<int>(kCanvas * aspect + 0.5), 1, kCanvas);
    wchar_t value[16]{};
    swprintf(value, 16, L"%d", width); SetWindowTextW(GetDlgItem(hwnd, ID_WIDTH), value);
    swprintf(value, 16, L"%d", height); SetWindowTextW(GetDlgItem(hwnd, ID_HEIGHT), value);
    swprintf(value, 16, L"%d", (kCanvas - width) / 2); SetWindowTextW(GetDlgItem(hwnd, ID_X), value);
    swprintf(value, 16, L"%d", (kCanvas - height) / 2); SetWindowTextW(GetDlgItem(hwnd, ID_Y), value);
}

RECT MakeCrop(POINT anchor, POINT current, double aspect) {
    int dx = current.x - anchor.x, dy = current.y - anchor.y;
    int width = std::abs(dx), height = std::abs(dy);
    if (width == 0 && height == 0) width = 1;
    if (height == 0) height = std::max(1, static_cast<int>(width / aspect));
    if (static_cast<double>(width) / height > aspect) height = std::max(1, static_cast<int>(width / aspect));
    else width = std::max(1, static_cast<int>(height * aspect));
    width = std::min(width, g_sourceWidth); height = std::min(height, g_sourceHeight);
    int left = dx >= 0 ? anchor.x : anchor.x - width;
    int top = dy >= 0 ? anchor.y : anchor.y - height;
    left = std::clamp(left, 0, g_sourceWidth - width);
    top = std::clamp(top, 0, g_sourceHeight - height);
    return RECT{ left, top, left + width, top + height };
}

bool OpenFileDialog(HWND owner, wchar_t* path, DWORD size, const wchar_t* filter, const wchar_t* extension) {
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = size;
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&dialog) == TRUE;
}

bool SaveFileDialog(HWND owner, wchar_t* path, DWORD size, const wchar_t* filter, const wchar_t* extension) {
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = size;
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    return GetSaveFileNameW(&dialog) == TRUE;
}

bool LoadStaticFrameWithWic(const std::wstring& path, std::wstring& error) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* source = nullptr;
    IWICFormatConverter* converter = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                         WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &source);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(source, GUID_WICPixelFormat32bppRGBA,
                                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                                           WICBitmapPaletteTypeCustom);

    UINT width = 0, height = 0;
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0) {
        if (converter) converter->Release();
        if (source) source->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        error = L"无法解码该图片或 GIF。请确认图片文件未损坏。";
        return false;
    }

    Frame frame;
    frame.delayMs = 100;
    frame.rgba.resize(static_cast<size_t>(width) * height * 4);
    result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(frame.rgba.size()), frame.rgba.data());
    converter->Release();
    source->Release();
    decoder->Release();
    factory->Release();
    if (FAILED(result)) {
        error = L"无法读取图片像素数据。";
        return false;
    }

    g_source.assign(1, std::move(frame));
    g_sourceWidth = static_cast<int>(width);
    g_sourceHeight = static_cast<int>(height);
    g_sourceBgr.resize(static_cast<size_t>(width) * height * 4);
    for (int index = 0; index < g_sourceWidth * g_sourceHeight; ++index) {
        g_sourceBgr[index * 4] = g_source[0].rgba[index * 4 + 2];
        g_sourceBgr[index * 4 + 1] = g_source[0].rgba[index * 4 + 1];
        g_sourceBgr[index * 4 + 2] = g_source[0].rgba[index * 4];
        g_sourceBgr[index * 4 + 3] = g_source[0].rgba[index * 4 + 3];
    }
    return true;
}

bool LoadFrames(const std::wstring& path, std::wstring& error) {
    Image source(path.c_str());
    if (source.GetLastStatus() != Ok || source.GetWidth() == 0 || source.GetHeight() == 0) {
        return LoadStaticFrameWithWic(path, error);
    }
    UINT dimensionCount = source.GetFrameDimensionsCount();
    std::vector<GUID> dimensions(dimensionCount);
    if (dimensionCount) source.GetFrameDimensionsList(dimensions.data(), dimensionCount);
    GUID dimension = dimensionCount ? dimensions[0] : FrameDimensionTime;
    UINT frameCount = source.GetFrameCount(&dimension);
    if (frameCount == 0) frameCount = 1;

    std::vector<UINT> delays(frameCount, 100);
    UINT propertySize = source.GetPropertyItemSize(PropertyTagFrameDelay);
    if (propertySize >= sizeof(PropertyItem)) {
        std::vector<BYTE> raw(propertySize);
        PropertyItem* item = reinterpret_cast<PropertyItem*>(raw.data());
        if (source.GetPropertyItem(PropertyTagFrameDelay, propertySize, item) == Ok && item->length >= 4) {
            UINT* values = static_cast<UINT*>(item->value);
            for (UINT i = 0; i < frameCount && (i + 1) * 4 <= item->length; ++i)
                delays[i] = std::max(10u, values[i] * 10u);
        }
    }

    std::vector<Frame> loaded;
    for (UINT index = 0; index < frameCount; ++index) {
        source.SelectActiveFrame(&dimension, index);
        Bitmap copy(source.GetWidth(), source.GetHeight(), PixelFormat32bppARGB);
        Graphics graphics(&copy);
        graphics.SetCompositingMode(CompositingModeSourceCopy);
        graphics.DrawImage(&source, 0, 0, static_cast<INT>(copy.GetWidth()), static_cast<INT>(copy.GetHeight()));
        Rect area(0, 0, static_cast<INT>(copy.GetWidth()), static_cast<INT>(copy.GetHeight()));
        BitmapData data{};
        if (copy.LockBits(&area, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) {
            error = L"无法读取解码后的像素。";
            return false;
        }
        Frame frame;
        frame.delayMs = delays[index];
        frame.rgba.resize(copy.GetWidth() * copy.GetHeight() * 4);
        for (UINT y = 0; y < copy.GetHeight(); ++y) {
            BYTE* src = static_cast<BYTE*>(data.Scan0) + y * data.Stride;
            for (UINT x = 0; x < copy.GetWidth(); ++x) {
                size_t destination = (static_cast<size_t>(y) * copy.GetWidth() + x) * 4;
                frame.rgba[destination + 0] = src[x * 4 + 2];
                frame.rgba[destination + 1] = src[x * 4 + 1];
                frame.rgba[destination + 2] = src[x * 4 + 0];
                frame.rgba[destination + 3] = src[x * 4 + 3];
            }
        }
        copy.UnlockBits(&data);
        loaded.push_back(std::move(frame));
    }
    g_source = std::move(loaded);
    g_sourceWidth = static_cast<int>(source.GetWidth());
    g_sourceHeight = static_cast<int>(source.GetHeight());
    g_sourceBgr.resize(static_cast<size_t>(g_sourceWidth) * g_sourceHeight * 4);
    for (int i = 0; i < g_sourceWidth * g_sourceHeight; ++i) {
        g_sourceBgr[i * 4] = g_source[0].rgba[i * 4 + 2]; g_sourceBgr[i * 4 + 1] = g_source[0].rgba[i * 4 + 1];
        g_sourceBgr[i * 4 + 2] = g_source[0].rgba[i * 4]; g_sourceBgr[i * 4 + 3] = 255;
    }
    return true;
}

void ApplyEnhancement(uint32_t& red, uint32_t& green, uint32_t& blue, int brightness, int contrast, int saturation) {
    float r = static_cast<float>(red) + brightness * 2.55f;
    float g = static_cast<float>(green) + brightness * 2.55f;
    float b = static_cast<float>(blue) + brightness * 2.55f;
    float contrastFactor = 1.0f + contrast / 100.0f;
    r = (r - 128.0f) * contrastFactor + 128.0f;
    g = (g - 128.0f) * contrastFactor + 128.0f;
    b = (b - 128.0f) * contrastFactor + 128.0f;
    float light = r * 0.299f + g * 0.587f + b * 0.114f;
    float saturationFactor = 1.0f + saturation / 100.0f;
    red = std::clamp<int>(static_cast<int>(light + (r - light) * saturationFactor), 0, 255);
    green = std::clamp<int>(static_cast<int>(light + (g - light) * saturationFactor), 0, 255);
    blue = std::clamp<int>(static_cast<int>(light + (b - light) * saturationFactor), 0, 255);
}

Frame PixelateFrame(const Frame& source, int outputWidth, int outputHeight, int xOffset, int yOffset, int mode, int brightness, int contrast, int saturation) {
    Frame canvas;
    canvas.delayMs = source.delayMs;
    canvas.rgba.assign(kCanvas * kCanvas * 4, 0);
    for (int i = 0; i < kCanvas * kCanvas; ++i) canvas.rgba[i * 4 + 3] = 255;

    int cropWidth = g_crop.right - g_crop.left;
    int cropHeight = g_crop.bottom - g_crop.top;

    for (int y = 0; y < outputHeight; ++y) {
        int targetY = yOffset + y;
        if (targetY < 0 || targetY >= kCanvas) continue;
        for (int x = 0; x < outputWidth; ++x) {
            int targetX = xOffset + x;
            if (targetX < 0 || targetX >= kCanvas) continue;
            int x0 = g_crop.left + x * cropWidth / outputWidth;
            int x1 = g_crop.left + std::max(1, (x + 1) * cropWidth / outputWidth) - 1;
            int y0 = g_crop.top + y * cropHeight / outputHeight;
            int y1 = g_crop.top + std::max(1, (y + 1) * cropHeight / outputHeight) - 1;
            uint32_t red = 0, green = 0, blue = 0, alpha = 0;
            int samples = mode == 0 ? 1 : 4;
            for (int sampleY = 0; sampleY < (mode == 0 ? 1 : 2); ++sampleY) for (int sampleX = 0; sampleX < (mode == 0 ? 1 : 2); ++sampleX) {
                int sourceX = mode == 0 ? (x0 + x1) / 2 : x0 + (x1 - x0) * (sampleX * 2 + 1) / 4;
                int sourceY = mode == 0 ? (y0 + y1) / 2 : y0 + (y1 - y0) * (sampleY * 2 + 1) / 4;
                size_t from = (static_cast<size_t>(sourceY) * g_sourceWidth + sourceX) * 4;
                red += source.rgba[from]; green += source.rgba[from + 1]; blue += source.rgba[from + 2]; alpha += source.rgba[from + 3];
            }
            red /= samples; green /= samples; blue /= samples; alpha /= samples;
            ApplyEnhancement(red, green, blue, brightness, contrast, saturation);
            size_t to = (static_cast<size_t>(targetY) * kCanvas + targetX) * 4;
            canvas.rgba[to + 0] = static_cast<uint8_t>(red);
            canvas.rgba[to + 1] = static_cast<uint8_t>(green);
            canvas.rgba[to + 2] = static_cast<uint8_t>(blue);
            canvas.rgba[to + 3] = static_cast<uint8_t>(alpha);
        }
    }
    return canvas;
}

bool Render(HWND hwnd, bool previewOnly = false) {
    if (g_source.empty()) { SetStatus(L"请先导入图片或 GIF。 "); return false; }
    int outputWidth = std::clamp(ReadInt(hwnd, ID_WIDTH, 64), 1, 64);
    int outputHeight = std::clamp(ReadInt(hwnd, ID_HEIGHT, 64), 1, 64);
    int x = std::clamp(ReadInt(hwnd, ID_X, 0), -63, 63);
    int y = std::clamp(ReadInt(hwnd, ID_Y, 0), -63, 63);
    if (g_sourceWidth <= 0 || g_sourceHeight <= 0) { SetStatus(L"源文件尺寸无效。 "); return false; }
    int mode = static_cast<int>(SendMessageW(GetDlgItem(hwnd, ID_MODE), CB_GETCURSEL, 0, 0));
    int brightness = static_cast<int>(SendMessageW(GetDlgItem(hwnd, ID_BRIGHTNESS), TBM_GETPOS, 0, 0));
    int contrast = static_cast<int>(SendMessageW(GetDlgItem(hwnd, ID_CONTRAST), TBM_GETPOS, 0, 0));
    int saturation = static_cast<int>(SendMessageW(GetDlgItem(hwnd, ID_SATURATION), TBM_GETPOS, 0, 0));
    g_canvas.clear();
    size_t framesToRender = previewOnly ? 1 : g_source.size();
    for (size_t index = 0; index < framesToRender; ++index)
        g_canvas.push_back(PixelateFrame(g_source[index], outputWidth, outputHeight, x, y, mode, brightness, contrast, saturation));
    g_previewFrame = 0;
    if (!previewOnly && g_canvas.size() > 1) SetTimer(hwnd, 1, std::max<UINT>(10, g_canvas[0].delayMs), nullptr);
    else KillTimer(hwnd, 1);
    InvalidatePanels(hwnd);
    if (!previewOnly) {
        std::wstringstream message;
        message << L"已生成 " << g_canvas.size() << L" 帧，像素图尺寸 " << outputWidth << L"x" << outputHeight << L"，画布位置 (" << x << L", " << y << L")。";
        SetStatus(message.str());
    }
    return true;
}

bool ExportGif(HWND hwnd) {
    if (!Render(hwnd)) return false;
    wchar_t path[MAX_PATH]{};
    if (!SaveFileDialog(hwnd, path, MAX_PATH, L"GIF 动图\0*.gif\0\0", L"gif")) return false;
    GifWriter writer{};
    std::string output = Utf8(path);
    if (!GifBegin(&writer, output.c_str(), kCanvas, kCanvas, g_canvas[0].delayMs, 8, false)) { SetStatus(L"无法创建 GIF 文件。 "); return false; }
    for (const Frame& frame : g_canvas) {
        if (!GifWriteFrame(&writer, frame.rgba.data(), kCanvas, kCanvas, frame.delayMs, 8, false)) { GifEnd(&writer); SetStatus(L"GIF 导出失败。 "); return false; }
    }
    GifEnd(&writer);
    SetStatus(L"GIF 动图已导出。 ");
    return true;
}

int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT count = 0, bytes = 0;
    GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> buffer(bytes);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(count, bytes, codecs);
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, mime) == 0) { *clsid = codecs[i].Clsid; return 1; }
    return 0;
}

bool ExportPng(HWND hwnd) {
    if (!Render(hwnd)) return false;
    wchar_t path[MAX_PATH]{};
    if (!SaveFileDialog(hwnd, path, MAX_PATH, L"PNG 图片\0*.png\0\0", L"png")) return false;
    Bitmap output(kCanvas, kCanvas, PixelFormat32bppARGB);
    Rect area(0, 0, kCanvas, kCanvas);
    BitmapData data{};
    if (output.LockBits(&area, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok) return false;
    for (int y = 0; y < kCanvas; ++y) {
        BYTE* row = static_cast<BYTE*>(data.Scan0) + y * data.Stride;
        for (int x = 0; x < kCanvas; ++x) {
            size_t from = (static_cast<size_t>(y) * kCanvas + x) * 4;
            row[x * 4 + 0] = g_canvas[0].rgba[from + 2]; row[x * 4 + 1] = g_canvas[0].rgba[from + 1];
            row[x * 4 + 2] = g_canvas[0].rgba[from + 0]; row[x * 4 + 3] = 255;
        }
    }
    output.UnlockBits(&data);
    CLSID encoder{};
    if (!GetEncoderClsid(L"image/png", &encoder) || output.Save(path, &encoder, nullptr) != Ok) { SetStatus(L"PNG 导出失败。 "); return false; }
    SetStatus(L"PNG 已导出（GIF 导出第一帧）。 ");
    return true;
}

uint8_t Rgb332(const uint8_t* pixel) {
    return static_cast<uint8_t>((pixel[0] & 0xE0) | ((pixel[1] & 0xE0) >> 3) | ((pixel[2] & 0xC0) >> 6));
}

void WriteU32(std::ofstream& file, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)
    };
    file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::wstring EnsurePmxExtension(const std::wstring& path) {
    if (path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".pmx") == 0) return path;
    return path + L".pmx";
}

bool ExportPmx(HWND hwnd) {
    if (!Render(hwnd)) return false;

    const uint32_t frameCount = static_cast<uint32_t>(g_canvas.size());
    const uint32_t directoryOffset = kPmxHeaderBytes;
    const uint32_t dataOffset = directoryOffset + frameCount * kPmxFrameDirectoryBytes;
    const uint32_t fileBytes = dataOffset + frameCount * kPmxFrameBytes;
    if (fileBytes > kPmxAssetCapacity) {
        std::wstringstream message;
        message << L"动画文件超过 ESP32 内容分区容量：" << fileBytes << L" 字节。";
        SetStatus(message.str());
        return false;
    }

    wchar_t path[MAX_PATH]{};
    if (!SaveFileDialog(hwnd, path, MAX_PATH, L"PMX 显示内容 (*.pmx)\0*.pmx\0\0", L"pmx")) return false;
    const std::wstring outputPath = EnsurePmxExtension(path);
    std::ofstream file(Utf8(outputPath), std::ios::binary);
    if (!file) { SetStatus(L"无法创建 PMX 内容文件。 "); return false; }

    const uint8_t header[8] = { 'P', 'M', 'X', '1', 1, 1, kCanvas, kCanvas };
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    WriteU32(file, frameCount);
    WriteU32(file, directoryOffset);
    WriteU32(file, dataOffset);
    WriteU32(file, fileBytes);

    for (uint32_t index = 0; index < frameCount; ++index) {
        WriteU32(file, std::max<UINT>(20, g_canvas[index].delayMs));
        WriteU32(file, dataOffset + index * kPmxFrameBytes);
    }
    for (const Frame& frame : g_canvas) {
        for (int pixel = 0; pixel < kCanvas * kCanvas; ++pixel) {
            const uint8_t value = Rgb332(&frame.rgba[pixel * 4]);
            file.write(reinterpret_cast<const char*>(&value), 1);
        }
    }

    if (!file.good()) { SetStatus(L"PMX 内容导出失败。 "); return false; }
    std::wstringstream message;
    message << L"PMX 已导出：" << frameCount << L" 帧，" << fileBytes << L" 字节。";
    SetStatus(message.str());
    return true;
}

bool FlashSelectedPmx(HWND hwnd) {
    wchar_t contentPath[MAX_PATH]{};
    if (!OpenFileDialog(hwnd, contentPath, MAX_PATH, L"PMX 显示内容 (*.pmx)\0*.pmx\0\0", L"pmx")) return false;

    wchar_t executablePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0) {
        SetStatus(L"无法定位烧录脚本。 ");
        return false;
    }
    std::wstring scriptPath(executablePath);
    const size_t directoryEnd = scriptPath.find_last_of(L"\\/");
    if (directoryEnd == std::wstring::npos) {
        SetStatus(L"无法定位烧录脚本。 ");
        return false;
    }
    scriptPath.resize(directoryEnd);
    scriptPath += L"\\..\\matrix_idf\\flash_pmx.ps1";
    if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(L"未找到 matrix_idf\\flash_pmx.ps1。 ");
        return false;
    }

    std::wstring command = L"powershell.exe -NoExit -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath +
                           L"\" -ContentPath \"" + contentPath + L"\" -Port COM20";
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                        nullptr, nullptr, &startup, &process)) {
        SetStatus(L"无法启动烧录窗口。请确认 PowerShell 可用。 ");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SetStatus(L"已启动 PMX 烧录：COM20。请在打开的 PowerShell 窗口查看结果。 ");
    return true;
}

void DrawPreview(HDC hdc, const RECT& rect) {
    HBRUSH background = CreateSolidBrush(RGB(24, 27, 31));
    FillRect(hdc, &rect, background); DeleteObject(background);
    if (g_canvas.empty()) return;
    const Frame& frame = g_canvas[std::min(g_previewFrame, g_canvas.size() - 1)];
    int scale = std::min((rect.right - rect.left) / kCanvas, (rect.bottom - rect.top) / kCanvas);
    int width = kCanvas * scale, height = kCanvas * scale;
    int left = rect.left + ((rect.right - rect.left) - width) / 2;
    int top = rect.top + ((rect.bottom - rect.top) - height) / 2;
    std::vector<uint8_t> bgr(kCanvas * kCanvas * 4);
    for (int i = 0; i < kCanvas * kCanvas; ++i) {
        bgr[i * 4] = frame.rgba[i * 4 + 2]; bgr[i * 4 + 1] = frame.rgba[i * 4 + 1];
        bgr[i * 4 + 2] = frame.rgba[i * 4]; bgr[i * 4 + 3] = 255;
    }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = kCanvas; info.bmiHeader.biHeight = -kCanvas;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, left, top, width, height, 0, 0, kCanvas, kCanvas, bgr.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    RECT border{ left - 1, top - 1, left + width + 1, top + height + 1 };
    FrameRect(hdc, &border, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
}

void DrawSourcePreview(HDC hdc, const RECT& panel) {
    HBRUSH background = CreateSolidBrush(RGB(24, 27, 31));
    FillRect(hdc, &panel, background); DeleteObject(background);
    g_sourceView = RECT{};
    if (g_source.empty() || g_sourceWidth <= 0 || g_sourceHeight <= 0) return;
    double scale = std::min(static_cast<double>(panel.right - panel.left) / g_sourceWidth, static_cast<double>(panel.bottom - panel.top) / g_sourceHeight);
    int width = std::max(1, static_cast<int>(g_sourceWidth * scale));
    int height = std::max(1, static_cast<int>(g_sourceHeight * scale));
    g_sourceView = RECT{ panel.left + ((panel.right - panel.left) - width) / 2, panel.top + ((panel.bottom - panel.top) - height) / 2,
                         panel.left + ((panel.right - panel.left) - width) / 2 + width, panel.top + ((panel.bottom - panel.top) - height) / 2 + height };
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = g_sourceWidth; info.bmiHeader.biHeight = -g_sourceHeight;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, g_sourceView.left, g_sourceView.top, width, height, 0, 0, g_sourceWidth, g_sourceHeight, g_sourceBgr.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    RECT crop{ g_sourceView.left + g_crop.left * width / g_sourceWidth, g_sourceView.top + g_crop.top * height / g_sourceHeight,
               g_sourceView.left + g_crop.right * width / g_sourceWidth, g_sourceView.top + g_crop.bottom * height / g_sourceHeight };
    HBRUSH gray = CreateSolidBrush(RGB(100, 100, 100));
    RECT outsideTop{ g_sourceView.left, g_sourceView.top, g_sourceView.right, crop.top };
    RECT outsideBottom{ g_sourceView.left, crop.bottom, g_sourceView.right, g_sourceView.bottom };
    RECT outsideLeft{ g_sourceView.left, crop.top, crop.left, crop.bottom };
    RECT outsideRight{ crop.right, crop.top, g_sourceView.right, crop.bottom };
    FillRect(hdc, &outsideTop, gray); FillRect(hdc, &outsideBottom, gray); FillRect(hdc, &outsideLeft, gray); FillRect(hdc, &outsideRight, gray);
    DeleteObject(gray);
    FrameRect(hdc, &crop, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
}

POINT PointToSource(POINT point) {
    POINT result{};
    result.x = std::clamp(static_cast<int>((point.x - g_sourceView.left) * g_sourceWidth / std::max<LONG>(1, g_sourceView.right - g_sourceView.left)), 0, g_sourceWidth - 1);
    result.y = std::clamp(static_cast<int>((point.y - g_sourceView.top) * g_sourceHeight / std::max<LONG>(1, g_sourceView.bottom - g_sourceView.top)), 0, g_sourceHeight - 1);
    return result;
}

HWND AddLabel(HWND parent, const wchar_t* text, int x, int y, int w) { return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20, parent, nullptr, nullptr, nullptr); }
HWND AddEdit(HWND parent, int id, const wchar_t* value, int x, int y, int w) { return CreateWindowW(L"EDIT", value, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, x, y, w, 24, parent, reinterpret_cast<HMENU>(id), nullptr, nullptr); }

HWND AddSlider(HWND parent, int id, int x, int y) {
    HWND slider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, x, y, 155, 28, parent, reinterpret_cast<HMENU>(id), nullptr, nullptr);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(-100, 100));
    SendMessageW(slider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(slider, TBM_SETPOS, TRUE, 0);
    return slider;
}

void UpdateAdjustmentLabels(HWND hwnd) {
    struct Item { int slider; int input; } items[] = {
        { ID_BRIGHTNESS, ID_BRIGHTNESS_INPUT }, { ID_CONTRAST, ID_CONTRAST_INPUT }, { ID_SATURATION, ID_SATURATION_INPUT }
    };
    g_syncingAdjustments = true;
    for (const Item& item : items) {
        int value = static_cast<int>(SendMessageW(GetDlgItem(hwnd, item.slider), TBM_GETPOS, 0, 0));
        wchar_t text[16]{}; swprintf(text, 16, L"%d", value);
        SetWindowTextW(GetDlgItem(hwnd, item.input), text);
    }
    g_syncingAdjustments = false;
}

void ResetAdjustments(HWND hwnd) {
    const int sliders[] = { ID_BRIGHTNESS, ID_CONTRAST, ID_SATURATION };
    for (int slider : sliders) SendMessageW(GetDlgItem(hwnd, slider), TBM_SETPOS, TRUE, 0);
    UpdateAdjustmentLabels(hwnd);
    if (!g_source.empty()) Render(hwnd, true);
}

bool UpdateSliderFromInput(HWND hwnd, int inputId, int sliderId) {
    wchar_t text[16]{};
    GetWindowTextW(GetDlgItem(hwnd, inputId), text, 15);
    if (text[0] == L'\0' || (text[0] == L'-' && text[1] == L'\0')) return false;
    int value = std::clamp(_wtoi(text), -100, 100);
    SendMessageW(GetDlgItem(hwnd, sliderId), TBM_SETPOS, TRUE, value);
    return true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"选择文件导入（图片 / GIF）", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 18, 190, 32, hwnd, reinterpret_cast<HMENU>(ID_LOAD), nullptr, nullptr);
        AddLabel(hwnd, L"裁剪比例（16:9、1:1）", 18, 72, 190); AddEdit(hwnd, ID_ASPECT, L"1:1", 18, 94, 190);
        AddLabel(hwnd, L"像素宽度", 18, 132, 80); AddEdit(hwnd, ID_WIDTH, L"64", 18, 154, 80);
        AddLabel(hwnd, L"像素高度", 120, 132, 80); AddEdit(hwnd, ID_HEIGHT, L"64", 120, 154, 80);
        AddLabel(hwnd, L"画布 X", 18, 192, 80); AddEdit(hwnd, ID_X, L"0", 18, 214, 80);
        AddLabel(hwnd, L"画布 Y", 120, 192, 80); AddEdit(hwnd, ID_Y, L"0", 120, 214, 80);
        AddLabel(hwnd, L"降采样方式", 18, 250, 190);
        HWND mode = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWNLIST, 18, 272, 190, 120, hwnd, reinterpret_cast<HMENU>(ID_MODE), nullptr, nullptr);
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"清晰最近邻（硬朗像素边缘）"));
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"多点降采样（保留原色，推荐）"));
        SendMessageW(mode, CB_SETCURSEL, 1, 0);
        CreateWindowW(L"BUTTON", L"重置居中裁剪框", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 314, 190, 30, hwnd, reinterpret_cast<HMENU>(ID_RESET_CROP), nullptr, nullptr);
        AddLabel(hwnd, L"亮度", 18, 352, 56); AddEdit(hwnd, ID_BRIGHTNESS_INPUT, L"0", 78, 348, 122); AddSlider(hwnd, ID_BRIGHTNESS, 18, 372);
        AddLabel(hwnd, L"对比度", 18, 414, 56); AddEdit(hwnd, ID_CONTRAST_INPUT, L"0", 78, 410, 122); AddSlider(hwnd, ID_CONTRAST, 18, 434);
        AddLabel(hwnd, L"饱和度", 18, 476, 56); AddEdit(hwnd, ID_SATURATION_INPUT, L"0", 78, 472, 122); AddSlider(hwnd, ID_SATURATION, 18, 496);
        CreateWindowW(L"BUTTON", L"增强参数归零", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 530, 190, 28, hwnd, reinterpret_cast<HMENU>(ID_RESET_ADJUST), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"导出 PNG（第一帧）", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 568, 190, 28, hwnd, reinterpret_cast<HMENU>(ID_PNG), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"导出 GIF 动图", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 606, 190, 28, hwnd, reinterpret_cast<HMENU>(ID_GIF), nullptr, nullptr);
        AddLabel(hwnd, L"原图裁剪：框内拖动可移动，框外拖动可重新选区。", 235, 22, 350);
        AddLabel(hwnd, L"64x64 LED 画布预览", 620, 22, 300);
        g_status = CreateWindowW(L"STATIC", L"增强参数默认均为 0，不会改变原图色彩。", WS_CHILD | WS_VISIBLE, 235, 630, 680, 24, hwnd, reinterpret_cast<HMENU>(ID_STATUS), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"导出 ESP32 内容 .pmx", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 644, 190, 28, hwnd, reinterpret_cast<HMENU>(ID_PMX), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"选择 PMX 并烧录到 COM20", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 18, 682, 190, 28, hwnd, reinterpret_cast<HMENU>(ID_FLASH_PMX), nullptr, nullptr);
        UpdateAdjustmentLabels(hwnd);
        g_chineseFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"SimSun");
        g_latinFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_ROMAN, L"Times New Roman");
        EnumChildWindows(hwnd, ApplyFontToChild, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notification = HIWORD(wParam);
        if (id == ID_LOAD) {
            wchar_t path[MAX_PATH]{};
            if (OpenFileDialog(hwnd, path, MAX_PATH,
                               L"所有支持的图片\0*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.webp\0"
                               L"GIF 动图\0*.gif\0"
                               L"PNG 图片\0*.png\0"
                               L"JPEG 图片\0*.jpg;*.jpeg\0"
                               L"BMP 图片\0*.bmp\0"
                               L"WebP 图片\0*.webp\0"
                               L"所有文件\0*.*\0\0", L"gif")) {
                std::wstring error;
                if (LoadFrames(path, error)) { g_inputName = path; ResetCrop(ReadAspect(hwnd)); FitPixelContentToAspect(hwnd, ReadAspect(hwnd)); Render(hwnd, true); }
                else SetStatus(error);
            }
        } else if (id == ID_RESET_CROP) { ResetCrop(ReadAspect(hwnd)); FitPixelContentToAspect(hwnd, ReadAspect(hwnd)); Render(hwnd, true); }
        else if (id == ID_GIF) ExportGif(hwnd);
        else if (id == ID_PNG) ExportPng(hwnd);
        else if (id == ID_RESET_ADJUST) ResetAdjustments(hwnd);
        else if (id == ID_PMX) ExportPmx(hwnd);
        else if (id == ID_FLASH_PMX) FlashSelectedPmx(hwnd);
        else if (!g_source.empty() && id == ID_ASPECT && notification == EN_CHANGE && HasCompleteAspect(hwnd)) { ResetCrop(ReadAspect(hwnd)); FitPixelContentToAspect(hwnd, ReadAspect(hwnd)); Render(hwnd, true); }
        else if (!g_source.empty() && id == ID_ASPECT && notification == EN_KILLFOCUS && HasCompleteAspect(hwnd)) { ResetCrop(ReadAspect(hwnd)); FitPixelContentToAspect(hwnd, ReadAspect(hwnd)); Render(hwnd, true); }
        else if (!g_source.empty() && ((id == ID_WIDTH || id == ID_HEIGHT || id == ID_X || id == ID_Y) && notification == EN_CHANGE)) Render(hwnd, true);
        else if (!g_source.empty() && id == ID_MODE && notification == CBN_SELCHANGE) Render(hwnd, true);
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_CHANGE && id == ID_BRIGHTNESS_INPUT && UpdateSliderFromInput(hwnd, ID_BRIGHTNESS_INPUT, ID_BRIGHTNESS)) Render(hwnd, true);
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_CHANGE && id == ID_CONTRAST_INPUT && UpdateSliderFromInput(hwnd, ID_CONTRAST_INPUT, ID_CONTRAST)) Render(hwnd, true);
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_CHANGE && id == ID_SATURATION_INPUT && UpdateSliderFromInput(hwnd, ID_SATURATION_INPUT, ID_SATURATION)) Render(hwnd, true);
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_KILLFOCUS && id == ID_BRIGHTNESS_INPUT && UpdateSliderFromInput(hwnd, ID_BRIGHTNESS_INPUT, ID_BRIGHTNESS)) { UpdateAdjustmentLabels(hwnd); Render(hwnd, true); }
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_KILLFOCUS && id == ID_CONTRAST_INPUT && UpdateSliderFromInput(hwnd, ID_CONTRAST_INPUT, ID_CONTRAST)) { UpdateAdjustmentLabels(hwnd); Render(hwnd, true); }
        else if (!g_source.empty() && !g_syncingAdjustments && notification == EN_KILLFOCUS && id == ID_SATURATION_INPUT && UpdateSliderFromInput(hwnd, ID_SATURATION_INPUT, ID_SATURATION)) { UpdateAdjustmentLabels(hwnd); Render(hwnd, true); }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC hdc = BeginPaint(hwnd, &paint);
        RECT client{}; GetClientRect(hwnd, &client);
        HDC buffer = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        DrawBackdrop(buffer, client);
        RECT sourcePreview{ 235, 55, 590, 585 }; DrawSourcePreview(buffer, sourcePreview);
        RECT preview{ 620, 70, 920, 500 }; DrawPreview(buffer, preview);
        BitBlt(hdc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);
        return reinterpret_cast<LRESULT>(g_darkBrush);
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kInput);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        SetBkColor(reinterpret_cast<HDC>(wParam), kInput);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType != ODT_BUTTON) return FALSE;
        bool pressed = (item->itemState & ODS_SELECTED) != 0;
        HBRUSH fill = CreateSolidBrush(pressed ? RGB(102, 74, 104) : RGB(59, 73, 103));
        FillRect(item->hDC, &item->rcItem, fill); DeleteObject(fill);
        HPEN outer = CreatePen(PS_SOLID, 2, kAccent); HPEN inner = CreatePen(PS_SOLID, 1, kMint);
        HGDIOBJ oldPen = SelectObject(item->hDC, outer); HGDIOBJ oldBrush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom);
        SelectObject(item->hDC, inner); Rectangle(item->hDC, item->rcItem.left + 3, item->rcItem.top + 3, item->rcItem.right - 3, item->rcItem.bottom - 3);
        SelectObject(item->hDC, oldBrush); SelectObject(item->hDC, oldPen); DeleteObject(outer); DeleteObject(inner);
        wchar_t text[128]{}; GetWindowTextW(item->hwndItem, text, 127);
        HGDIOBJ oldFont = SelectObject(item->hDC, g_chineseFont); SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, kText);
        DrawTextW(item->hDC, text, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, oldFont);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == 1 && g_canvas.size() > 1) {
            g_previewFrame = (g_previewFrame + 1) % g_canvas.size();
            SetTimer(hwnd, 1, std::max<UINT>(10, g_canvas[g_previewFrame].delayMs), nullptr);
            InvalidatePanels(hwnd, false, true);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!g_source.empty() && PtInRect(&g_sourceView, screen)) {
            POINT point = PointToSource(screen);
            POINT cropPoint{ point.x, point.y };
            if (PtInRect(&g_crop, cropPoint)) { g_movingCrop = true; g_moveOffset = POINT{ point.x - g_crop.left, point.y - g_crop.top }; }
            else { g_movingCrop = false; g_dragAnchor = point; g_crop = MakeCrop(point, point, ReadAspect(hwnd)); }
            g_dragging = true; SetCapture(hwnd); InvalidatePanels(hwnd, true, false);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging && (wParam & MK_LBUTTON) && !g_source.empty()) {
            POINT point = PointToSource(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (g_movingCrop) {
                int width = g_crop.right - g_crop.left, height = g_crop.bottom - g_crop.top;
                int left = std::clamp(static_cast<int>(point.x - g_moveOffset.x), 0, g_sourceWidth - width);
                int top = std::clamp(static_cast<int>(point.y - g_moveOffset.y), 0, g_sourceHeight - height);
                g_crop = RECT{ left, top, left + width, top + height };
            } else g_crop = MakeCrop(g_dragAnchor, point, ReadAspect(hwnd));
            Render(hwnd, true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = false; ReleaseCapture(); Render(hwnd); }
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, ID_BRIGHTNESS) || reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, ID_CONTRAST) || reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, ID_SATURATION)) {
            UpdateAdjustmentLabels(hwnd);
            if (!g_source.empty()) Render(hwnd, LOWORD(wParam) == TB_THUMBTRACK);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_chineseFont) { DeleteObject(g_chineseFont); g_chineseFont = nullptr; }
        if (g_latinFont) { DeleteObject(g_latinFont); g_latinFont = nullptr; }
        if (g_darkBrush) { DeleteObject(g_darkBrush); g_darkBrush = nullptr; }
        if (g_panelBrush) { DeleteObject(g_panelBrush); g_panelBrush = nullptr; }
        if (g_inputBrush) { DeleteObject(g_inputBrush); g_inputBrush = nullptr; }
        if (g_buttonBrush) { DeleteObject(g_buttonBrush); g_buttonBrush = nullptr; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput input; ULONG_PTR token = 0; GdiplusStartup(&token, &input, nullptr);
    g_darkBrush = CreateSolidBrush(kBackground);
    g_panelBrush = CreateSolidBrush(kPanel);
    g_inputBrush = CreateSolidBrush(kInput);
    g_buttonBrush = CreateSolidBrush(RGB(27, 53, 62));
    INITCOMMONCONTROLSEX common{}; common.dwSize = sizeof(common); common.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&common);
    WNDCLASSW window{}; window.hInstance = instance; window.lpszClassName = L"PixelMatrixStudio"; window.lpfnWndProc = WindowProc;
    window.hCursor = LoadCursor(nullptr, IDC_ARROW); window.hbrBackground = static_cast<HBRUSH>(GetStockObject(COLOR_WINDOW + 1));
    RegisterClassW(&window);
    HWND hwnd = CreateWindowW(window.lpszClassName, L"像素矩阵动图工具 - 微雪 64x64", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, 960, 760, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    GdiplusShutdown(token);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return 0;
}
