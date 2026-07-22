#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

#pragma pack(push, 1)
struct IconHeader { uint16_t reserved, type, count; };
struct IconEntry { uint8_t width, height, colors, reserved; uint16_t planes, bits; uint32_t bytes, offset; };
struct BitmapHeader { uint32_t size; int32_t width, height; uint16_t planes, bits; uint32_t compression, imageSize; int32_t xppm, yppm; uint32_t colors, important; };
#pragma pack(pop)

int main() {
    constexpr int size = 32;
    std::array<uint8_t, size * size * 4> pixels{};
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= size || y < 0 || y >= size) return;
        int index = ((size - 1 - y) * size + x) * 4;
        pixels[index] = b; pixels[index + 1] = g; pixels[index + 2] = r; pixels[index + 3] = 255;
    };
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) put(x, y, 13, 19, 25);

    for (int y = 2; y < 30; ++y) for (int x = 2; x < 30; ++x) {
        float dx = (x - 16.0f) / 1.0f, dy = (y - 16.0f) / 0.78f;
        float ring = std::sqrt(dx * dx + dy * dy);
        if (ring > 9.0f && ring < 13.0f) {
            float shade = (dy + 17.0f) / 34.0f;
            put(x, y, static_cast<uint8_t>(37 + 40 * shade), static_cast<uint8_t>(112 + 90 * shade), static_cast<uint8_t>(127 + 92 * shade));
        }
    }
    for (int y = 4; y < 28; ++y) for (int x = 4; x < 28; ++x) {
        float diagonal = std::fabs((y - 16.0f) - (x - 16.0f) * 0.55f);
        float centered = std::sqrt((x - 16.0f) * (x - 16.0f) + (y - 16.0f) * (y - 16.0f));
        if (diagonal < 2.6f && centered < 14.2f) {
            uint8_t shade = static_cast<uint8_t>(150 + std::max(0.0f, 65.0f - std::fabs(x - 16.0f) * 4.0f));
            put(x, y, shade, static_cast<uint8_t>(120 + shade / 4), 42);
        }
    }
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        float edge = std::sqrt((x - 16.0f) * (x - 16.0f) + (y - 16.0f) * (y - 16.0f));
        if (edge > 14.5f) put(x, y, 13, 19, 25);
    }

    constexpr uint32_t imageBytes = size * size * 4;
    constexpr uint32_t maskBytes = size * 4 * size;
    IconHeader header{ 0, 1, 1 };
    IconEntry entry{ size, size, 0, 0, 1, 32, static_cast<uint32_t>(sizeof(BitmapHeader) + imageBytes + maskBytes), static_cast<uint32_t>(sizeof(IconHeader) + sizeof(IconEntry)) };
    BitmapHeader bitmap{ sizeof(BitmapHeader), size, size * 2, 1, 32, 0, imageBytes, 0, 0, 0, 0 };
    std::array<uint8_t, maskBytes> mask{};
    std::ofstream out("pixel_mobius.ico", std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    out.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    out.write(reinterpret_cast<const char*>(mask.data()), mask.size());
    return out ? 0 : 1;
}
