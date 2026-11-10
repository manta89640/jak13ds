/*!
 * @file CtrButtonArt.cpp
 * (AI-assisted) See CtrButtonArt.h.
 *
 * The art: a disc in the button's color (lighter at the top), a dark rim, and the letter as
 * rounded strokes in white with a darker edge (it stays readable on the yellow B). Each texel is
 * sampled 4 x 4 times (antialiased edges). Everything is drawn in the coordinates of the box the
 * PS2 disc filled, which the game shows as a circle whatever the texel aspect is.
 *
 * OPENGOAL_CTR_ART_DUMP=<folder> (PC): the patched textures as raw RGBA files, for checking the art.
 */

#include "CtrButtonArt.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

namespace ctr_button_art {
namespace {
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_jak2{false};
std::atomic<u32> g_gen{1};
std::atomic<u64> g_tex[(int)Ps2Button::COUNT];  // (psm << 32) | (tbp0 + 1), 0 = none

enum class Face { A, B, X, Y };

// by position (platform/3ds/port/ctr_port.c ctr_pad_read): circle = A, cross = B,
// triangle = X, square = Y
Face face_of(Ps2Button b) {
  switch (b) {
    case Ps2Button::CROSS:
      return Face::B;
    case Ps2Button::SQUARE:
      return Face::Y;
    case Ps2Button::TRIANGLE:
      return Face::X;
    default:
      return Face::A;
  }
}

struct Rgb {
  float r, g, b;
};

// the New 3DS (small model) buttons
Rgb face_color(Face f) {
  switch (f) {
    case Face::A:
      return {222, 44, 48};
    case Face::B:
      return {242, 188, 24};
    case Face::X:
      return {46, 96, 206};
    default:
      return {44, 164, 76};
  }
}

struct Seg {
  float x0, y0, x1, y1;
};

// letters in a box of -0.5..0.5 (y down)
int letter_segs(Face f, Seg* out) {
  int n = 0;
  auto add = [&](float a, float b, float c, float d) { out[n++] = {a, b, c, d}; };
  switch (f) {
    case Face::A:
      add(-0.38f, 0.5f, 0.f, -0.5f);
      add(0.f, -0.5f, 0.38f, 0.5f);
      add(-0.2f, 0.16f, 0.2f, 0.16f);
      break;
    case Face::X:
      add(-0.36f, -0.5f, 0.36f, 0.5f);
      add(0.36f, -0.5f, -0.36f, 0.5f);
      break;
    case Face::Y:
      add(-0.38f, -0.5f, 0.f, 0.02f);
      add(0.38f, -0.5f, 0.f, 0.02f);
      add(0.f, 0.02f, 0.f, 0.5f);
      break;
    case Face::B: {
      add(-0.3f, -0.5f, -0.3f, 0.5f);
      // two bowls: half circles right of x = 0 (top) and 0.05 (bottom)
      const float pi = 3.14159265f;
      for (int bowl = 0; bowl < 2; bowl++) {
        const float cy = bowl ? 0.25f : -0.25f, r = 0.25f;
        const float cx = bowl ? 0.05f : 0.f;
        add(-0.3f, cy - r, cx, cy - r);
        add(-0.3f, cy + r, cx, cy + r);
        const int steps = 6;
        for (int i = 0; i < steps; i++) {
          const float a0 = -pi / 2 + pi * i / steps, a1 = -pi / 2 + pi * (i + 1) / steps;
          add(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1),
              cy + r * std::sin(a1));
        }
      }
      break;
    }
  }
  return n;
}

float seg_dist(const Seg& s, float x, float y) {
  const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? ((x - s.x0) * dx + (y - s.y0) * dy) / l2 : 0.f;
  t = std::clamp(t, 0.f, 1.f);
  const float ex = x - (s.x0 + t * dx), ey = y - (s.y0 + t * dy);
  return std::sqrt(ex * ex + ey * ey);
}

struct Sample {
  float r, g, b, a;  // straight color 0..255, alpha 0..1
};

// u, v: -1..1 over the button's box
Sample shade(Face f, const Seg* segs, int nsegs, float u, float v) {
  const float d = std::sqrt(u * u + v * v);
  if (d > 1.f) {
    return {0, 0, 0, 0};
  }
  constexpr float kFill = 0.84f;  // the rim is the rest
  if (d > kFill) {
    return {28, 28, 32, 1.f};
  }
  const Rgb c = face_color(f);
  // lighter at the top, a little darker at the bottom
  const float shade_k = 1.12f - 0.22f * (v + 1.f) * 0.5f;
  Sample s{std::min(255.f, c.r * shade_k), std::min(255.f, c.g * shade_k),
           std::min(255.f, c.b * shade_k), 1.f};
  // the letter: 1.14 of the box's half height tall (57% of the button), centered
  constexpr float kLetter = 1.14f;
  const float lx = u / kLetter, ly = v / kLetter;
  float dist = 10.f;
  for (int i = 0; i < nsegs; i++) {
    dist = std::min(dist, seg_dist(segs[i], lx, ly) * kLetter);
  }
  constexpr float kStroke = 0.14f, kEdge = 0.04f;
  if (dist < kStroke) {
    s.r = s.g = s.b = 252.f;
  } else if (dist < kStroke + kEdge) {
    s.r = c.r * 0.45f;
    s.g = c.g * 0.45f;
    s.b = c.b * 0.45f;
  }
  return s;
}

struct Box {
  float x0, y0, x1, y1;  // texels (x1, y1 exclusive)
};

/*!
 * Draw the button of face f into rgba (w wide), in the box b, writing only texels inside the
 * clip rectangle.
 */
void draw_button(u32* rgba, int w, Face f, const Box& b, int cx0, int cy0, int cx1, int cy1) {
  Seg segs[32];
  const int nsegs = letter_segs(f, segs);
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const float rx = (b.x1 - b.x0) * 0.5f, ry = (b.y1 - b.y0) * 0.5f;
  constexpr int kSs = 4;
  for (int y = cy0; y < cy1; y++) {
    for (int x = cx0; x < cx1; x++) {
      float pr = 0, pg = 0, pb = 0, pa = 0;  // premultiplied sums
      for (int sy = 0; sy < kSs; sy++) {
        for (int sx = 0; sx < kSs; sx++) {
          const float px = x + (sx + 0.5f) / kSs, py = y + (sy + 0.5f) / kSs;
          const Sample s = shade(f, segs, nsegs, (px - cx) / rx, (py - cy) / ry);
          pr += s.r * s.a;
          pg += s.g * s.a;
          pb += s.b * s.a;
          pa += s.a;
        }
      }
      u32 out = 0;
      if (pa > 0) {
        const float inv = 1.f / pa;
        const u32 r = (u32)std::clamp(pr * inv + 0.5f, 0.f, 255.f);
        const u32 g = (u32)std::clamp(pg * inv + 0.5f, 0.f, 255.f);
        const u32 bl = (u32)std::clamp(pb * inv + 0.5f, 0.f, 255.f);
        const u32 a = (u32)std::clamp(pa / (kSs * kSs) * 128.f + 0.5f, 0.f, 128.f);
        out = r | (g << 8) | (bl << 16) | (a << 24);
      }
      rgba[x + y * w] = out;
    }
  }
}

// the bounding box of the texels with alpha >= min_a in [x0, x1) x [y0, y1); false if none
bool alpha_box(const u32* rgba, int w, int x0, int y0, int x1, int y1, u32 min_a, Box* out) {
  int bx0 = x1, by0 = y1, bx1 = x0 - 1, by1 = y0 - 1;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      if ((rgba[x + y * w] >> 24) >= min_a) {
        bx0 = std::min(bx0, x);
        by0 = std::min(by0, y);
        bx1 = std::max(bx1, x);
        by1 = std::max(by1, y);
      }
    }
  }
  if (bx1 < bx0) {
    return false;
  }
  *out = {(float)bx0, (float)by0, (float)(bx1 + 1), (float)(by1 + 1)};
  return true;
}

// the cell of character c in a font atlas (font.gc *font12-table* / *font24-table*: 10 cells of
// 0.09375 x 0.0625 per row, from character 16)
void cell_of(int c, int w, int h, int* x0, int* y0, int* x1, int* y1) {
  const int i = c - 16, col = i % 10, row = i / 10;
  *x0 = (int)std::floor(col * 0.09375f * w);
  *x1 = std::min(w, (int)std::floor((col + 1) * 0.09375f * w));
  *y0 = (int)std::floor(row * 0.0625f * h);
  *y1 = std::min(h, (int)std::floor((row + 1) * 0.0625f * h));
}

void clear_rect(u32* rgba, int w, int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      rgba[x + y * w] = 0;
    }
  }
}

bool patch_font(u32* rgba, int w, int h) {
  if (w < 64 || h < 64) {
    return false;
  }
  // the PS2 disc ('<') tells where the button goes in a cell
  int dx0, dy0, dx1, dy1;
  cell_of('<', w, h, &dx0, &dy0, &dx1, &dy1);
  Box disc;
  if (!alpha_box(rgba, w, dx0, dy0, dx1, dy1, 0x40, &disc)) {
    return false;
  }
  for (int c : {'<', '>', '['}) {
    int x0, y0, x1, y1;
    cell_of(c, w, h, &x0, &y0, &x1, &y1);
    clear_rect(rgba, w, x0, y0, x1, y1);
  }
  const struct {
    int c;
    Ps2Button b;
  } symbols[] = {{'*', Ps2Button::CROSS},
                 {'#', Ps2Button::SQUARE},
                 {';', Ps2Button::TRIANGLE},
                 {'@', Ps2Button::CIRCLE}};
  for (const auto& s : symbols) {
    int x0, y0, x1, y1;
    cell_of(s.c, w, h, &x0, &y0, &x1, &y1);
    clear_rect(rgba, w, x0, y0, x1, y1);
    // the disc's box, moved from its cell to this one (half a texel bigger: the rim's outside)
    const Box b{disc.x0 - dx0 + x0 - 0.5f, disc.y0 - dy0 + y0 - 0.5f, disc.x1 - dx0 + x0 + 0.5f,
                disc.y1 - dy0 + y0 + 0.5f};
    draw_button(rgba, w, face_of(s.b), b, x0, y0, x1, y1);
  }
  return true;
}

// Jak II: the cell of character c (font-data.gc: entry c - 6 of 10 per row, cells of 0.09375 x
// 0.03125 of the page)
void cell_of_jak2(int c, int w, int h, int* x0, int* y0, int* x1, int* y1) {
  const int i = c - 6, col = i % 10, row = i / 10;
  *x0 = (int)std::floor(col * 0.09375f * w);
  *x1 = std::min(w, (int)std::floor((col + 1) * 0.09375f * w));
  *y0 = (int)std::floor(row * 0.03125f * h);
  *y1 = std::min(h, (int)std::floor((row + 1) * 0.03125f * h));
}

/*!
 * Jak II: the font's capitals `letters` side by side into the cell of character dst, as tall as
 * what the cell held (its texels of alpha 0x40 and more) and centered on it; the rest of the cell
 * cleared. Each texel is the average of 4 x 4 samples of the letters' texels. False (nothing
 * changed): the cell or a letter's is empty.
 */
bool stamp_letters(u32* rgba, int w, int h, const char* letters, int dst) {
  int dx0, dy0, dx1, dy1;
  cell_of_jak2(dst, w, h, &dx0, &dy0, &dx1, &dy1);
  Box box;
  if (!alpha_box(rgba, w, dx0, dy0, dx1, dy1, 0x40, &box)) {
    return false;
  }
  struct Letter {
    int cx0, cy0, cx1, cy1;  // its cell
    Box ink;
    float x;  // where its ink starts in the destination
  } ls[4];
  int n = 0;
  float ink_h = 0;
  for (const char* c = letters; *c && n < 4; c++, n++) {
    Letter& l = ls[n];
    cell_of_jak2(*c, w, h, &l.cx0, &l.cy0, &l.cx1, &l.cy1);
    if (!alpha_box(rgba, w, l.cx0, l.cy0, l.cx1, l.cy1, 0x40, &l.ink)) {
      return false;
    }
    ink_h = std::max(ink_h, l.ink.y1 - l.ink.y0);
  }
  const float k = (box.y1 - box.y0) / ink_h;  // destination texels per letter texel
  const float gap = n > 1 ? std::max(1.f, 2.f * k) : 0.f;
  float total = gap * (n - 1);
  for (int i = 0; i < n; i++) {
    total += (ls[i].ink.x1 - ls[i].ink.x0) * k;
  }
  float x = (box.x0 + box.x1) * 0.5f - total * 0.5f;
  for (int i = 0; i < n; i++) {
    ls[i].x = x;
    x += (ls[i].ink.x1 - ls[i].ink.x0) * k + gap;
  }
  constexpr int kSs = 4;
  for (int y = dy0; y < dy1; y++) {
    for (int px = dx0; px < dx1; px++) {
      float pr = 0, pg = 0, pb = 0, pa = 0;  // premultiplied sums
      for (int sy = 0; sy < kSs; sy++) {
        for (int sx = 0; sx < kSs; sx++) {
          const float fx = px + (sx + 0.5f) / kSs, fy = y + (sy + 0.5f) / kSs;
          for (int i = 0; i < n; i++) {
            const Letter& l = ls[i];
            // the letter's texel under the sample (a texel around its ink: the soft edge)
            const int u = (int)std::floor(l.ink.x0 + (fx - l.x) / k);
            const int v = (int)std::floor(l.ink.y0 + (fy - box.y0) / k);
            if (u < l.cx0 || u >= l.cx1 || v < l.cy0 || v >= l.cy1 || u < (int)l.ink.x0 - 1 ||
                u > (int)l.ink.x1 || v < (int)l.ink.y0 - 1 || v > (int)l.ink.y1) {
              continue;
            }
            const u32 t = rgba[u + v * w];
            const float a = (float)(t >> 24);
            pr += (float)(t & 0xff) * a;
            pg += (float)((t >> 8) & 0xff) * a;
            pb += (float)((t >> 16) & 0xff) * a;
            pa += a;
            break;
          }
        }
      }
      u32 out = 0;
      if (pa > 0) {
        const float inv = 1.f / pa;
        const u32 r = (u32)std::clamp(pr * inv + 0.5f, 0.f, 255.f);
        const u32 g = (u32)std::clamp(pg * inv + 0.5f, 0.f, 255.f);
        const u32 bl = (u32)std::clamp(pb * inv + 0.5f, 0.f, 255.f);
        const u32 a = (u32)std::clamp(pa / (kSs * kSs) + 0.5f, 0.f, 128.f);
        out = a ? r | (g << 8) | (bl << 16) | (a << 24) : 0;
      }
      rgba[px + y * w] = out;
    }
  }
  return true;
}

// Jak II: by position (platform/3ds/port/ctr_port.c map_buttons): cross B, square Y, triangle X,
// circle A; L1 L, R1 R, L2 ZL, R2 ZR
bool patch_font_jak2(u32* rgba, int w, int h) {
  if (w < 64 || h < 64) {
    return false;
  }
  const struct {
    int cell;
    const char* letters;
  } map[] = {{'*', "B"},  {'#', "Y"},  {';', "X"},   {'@', "A"},
             {0x95, "L"}, {0x94, "R"}, {0x97, "ZL"}, {0x96, "ZR"}};
  bool any = false;
  for (const auto& m : map) {
    any |= stamp_letters(rgba, w, h, m.letters, m.cell);
  }
  return any;
}

bool patch_icon(Ps2Button button, u32* rgba, int w, int h) {
  // the PS2 icon's own disc (its opaque part) is where the button goes
  Box b;
  if (!alpha_box(rgba, w, 0, 0, w, h, 0x40, &b)) {
    b = {0, 0, (float)w, (float)h};
  }
  // a square box around its center: the icons are drawn square
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const float r = std::max(b.x1 - b.x0, b.y1 - b.y0) * 0.5f;
  const Box sq{cx - r, cy - r, cx + r, cy + r};
  clear_rect(rgba, w, 0, 0, w, h);
  draw_button(rgba, w, face_of(button), sq, 0, 0, w, h);
  return true;
}

void dump(const char* what, u64 tex0, const u32* rgba, int w, int h) {
  const char* dir = getenv("OPENGOAL_CTR_ART_DUMP");
  if (!dir) {
    return;
  }
  char path[512];
  snprintf(path, sizeof(path), "%s/%s_%llx_%dx%d.rgba", dir, what, (unsigned long long)tex0, w,
           h);
  if (FILE* f = fopen(path, "wb")) {
    fwrite(rgba, 4, (size_t)w * h, f);
    fclose(f);
  }
}
}  // namespace

void set_enabled(bool on) {
  if (g_enabled.exchange(on) != on) {
    g_gen++;
  }
}

bool enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

void set_jak2(bool jak2) {
  if (g_jak2.exchange(jak2) != jak2) {
    g_gen++;
  }
}

void register_texture(int ps2_button, u32 tbp0, u32 psm) {
  if (ps2_button < 0 || ps2_button >= (int)Ps2Button::COUNT) {
    return;
  }
  const u64 v = ((u64)psm << 32) | (u64)(tbp0 + 1);
  if (g_tex[ps2_button].exchange(v) != v) {
    g_gen++;
  }
}

u32 generation() {
  return g_gen.load(std::memory_order_relaxed);
}

namespace {
// the font atlases with the PS2 button glyphs: ascii.12lo / ascii.24lo, which
// setup-font-texture! (texture.gc) moves to #xe0000 / #xe6000 (words) as PSMT4HL
// Jak II: font.24lo / font.12lo at #x90000 / #xc2000 (setup-font-texture)
bool is_font(const GsTex0& t) {
  if (t.psm() != GsTex0::PSM::PSMT4HL) {
    return false;
  }
  if (g_jak2.load(std::memory_order_relaxed)) {
    return t.tbp0() == 0x90000 / 64 || t.tbp0() == 0xc2000 / 64;
  }
  return t.tbp0() == 0x3800 || t.tbp0() == 0x3980;
}

int icon_of(const GsTex0& t) {
  for (int i = 0; i < (int)Ps2Button::COUNT; i++) {
    const u64 v = g_tex[i].load(std::memory_order_relaxed);
    if (v && (u32)v == t.tbp0() + 1 && (u32)(v >> 32) == (u32)t.psm()) {
      return i;
    }
  }
  return -1;
}
}  // namespace

bool may_patch(u64 tex0) {
  GsTex0 t(tex0);
  return is_font(t) || icon_of(t) >= 0;
}

bool patch(u64 tex0, u32* rgba, int w, int h) {
  if (!enabled()) {
    return false;
  }
  GsTex0 t(tex0);
  if (is_font(t)) {
    Timer timer;
    const bool done = g_jak2.load(std::memory_order_relaxed) ? patch_font_jak2(rgba, w, h)
                                                            : patch_font(rgba, w, h);
    if (done) {
      lg::info("[ctr] 3DS button symbols drawn into the {}x{} font in {:.1f} ms", w, h,
               timer.getMs());
      dump("font", tex0, rgba, w, h);
    }
    return done;
  }
  const int i = icon_of(t);
  if (i < 0) {
    return false;
  }
  patch_icon((Ps2Button)i, rgba, w, h);
  dump("icon", tex0, rgba, w, h);
  return true;
}

}  // namespace ctr_button_art
