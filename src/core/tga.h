// TGA (Truevision TARGA) decoder covering the whole TGA 1.0 / 2.0 image spec:
// color-mapped, true-color and grayscale images, raw or RLE, 8/15/16/24/32 bits per pixel.
// Written from the public format description; no third-party code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tga {

enum class Alpha {
    None,           // opaque image
    Straight,       // alpha channel in use
    Premultiplied,  // TGA 2.0 attribute type 4; undone on decode
};

struct Info {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t imageType = 0;    // 1 color-mapped, 2 true-color, 3 grayscale; +8 when RLE
    uint8_t pixelDepth = 0;   // bits per stored pixel (or per color-map index)
    uint8_t mapEntryBits = 0; // bits per color-map entry
    uint8_t alphaBits = 0;    // from the image descriptor
    bool topDown = false;
    bool rightToLeft = false;
    bool rle = false;
    Alpha alpha = Alpha::None;
    size_t mapOffset = 0;     // color map position in the file
    size_t dataOffset = 0;    // pixel data position in the file
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // top-down, straight alpha, stride = width * 4
};

// Reads and validates the header (and TGA 2.0 footer). Pixel data is not touched.
bool parse(const uint8_t* data, size_t size, Info& info, std::string* error = nullptr);

// Decodes the image to BGRA.
bool decode(const uint8_t* data, size_t size, const Info& info, Image& out, std::string* error = nullptr);

// Short description such as "24-bit RLE" or "8-bit color-mapped".
std::string describe(const Info& info);

}  // namespace tga
