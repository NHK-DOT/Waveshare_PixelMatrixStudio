#include <array>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iostream>

#pragma pack(push, 1)
struct PmxHeader {
    char magic[4];
    uint8_t version;
    uint8_t format;
    uint8_t width;
    uint8_t height;
    uint32_t frame_count;
    uint32_t directory_offset;
    uint32_t data_offset;
    uint32_t file_size;
};

struct PmxFrameEntry {
    uint32_t delay_ms;
    uint32_t data_offset;
};
#pragma pack(pop)

static_assert(sizeof(PmxHeader) == 24);
static_assert(sizeof(PmxFrameEntry) == 8);

int main(int argc, char **argv)
{
    const char *output_path = argc > 1 ? argv[1] : "white_test.pmx";
    uint8_t pixel_color = 0xFF;
    if (argc > 2 && std::strcmp(argv[2], "blue") == 0) pixel_color = 0x03;
    if (argc > 2 && std::strcmp(argv[2], "red") == 0) pixel_color = 0xE0;
    if (argc > 2 && std::strcmp(argv[2], "green") == 0) pixel_color = 0x1C;
    constexpr uint32_t kPixelBytes = 64 * 64;
    constexpr uint32_t kDataOffset = sizeof(PmxHeader) + sizeof(PmxFrameEntry);
    const PmxHeader header = {{'P', 'M', 'X', '1'}, 1, 1, 64, 64, 1,
                              sizeof(PmxHeader), kDataOffset, kDataOffset + kPixelBytes};
    const PmxFrameEntry frame = {1000, kDataOffset};
    std::array<uint8_t, kPixelBytes> white_pixels{};
    white_pixels.fill(pixel_color);

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        std::cerr << "Unable to create " << output_path << '\n';
        return 1;
    }
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(&frame), sizeof(frame));
    output.write(reinterpret_cast<const char *>(white_pixels.data()), white_pixels.size());
    if (!output.good()) {
        std::cerr << "Failed to write " << output_path << '\n';
        return 1;
    }
    std::cout << "Wrote " << output_path << " with " << white_pixels.size() << " full-white RGB332 pixels\n";
    return 0;
}
