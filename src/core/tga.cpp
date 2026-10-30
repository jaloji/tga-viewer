#include "tga.h"

#include <cstring>

namespace tga {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

constexpr size_t kHeader = 18;
constexpr size_t kFooter = 26;
const char kSignature[] = "TRUEVISION-XFILE.";  // followed by '\0'

uint8_t expand5(unsigned v) { return uint8_t((v << 3) | (v >> 2)); }

// Converts one stored value (pixel or color-map entry) of the given depth to BGRA.
void toBgra(const uint8_t* p, unsigned bits, bool gray, uint8_t out[4]) {
    if (gray) {
        out[0] = out[1] = out[2] = p[0];
        out[3] = bits == 16 ? p[1] : 255;
        return;
    }
    switch (bits) {
        case 15:
        case 16: {
            const uint16_t v = rd16(p);
            out[0] = expand5(v & 31);
            out[1] = expand5((v >> 5) & 31);
            out[2] = expand5((v >> 10) & 31);
            out[3] = (v & 0x8000) ? 255 : 0;
            break;
        }
        case 24:
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
            break;
        default:  // 32
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
            break;
    }
}

}  // namespace

bool parse(const uint8_t* data, size_t size, Info& info, std::string* error) {
    info = Info();
    if (size < kHeader) return fail(error, "file too small");

    const uint8_t idLength = data[0];
    const uint8_t mapType = data[1];
    const uint8_t type = data[2];
    const uint16_t mapFirst = rd16(data + 3);
    const uint16_t mapLength = rd16(data + 5);
    const uint8_t mapBits = data[7];
    info.width = rd16(data + 12);
    info.height = rd16(data + 14);
    info.pixelDepth = data[16];
    const uint8_t descriptor = data[17];
    (void)mapFirst;

    info.imageType = type;
    info.rle = type >= 9;
    info.alphaBits = descriptor & 0x0F;
    info.rightToLeft = (descriptor & 0x10) != 0;
    info.topDown = (descriptor & 0x20) != 0;
    if (descriptor & 0xC0) return fail(error, "interleaved TGA not supported");
    if (info.width == 0 || info.height == 0) return fail(error, "empty image");
    if (mapType > 1) return fail(error, "not a TGA file");

    const unsigned base = type & 7;
    switch (base) {
        case 1:  // color-mapped
            if (type != 1 && type != 9) return fail(error, "not a TGA file");
            if (mapType != 1 || mapLength == 0) return fail(error, "missing color map");
            if (info.pixelDepth != 8 && info.pixelDepth != 16) return fail(error, "bad index size");
            if (mapBits != 15 && mapBits != 16 && mapBits != 24 && mapBits != 32) return fail(error, "bad color map entry size");
            info.mapEntryBits = mapBits;
            break;
        case 2:  // true-color
            if (type != 2 && type != 10) return fail(error, "not a TGA file");
            if (info.pixelDepth != 15 && info.pixelDepth != 16 && info.pixelDepth != 24 && info.pixelDepth != 32)
                return fail(error, "bad pixel depth");
            break;
        case 3:  // grayscale
            if (type != 3 && type != 11) return fail(error, "not a TGA file");
            if (info.pixelDepth != 8 && info.pixelDepth != 16) return fail(error, "bad pixel depth");
            break;
        default:
            return fail(error, "not a TGA file");
    }
    // A color map may be present (and skipped) even for non color-mapped images.
    if (mapType == 1 && mapBits != 15 && mapBits != 16 && mapBits != 24 && mapBits != 32 && mapBits != 0)
        return fail(error, "bad color map entry size");

    info.mapOffset = kHeader + idLength;
    const size_t mapBytes = mapType == 1 ? size_t(mapLength) * ((mapBits + 7) / 8) : 0;
    info.dataOffset = info.mapOffset + mapBytes;
    if (info.dataOffset > size) return fail(error, "truncated header");

    // The pixel data must be able to hold the announced image. TGA has no signature, so this
    // also keeps other formats whose first bytes look like a TGA header from being taken as one.
    const size_t pixelBytes = (info.pixelDepth + 7) / 8;
    const size_t count = size_t(info.width) * info.height;
    const size_t minData = info.rle ? (count + 127) / 128 * (1 + pixelBytes) : count * pixelBytes;
    if (size - info.dataOffset < minData) return fail(error, "truncated pixel data");

    // Which kind of alpha the pixels carry.
    const unsigned depth = base == 1 ? info.mapEntryBits : info.pixelDepth;
    bool hasAlpha;
    if (base == 3) hasAlpha = depth == 16;
    else if (depth == 32) hasAlpha = true;
    else if (depth == 16) hasAlpha = info.alphaBits == 1;  // otherwise the top bit is unused
    else hasAlpha = false;
    info.alpha = hasAlpha ? Alpha::Straight : Alpha::None;

    // TGA 2.0 footer: the extension area states whether the alpha is meaningful.
    if (size >= kHeader + kFooter && std::memcmp(data + size - 18, kSignature, 18) == 0) {
        const uint32_t ext = rd32(data + size - kFooter);
        if (ext != 0 && size_t(ext) + 495 <= size - kFooter && rd16(data + ext) >= 495) {
            const uint8_t attributes = data[ext + 494];
            if (attributes == 0 || attributes == 1 || attributes == 2) info.alpha = Alpha::None;
            else if (attributes == 4 && hasAlpha) info.alpha = Alpha::Premultiplied;
        }
    }
    return true;
}

bool decode(const uint8_t* data, size_t size, const Info& info, Image& out, std::string* error) {
    const unsigned base = info.imageType & 7;
    const bool gray = base == 3;
    const size_t bytesPerPixel = (info.pixelDepth + 7) / 8;
    const size_t count = size_t(info.width) * info.height;

    // Color map converted to BGRA once.
    std::vector<uint8_t> palette;
    uint16_t mapFirst = 0;
    if (base == 1) {
        mapFirst = rd16(data + 3);
        const uint16_t mapLength = rd16(data + 5);
        const size_t entryBytes = (info.mapEntryBits + 7) / 8;
        palette.resize(size_t(mapLength) * 4);
        for (size_t i = 0; i < mapLength; ++i)
            toBgra(data + info.mapOffset + i * entryBytes, info.mapEntryBits, false, &palette[i * 4]);
    }

    // Stored pixels in file order, after RLE expansion.
    std::vector<uint8_t> raw;
    const uint8_t* pixels;
    if (info.rle) {
        raw.resize(count * bytesPerPixel);
        size_t ip = info.dataOffset, op = 0;
        const size_t total = raw.size();
        while (op < total) {
            if (ip >= size) return fail(error, "truncated RLE data");
            const uint8_t packet = data[ip++];
            const size_t n = size_t(packet & 0x7F) + 1;
            if (op + n * bytesPerPixel > total) return fail(error, "corrupt RLE data");
            if (packet & 0x80) {
                if (ip + bytesPerPixel > size) return fail(error, "truncated RLE data");
                for (size_t k = 0; k < n; ++k, op += bytesPerPixel) std::memcpy(&raw[op], data + ip, bytesPerPixel);
                ip += bytesPerPixel;
            } else {
                if (ip + n * bytesPerPixel > size) return fail(error, "truncated RLE data");
                std::memcpy(&raw[op], data + ip, n * bytesPerPixel);
                ip += n * bytesPerPixel;
                op += n * bytesPerPixel;
            }
        }
        pixels = raw.data();
    } else {
        if (info.dataOffset + count * bytesPerPixel > size) return fail(error, "truncated pixel data");
        pixels = data + info.dataOffset;
    }

    out.width = info.width;
    out.height = info.height;
    out.bgra.assign(count * 4, 0);
    bool anyAlpha = false;
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint32_t dy = info.topDown ? y : info.height - 1 - y;
        for (uint32_t x = 0; x < info.width; ++x) {
            const uint32_t dx = info.rightToLeft ? info.width - 1 - x : x;
            const uint8_t* src = pixels + (size_t(y) * info.width + x) * bytesPerPixel;
            uint8_t* dst = &out.bgra[(size_t(dy) * info.width + dx) * 4];
            if (base == 1) {
                const size_t index = (info.pixelDepth == 16 ? rd16(src) : src[0]);
                if (index >= mapFirst && (index - mapFirst) * 4 < palette.size())
                    std::memcpy(dst, &palette[(index - mapFirst) * 4], 4);
                else
                    dst[3] = 255;  // out-of-range index: opaque black
            } else {
                toBgra(src, info.pixelDepth, gray, dst);
            }
            if (info.alpha == Alpha::None) dst[3] = 255;
            else if (dst[3] != 0) anyAlpha = true;
        }
    }

    if (info.alpha != Alpha::None && !anyAlpha) {
        // An alpha channel that is zero everywhere was never filled in: show the image opaque.
        for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 255;
    } else if (info.alpha == Alpha::Premultiplied) {
        for (size_t i = 0; i < out.bgra.size(); i += 4) {
            const unsigned a = out.bgra[i + 3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; ++c) {
                const unsigned v = (out.bgra[i + c] * 255u + a / 2) / a;
                out.bgra[i + c] = uint8_t(v > 255 ? 255 : v);
            }
        }
    }
    return true;
}

std::string describe(const Info& info) {
    const unsigned base = info.imageType & 7;
    std::string s = std::to_string(base == 1 ? info.mapEntryBits : info.pixelDepth) + "-bit";
    if (base == 1) s += " color-mapped";
    else if (base == 3) s += " grayscale";
    if (info.rle) s += " RLE";
    return s;
}

}  // namespace tga
