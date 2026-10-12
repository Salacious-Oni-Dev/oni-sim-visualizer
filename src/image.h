// A software framebuffer and a dependency-free PNG writer.
//
// The world is computed into one RGB buffer, one pixel per cell (views.h), and every PNG
// this program writes -- plain snapshots, composed ones, and the Qt proof sheets after
// qtui::FromQImage -- goes out through WritePng below. Owning the cell pixels is what lets
// `--snapshot` run with no window at all; the window (Qt) draws this same buffer scaled.
//
// The PNG is written with STORED deflate blocks -- valid zlib, no compression, no
// libpng and no zlib dependency. A 636x404 frame is ~775 KB, which is irrelevant for a
// debugging artifact and buys the build one fewer moving part.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace simviz {

struct Rgb {
  uint8_t r, g, b;
};

class Image {
 public:
  Image() = default;
  Image(int w, int h) { Reset(w, h); }

  void Reset(int w, int h) {
    width_ = w;
    height_ = h;
    px_.assign(static_cast<size_t>(w) * h, Rgb{0, 0, 0});
  }

  int width() const { return width_; }
  int height() const { return height_; }
  bool empty() const { return px_.empty(); }
  Rgb* data() { return px_.data(); }
  const Rgb* data() const { return px_.data(); }

  bool InBounds(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
  }

  void Set(int x, int y, Rgb c) {
    if (InBounds(x, y)) px_[static_cast<size_t>(y) * width_ + x] = c;
  }

  Rgb Get(int x, int y) const {
    if (!InBounds(x, y)) return Rgb{0, 0, 0};
    return px_[static_cast<size_t>(y) * width_ + x];
  }

  void Fill(Rgb c) {
    for (Rgb& p : px_) p = c;
  }

  void FillRect(int x, int y, int w, int h, Rgb c) {
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) Set(x + i, y + j, c);
  }

  // Integer nearest-neighbour magnification. Used by --zoom so a handful of cells can
  // be looked at without an image viewer's own resampling getting in the way.
  Image Magnified(int factor) const {
    if (factor <= 1) return *this;
    Image out(width_ * factor, height_ * factor);
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        const Rgb c = px_[static_cast<size_t>(y) * width_ + x];
        for (int j = 0; j < factor; ++j)
          for (int i = 0; i < factor; ++i)
            out.Set(x * factor + i, y * factor + j, c);
      }
    }
    return out;
  }

  Image Cropped(int x0, int y0, int w, int h) const {
    Image out(w, h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) out.Set(x, y, Get(x0 + x, y0 + y));
    return out;
  }

 private:
  int width_ = 0, height_ = 0;
  std::vector<Rgb> px_;
};

// ------------------------------------------------------------------ PNG encoding

namespace png {

inline uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

inline uint32_t Adler32(const uint8_t* data, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

inline void PutBe32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v >> 24));
  out->push_back(static_cast<uint8_t>(v >> 16));
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

inline void Chunk(std::vector<uint8_t>* out, const char tag[4],
                  const std::vector<uint8_t>& payload) {
  PutBe32(out, static_cast<uint32_t>(payload.size()));
  const size_t crc_start = out->size();
  out->insert(out->end(), tag, tag + 4);
  out->insert(out->end(), payload.begin(), payload.end());
  const uint32_t crc =
      Crc32(out->data() + crc_start, out->size() - crc_start) ^ 0xFFFFFFFFu;
  PutBe32(out, crc);
}

}  // namespace png

// Writes `img` as an 8-bit RGB PNG. Returns false only if the file cannot be opened.
inline bool WritePng(const Image& img, const std::string& path) {
  const int w = img.width(), h = img.height();
  if (w <= 0 || h <= 0) return false;

  // Raw scanlines, each preceded by filter byte 0 (None).
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const Rgb c = img.Get(x, y);
      raw.push_back(c.r);
      raw.push_back(c.g);
      raw.push_back(c.b);
    }
  }

  // zlib stream: 0x78 0x01 (deflate, 32K window, no preset dict, fastest level),
  // then stored blocks, then the adler32 of the uncompressed data.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    const size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
    const bool final = (off + n) >= raw.size();
    z.push_back(final ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n & 0xFF));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n & 0xFF));
    z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
  }
  png::PutBe32(&z, png::Adler32(raw.data(), raw.size()));

  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

  std::vector<uint8_t> ihdr;
  png::PutBe32(&ihdr, static_cast<uint32_t>(w));
  png::PutBe32(&ihdr, static_cast<uint32_t>(h));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: truecolour RGB
  ihdr.push_back(0);  // compression: deflate
  ihdr.push_back(0);  // filter: adaptive
  ihdr.push_back(0);  // interlace: none
  png::Chunk(&out, "IHDR", ihdr);
  png::Chunk(&out, "IDAT", z);
  png::Chunk(&out, "IEND", {});

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

}  // namespace simviz
