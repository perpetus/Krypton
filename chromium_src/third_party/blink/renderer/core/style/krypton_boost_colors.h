/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Oklab duotone and luminance-preserving inversion adapted from the local
// reference's nsZenBoostsBackend.cpp. Chromium integration is paint-scoped:
// stored styles, CSSOM values, inheritance and image pixels remain original.
#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_STYLE_KRYPTON_BOOST_COLORS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_STYLE_KRYPTON_BOOST_COLORS_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

#include "base/no_destructor.h"
#include "third_party/blink/renderer/core/css/css_variable_data.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_token_stream.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/graphics/color.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"

namespace blink::krypton_boost {
inline thread_local unsigned paint_depth = 0;
inline thread_local unsigned resolve_depth = 0;

class PaintScope {
 public:
  PaintScope() { ++paint_depth; }
  ~PaintScope() { --paint_depth; }
  PaintScope(const PaintScope&) = delete;
  PaintScope& operator=(const PaintScope&) = delete;
};

inline const AtomicString& PropertyName() {
  thread_local base::NoDestructor<AtomicString> name("--krypton-boost-palette");
  return *name;
}

inline bool ThemeChanged(const ComputedStyle& before,
                         const ComputedStyle& after) {
  const auto* a = before.GetVariableData(PropertyName());
  const auto* b = after.GetVariableData(PropertyName());
  return a != b && (!a || !b || !(*a == *b));
}

struct Lab {
  float l, a, b;
};
inline float Linear(float c) {
  return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
inline float Srgb(float c) {
  c = std::max(0.0f, c);
  return std::clamp(
      c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f,
      0.0f, 1.0f);
}
inline Lab ToLab(float r, float g, float b) {
  r = Linear(r);
  g = Linear(g);
  b = Linear(b);
  const float l =
      std::cbrt(.4122214708f * r + .5363325363f * g + .0514459929f * b);
  const float m =
      std::cbrt(.2119034982f * r + .6806995451f * g + .1073969566f * b);
  const float s =
      std::cbrt(.0883024619f * r + .2817188376f * g + .6299787005f * b);
  return {.2104542553f * l + .7936177850f * m - .0040720468f * s,
          1.9779984951f * l - 2.4285922050f * m + .4505937099f * s,
          .0259040371f * l + .7827717662f * m - .8086757660f * s};
}
inline std::array<float, 3> FromLab(Lab c) {
  const float l = c.l + .3963377774f * c.a + .2158037573f * c.b;
  const float m = c.l - .1055613458f * c.a - .0638541728f * c.b;
  const float s = c.l - .0894841775f * c.a - 1.2914855480f * c.b;
  const float ll = l * l * l, mm = m * m * m, ss = s * s * s;
  return {Srgb(4.0767416621f * ll - 3.3077115913f * mm + .2309699292f * ss),
          Srgb(-1.2684380046f * ll + 2.6097574011f * mm - .3413193965f * ss),
          Srgb(-.0041960863f * ll - .7034186147f * mm + 1.7076147010f * ss)};
}
struct ColorCell {
  uint32_t rgb = 0;
  std::array<float, 3> value{};
  bool valid = false;
};
struct Theme {
  String key;
  Lab base{}, complementary{};
  float strength = 0;
  bool invert = false, valid = false;
  std::array<ColorCell, 256> colors{};
};
struct ThemeCache {
  std::array<Theme, 4> entries;
  size_t next = 0;
};

inline Theme* GetTheme(const CSSVariableData& data) {
  const auto text = data.OriginalText();
  if (text.length() > 192) {
    return nullptr;
  }
  thread_local base::NoDestructor<ThemeCache> cache;
  for (auto& entry : cache->entries) {
    if (entry.valid && StringView(entry.key) == text) {
      return &entry;
    }
  }
  CSSParserTokenStream stream(text);
  stream.ConsumeWhitespace();
  std::array<float, 6> v{};
  for (auto& value : v) {
    if (stream.AtEnd()) {
      return nullptr;
    }
    const auto token = stream.ConsumeIncludingWhitespace();
    if (token.GetType() != kNumberToken ||
        !std::isfinite(token.NumericValue())) {
      return nullptr;
    }
    value = static_cast<float>(token.NumericValue());
  }
  if (!stream.AtEnd() || v[0] < 0 || v[0] > 255 || v[1] < 0 || v[1] > 255 ||
      v[2] < 0 || v[2] > 255 || v[3] < 0 || v[3] > 1 || v[4] < -360 ||
      v[4] > 360 || (v[5] != 0 && v[5] != 1)) {
    return nullptr;
  }
  auto& entry = cache->entries[cache->next++ % cache->entries.size()];
  entry.key = text.ToString();
  entry.base = ToLab(v[0] / 255, v[1] / 255, v[2] / 255);
  entry.strength = v[3];
  entry.invert = v[5] == 1;
  const float angle = v[4] * 3.14159265358979323846f / 180;
  const float c = std::cos(angle), s = std::sin(angle);
  entry.complementary = {entry.base.l, entry.base.a * c - entry.base.b * s,
                         entry.base.a * s + entry.base.b * c};
  for (auto& cell : entry.colors) {
    cell.valid = false;
  }
  entry.valid = true;
  return &entry;
}

inline Color MapColor(Theme& theme, const Color& original) {
  const uint32_t rgb = original.Rgb() & 0x00ffffffu;
  auto& cell = theme.colors[(rgb * 0x9e3779b9u) >> 24];
  if (!cell.valid || cell.rgb != rgb) {
    std::array<float, 3> c{static_cast<float>((rgb >> 16) & 255) / 255,
                           static_cast<float>((rgb >> 8) & 255) / 255,
                           static_cast<float>(rgb & 255) / 255};
    if (theme.strength > 0) {
      const auto source = ToLab(c[0], c[1], c[2]);
      const float half_width =
          std::clamp(.5f - theme.strength * .45f, .05f, .5f);
      float t = std::clamp((source.l - (.5f - half_width)) / (2 * half_width),
                           0.0f, 1.0f);
      t = t * t * (3 - 2 * t);
      const auto mix = [t](float a, float b) { return a + (b - a) * t; };
      const Lab selected{mix(theme.base.l, theme.complementary.l),
                         mix(theme.base.a, theme.complementary.a),
                         mix(theme.base.b, theme.complementary.b)};
      const float delta = selected.l - source.l;
      const float a = source.a + (selected.a - source.a) * theme.strength;
      const float b = source.b + (selected.b - source.b) * theme.strength;
      const float rotation =
          (delta > 0 ? -1.0f : 1.0f) * theme.strength * theme.strength * .25f;
      const float co = std::cos(rotation), si = std::sin(rotation);
      c = FromLab({source.l + delta * theme.strength * theme.strength * .5f,
                   a * co - b * si, a * si + b * co});
    }
    if (theme.invert) {
      const float sum =
          std::min({c[0], c[1], c[2]}) + std::max({c[0], c[1], c[2]});
      for (auto& channel : c) {
        channel = 1 - sum + channel;
      }
      const float luma = c[0] * 54 + c[1] * 183 + c[2] * 19;
      if (luma <= 127.0f / 255 * 256) {
        for (auto& channel : c) {
          channel = 15.0f / 255 + channel * (240.0f / 255);
        }
      }
    }
    cell.rgb = rgb;
    cell.value = c;
    cell.valid = true;
  }
  return Color::FromColorSpace(Color::ColorSpace::kSRGB, cell.value[0],
                               cell.value[1], cell.value[2], original.Alpha());
}

template <class Read>
Color Resolve(const ComputedStyle& style, Read&& read) {
  if (!paint_depth || resolve_depth || style.InForcedColorsMode()) {
    return read();
  }
  ++resolve_depth;
  Color color = read();
  --resolve_depth;
  if (color.IsFullyTransparent()) {
    return color;
  }
  const auto* data = style.GetVariableData(PropertyName());
  auto* theme = data ? GetTheme(*data) : nullptr;
  return theme ? MapColor(*theme, color) : color;
}
}  // namespace blink::krypton_boost
#endif
