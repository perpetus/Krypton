// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// Path data converted from Zen's screenshot overlay artwork: the selection
// face, the full-page and visible-page icons, and the close, copy and download
// glyphs. Krypton paints these paths natively instead of loading SVG files.

#include "chrome/browser/ui/krypton_ui/page_capture_icons.h"

#include <array>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkPathTypes.h"

namespace krypton_ui::capture_icons {
namespace {
// Each array holds absolute commands: 0 move(x, y), 1 line(x, y),
// 2 cubic(x1, y1, x2, y2, x, y), 3 quad(x1, y1, x, y),
// 4 SVG arc(rx, ry, rotation, large, sweep, x, y) and 5 close.

constexpr float kFaceLines[] = {
    0.f,   11.4f, 0.9f,  1.f,   11.4f, 3.8f,  1.f,   5.4f,  3.8f,  2.f,   4.5f,
    3.8f,  3.9f,  4.6f,  3.9f,  5.3f,  1.f,   3.9f,  11.3f, 1.f,   0.8f,  11.3f,
    1.f,   0.8f,  3.8f,  2.f,   0.8f,  2.1f,  2.2f,  0.7f,  3.9f,  0.7f,  1.f,
    11.5f, 0.7f,  1.f,   11.5f, 0.9f,  5.f,   0.f,   63.2f, 11.4f, 1.f,   60.1f,
    11.4f, 1.f,   60.1f, 5.4f,  2.f,   60.1f, 4.6f,  59.5f, 3.9f,  58.6f, 3.9f,
    1.f,   52.6f, 3.9f,  1.f,   52.6f, 0.9f,  1.f,   60.2f, 0.9f,  2.f,   61.9f,
    0.9f,  63.3f, 2.3f,  63.3f, 4.f,   5.f,   0.f,   52.6f, 63.2f, 1.f,   52.6f,
    60.1f, 1.f,   58.6f, 60.1f, 2.f,   59.5f, 60.1f, 60.1f, 59.5f, 60.1f, 58.6f,
    1.f,   60.1f, 52.6f, 1.f,   63.2f, 52.6f, 1.f,   63.2f, 60.2f, 2.f,   63.2f,
    61.9f, 61.8f, 63.3f, 60.1f, 63.3f, 5.f,   0.f,   0.8f,  52.7f, 1.f,   3.9f,
    52.7f, 1.f,   3.9f,  58.7f, 2.f,   3.9f,  59.6f, 4.5f,  60.2f, 5.4f,  60.2f,
    1.f,   11.4f, 60.2f, 1.f,   11.4f, 63.3f, 1.f,   3.8f,  63.3f, 2.f,   2.1f,
    63.3f, 0.7f,  61.9f, 0.7f,  60.2f, 5.f,   0.f,   33.3f, 49.2f, 1.f,   33.f,
    49.2f, 2.f,   28.4f, 49.1f, 25.2f, 45.6f, 25.1f, 45.4f, 2.f,   24.5f, 44.6f,
    24.5f, 43.4f, 25.2f, 42.7f, 2.f,   26.f,  41.9f, 27.1f, 42.1f, 27.8f, 42.8f,
    2.f,   27.8f, 42.8f, 30.1f, 45.4f, 33.f,  45.4f, 2.f,   34.8f, 45.4f, 36.6f,
    44.5f, 38.2f, 42.8f, 2.f,   39.f,  42.f,  40.1f, 42.f,  40.9f, 42.8f, 2.f,
    41.7f, 43.6f, 41.7f, 44.7f, 40.9f, 45.5f, 2.f,   38.7f, 47.9f, 36.f,  49.2f,
    33.3f, 49.2f, 5.f,
};

constexpr float kFullPageViewport[] = {
    0.f, 7.f, 42.f, 1.f, 39.f, 42.f, 1.f, 39.f, 5.1f, 1.f, 7.f, 5.1f, 5.f,
};

constexpr float kFullPageFrame[] = {
    0.f,  40.f, 11.f, 2.f,   40.5f, 11.f, 41.f, 11.4f, 41.f, 12.f, 1.f,  41.f,
    36.f, 2.f,  41.f, 36.5f, 40.5f, 37.f, 40.f, 37.f,  1.f,  6.f,  37.f, 2.f,
    5.4f, 37.f, 5.f,  36.5f, 5.f,   36.f, 1.f,  5.f,   12.f, 2.f,  5.f,  11.4f,
    5.4f, 11.f, 6.f,  11.f,  1.f,   40.f, 11.f, 5.f,   0.f,  7.f,  35.f, 1.f,
    39.f, 35.f, 1.f,  39.f,  13.f,  1.f,  7.f,  13.f,  1.f,  7.f,  35.f, 5.f,
};

constexpr float kFullPageFrameTop[] = {
    0.f, 7.f, 13.f, 1.f, 39.f, 13.f, 1.f, 39.f, 11.f, 1.f, 7.f, 11.f, 5.f,
};

constexpr float kFullPageFrameBottom[] = {
    0.f, 7.f, 37.f, 1.f, 39.f, 37.f, 1.f, 39.f, 35.f, 1.f, 7.f, 35.f, 5.f,
};

constexpr float kFullPageDash[] = {
    0.f,  38.f, 11.f, 1.f,  39.f, 11.f, 1.f,  39.f, 9.f,  1.f,  38.f, 9.f,
    1.f,  38.f, 11.f, 5.f,  0.f,  38.f, 14.f, 1.f,  39.f, 14.f, 1.f,  39.f,
    12.f, 1.f,  38.f, 12.f, 1.f,  38.f, 14.f, 5.f,  0.f,  38.f, 17.f, 1.f,
    39.f, 17.f, 1.f,  39.f, 15.f, 1.f,  38.f, 15.f, 1.f,  38.f, 17.f, 5.f,
    0.f,  38.f, 20.f, 1.f,  39.f, 20.f, 1.f,  39.f, 18.f, 1.f,  38.f, 18.f,
    1.f,  38.f, 20.f, 5.f,  0.f,  38.f, 23.f, 1.f,  39.f, 23.f, 1.f,  39.f,
    21.f, 1.f,  38.f, 21.f, 1.f,  38.f, 23.f, 5.f,  0.f,  38.f, 26.f, 1.f,
    39.f, 26.f, 1.f,  39.f, 24.f, 1.f,  38.f, 24.f, 1.f,  38.f, 26.f, 5.f,
    0.f,  38.f, 29.f, 1.f,  39.f, 29.f, 1.f,  39.f, 27.f, 1.f,  38.f, 27.f,
    1.f,  38.f, 29.f, 5.f,  0.f,  38.f, 32.f, 1.f,  39.f, 32.f, 1.f,  39.f,
    30.f, 1.f,  38.f, 30.f, 1.f,  38.f, 32.f, 5.f,  0.f,  38.f, 35.f, 1.f,
    39.f, 35.f, 1.f,  39.f, 33.f, 1.f,  38.f, 33.f, 1.f,  38.f, 35.f, 5.f,
    0.f,  38.f, 38.f, 1.f,  39.f, 38.f, 1.f,  39.f, 36.f, 1.f,  38.f, 36.f,
    1.f,  38.f, 38.f, 5.f,  0.f,  38.f, 41.f, 1.f,  39.f, 41.f, 1.f,  39.f,
    39.f, 1.f,  38.f, 39.f, 1.f,  38.f, 41.f, 5.f,  0.f,  37.f, 42.f, 1.f,
    39.f, 42.f, 1.f,  39.f, 41.f, 1.f,  37.f, 41.f, 1.f,  37.f, 42.f, 5.f,
    0.f,  34.f, 42.f, 1.f,  36.f, 42.f, 1.f,  36.f, 41.f, 1.f,  34.f, 41.f,
    1.f,  34.f, 42.f, 5.f,  0.f,  31.f, 42.f, 1.f,  33.f, 42.f, 1.f,  33.f,
    41.f, 1.f,  31.f, 41.f, 1.f,  31.f, 42.f, 5.f,  0.f,  28.f, 42.f, 1.f,
    30.f, 42.f, 1.f,  30.f, 41.f, 1.f,  28.f, 41.f, 1.f,  28.f, 42.f, 5.f,
    0.f,  25.f, 42.f, 1.f,  27.f, 42.f, 1.f,  27.f, 41.f, 1.f,  25.f, 41.f,
    1.f,  25.f, 42.f, 5.f,  0.f,  22.f, 42.f, 1.f,  24.f, 42.f, 1.f,  24.f,
    41.f, 1.f,  22.f, 41.f, 1.f,  22.f, 42.f, 5.f,  0.f,  19.f, 42.f, 1.f,
    21.f, 42.f, 1.f,  21.f, 41.f, 1.f,  19.f, 41.f, 1.f,  19.f, 42.f, 5.f,
    0.f,  16.f, 42.f, 1.f,  18.f, 42.f, 1.f,  18.f, 41.f, 1.f,  16.f, 41.f,
    1.f,  16.f, 42.f, 5.f,  0.f,  13.f, 42.f, 1.f,  15.f, 42.f, 1.f,  15.f,
    41.f, 1.f,  13.f, 41.f, 1.f,  13.f, 42.f, 5.f,  0.f,  10.f, 42.f, 1.f,
    12.f, 42.f, 1.f,  12.f, 41.f, 1.f,  10.f, 41.f, 1.f,  10.f, 42.f, 5.f,
    0.f,  8.f,  39.f, 1.f,  7.f,  39.f, 1.f,  7.f,  42.f, 1.f,  9.f,  42.f,
    1.f,  9.f,  41.f, 1.f,  8.f,  41.f, 1.f,  8.f,  39.f, 5.f,  0.f,  7.f,
    38.f, 1.f,  8.f,  38.f, 1.f,  8.f,  36.f, 1.f,  7.f,  36.f, 1.f,  7.f,
    38.f, 5.f,  0.f,  7.f,  35.f, 1.f,  8.f,  35.f, 1.f,  8.f,  33.f, 1.f,
    7.f,  33.f, 1.f,  7.f,  35.f, 5.f,  0.f,  7.f,  32.f, 1.f,  8.f,  32.f,
    1.f,  8.f,  30.f, 1.f,  7.f,  30.f, 1.f,  7.f,  32.f, 5.f,  0.f,  7.f,
    29.f, 1.f,  8.f,  29.f, 1.f,  8.f,  27.f, 1.f,  7.f,  27.f, 1.f,  7.f,
    29.f, 5.f,  0.f,  7.f,  26.f, 1.f,  8.f,  26.f, 1.f,  8.f,  24.f, 1.f,
    7.f,  24.f, 1.f,  7.f,  26.f, 5.f,  0.f,  7.f,  23.f, 1.f,  8.f,  23.f,
    1.f,  8.f,  21.f, 1.f,  7.f,  21.f, 1.f,  7.f,  23.f, 5.f,  0.f,  7.f,
    20.f, 1.f,  8.f,  20.f, 1.f,  8.f,  18.f, 1.f,  7.f,  18.f, 1.f,  7.f,
    20.f, 5.f,  0.f,  7.f,  17.f, 1.f,  8.f,  17.f, 1.f,  8.f,  15.f, 1.f,
    7.f,  15.f, 1.f,  7.f,  17.f, 5.f,  0.f,  7.f,  14.f, 1.f,  8.f,  14.f,
    1.f,  8.f,  12.f, 1.f,  7.f,  12.f, 1.f,  7.f,  14.f, 5.f,  0.f,  7.f,
    11.f, 1.f,  8.f,  11.f, 1.f,  8.f,  9.f,  1.f,  7.f,  9.f,  1.f,  7.f,
    11.f, 5.f,  0.f,  9.f,  5.f,  1.f,  7.f,  5.f,  1.f,  7.f,  8.f,  1.f,
    8.f,  8.f,  1.f,  8.f,  6.f,  1.f,  9.f,  6.f,  1.f,  9.f,  5.f,  5.f,
    0.f,  10.f, 6.f,  1.f,  12.f, 6.f,  1.f,  12.f, 5.f,  1.f,  10.f, 5.f,
    1.f,  10.f, 6.f,  5.f,  0.f,  13.f, 6.f,  1.f,  15.f, 6.f,  1.f,  15.f,
    5.f,  1.f,  13.f, 5.f,  1.f,  13.f, 6.f,  5.f,  0.f,  16.f, 6.f,  1.f,
    18.f, 6.f,  1.f,  18.f, 5.f,  1.f,  16.f, 5.f,  1.f,  16.f, 6.f,  5.f,
    0.f,  19.f, 6.f,  1.f,  21.f, 6.f,  1.f,  21.f, 5.f,  1.f,  19.f, 5.f,
    1.f,  19.f, 6.f,  5.f,  0.f,  22.f, 6.f,  1.f,  24.f, 6.f,  1.f,  24.f,
    5.f,  1.f,  22.f, 5.f,  1.f,  22.f, 6.f,  5.f,  0.f,  25.f, 6.f,  1.f,
    27.f, 6.f,  1.f,  27.f, 5.f,  1.f,  25.f, 5.f,  1.f,  25.f, 6.f,  5.f,
    0.f,  28.f, 6.f,  1.f,  30.f, 6.f,  1.f,  30.f, 5.f,  1.f,  28.f, 5.f,
    1.f,  28.f, 6.f,  5.f,  0.f,  31.f, 6.f,  1.f,  33.f, 6.f,  1.f,  33.f,
    5.f,  1.f,  31.f, 5.f,  1.f,  31.f, 6.f,  5.f,  0.f,  34.f, 6.f,  1.f,
    36.f, 6.f,  1.f,  36.f, 5.f,  1.f,  34.f, 5.f,  1.f,  34.f, 6.f,  5.f,
    0.f,  39.f, 5.f,  1.f,  37.f, 5.f,  1.f,  37.f, 6.f,  1.f,  38.f, 6.f,
    1.f,  38.f, 8.f,  1.f,  39.f, 8.f,  1.f,  39.f, 5.f,  5.f,
};

constexpr float kVisibleFrame[] = {
    0.f,  5.f,   12.f,  2.f,  5.f,  11.4f, 5.5f, 11.f, 6.f,  11.f, 1.f,  40.f,
    11.f, 2.f,   40.6f, 11.f, 41.f, 11.5f, 41.f, 12.f, 1.f,  41.f, 36.f, 2.f,
    41.f, 36.6f, 40.5f, 37.f, 40.f, 37.f,  1.f,  6.f,  37.f, 2.f,  5.4f, 37.f,
    5.f,  36.5f, 5.f,   36.f, 1.f,  5.f,   12.f, 5.f,  0.f,  7.f,  35.f, 1.f,
    7.f,  13.f,  1.f,   39.f, 13.f, 1.f,   39.f, 35.f, 1.f,  7.f,  35.f, 5.f,
};

constexpr float kVisibleViewport[] = {
    0.f, 7.f, 35.f, 1.f, 39.f, 35.f, 1.f, 39.f, 13.f, 1.f, 7.f, 13.f, 5.f,
};

constexpr float kVisibleDash[] = {
    0.f,  38.f, 19.f, 1.f,  39.f, 19.f, 1.f,  39.f, 17.f, 1.f,  38.f, 17.f,
    1.f,  38.f, 19.f, 5.f,  0.f,  38.f, 22.f, 1.f,  39.f, 22.f, 1.f,  39.f,
    20.f, 1.f,  38.f, 20.f, 1.f,  38.f, 22.f, 5.f,  0.f,  38.f, 25.f, 1.f,
    39.f, 25.f, 1.f,  39.f, 23.f, 1.f,  38.f, 23.f, 1.f,  38.f, 25.f, 5.f,
    0.f,  38.f, 28.f, 1.f,  39.f, 28.f, 1.f,  39.f, 26.f, 1.f,  38.f, 26.f,
    1.f,  38.f, 28.f, 5.f,  0.f,  38.f, 31.f, 1.f,  39.f, 31.f, 1.f,  39.f,
    29.f, 1.f,  38.f, 29.f, 1.f,  38.f, 31.f, 5.f,  0.f,  38.f, 34.f, 1.f,
    39.f, 34.f, 1.f,  39.f, 32.f, 1.f,  38.f, 32.f, 1.f,  38.f, 34.f, 5.f,
    0.f,  37.f, 35.f, 1.f,  39.f, 35.f, 1.f,  39.f, 34.f, 1.f,  37.f, 34.f,
    1.f,  37.f, 35.f, 5.f,  0.f,  34.f, 35.f, 1.f,  36.f, 35.f, 1.f,  36.f,
    34.f, 1.f,  34.f, 34.f, 1.f,  34.f, 35.f, 5.f,  0.f,  31.f, 35.f, 1.f,
    33.f, 35.f, 1.f,  33.f, 34.f, 1.f,  31.f, 34.f, 1.f,  31.f, 35.f, 5.f,
    0.f,  28.f, 35.f, 1.f,  30.f, 35.f, 1.f,  30.f, 34.f, 1.f,  28.f, 34.f,
    1.f,  28.f, 35.f, 5.f,  0.f,  25.f, 35.f, 1.f,  27.f, 35.f, 1.f,  27.f,
    34.f, 1.f,  25.f, 34.f, 1.f,  25.f, 35.f, 5.f,  0.f,  22.f, 35.f, 1.f,
    24.f, 35.f, 1.f,  24.f, 34.f, 1.f,  22.f, 34.f, 1.f,  22.f, 35.f, 5.f,
    0.f,  19.f, 35.f, 1.f,  21.f, 35.f, 1.f,  21.f, 34.f, 1.f,  19.f, 34.f,
    1.f,  19.f, 35.f, 5.f,  0.f,  16.f, 35.f, 1.f,  18.f, 35.f, 1.f,  18.f,
    34.f, 1.f,  16.f, 34.f, 1.f,  16.f, 35.f, 5.f,  0.f,  13.f, 35.f, 1.f,
    15.f, 35.f, 1.f,  15.f, 34.f, 1.f,  13.f, 34.f, 1.f,  13.f, 35.f, 5.f,
    0.f,  10.f, 35.f, 1.f,  12.f, 35.f, 1.f,  12.f, 34.f, 1.f,  10.f, 34.f,
    1.f,  10.f, 35.f, 5.f,  0.f,  8.f,  32.f, 1.f,  7.f,  32.f, 1.f,  7.f,
    35.f, 1.f,  9.f,  35.f, 1.f,  9.f,  34.f, 1.f,  8.f,  34.f, 1.f,  8.f,
    32.f, 5.f,  0.f,  7.f,  31.f, 1.f,  8.f,  31.f, 1.f,  8.f,  29.f, 1.f,
    7.f,  29.f, 1.f,  7.f,  31.f, 5.f,  0.f,  7.f,  28.f, 1.f,  8.f,  28.f,
    1.f,  8.f,  26.f, 1.f,  7.f,  26.f, 1.f,  7.f,  28.f, 5.f,  0.f,  7.f,
    25.f, 1.f,  8.f,  25.f, 1.f,  8.f,  23.f, 1.f,  7.f,  23.f, 1.f,  7.f,
    25.f, 5.f,  0.f,  7.f,  22.f, 1.f,  8.f,  22.f, 1.f,  8.f,  20.f, 1.f,
    7.f,  20.f, 1.f,  7.f,  22.f, 5.f,  0.f,  7.f,  19.f, 1.f,  8.f,  19.f,
    1.f,  8.f,  17.f, 1.f,  7.f,  17.f, 1.f,  7.f,  19.f, 5.f,  0.f,  9.f,
    13.f, 1.f,  7.f,  13.f, 1.f,  7.f,  16.f, 1.f,  8.f,  16.f, 1.f,  8.f,
    14.f, 1.f,  9.f,  14.f, 1.f,  9.f,  13.f, 5.f,  0.f,  10.f, 14.f, 1.f,
    12.f, 14.f, 1.f,  12.f, 13.f, 1.f,  10.f, 13.f, 1.f,  10.f, 14.f, 5.f,
    0.f,  13.f, 14.f, 1.f,  15.f, 14.f, 1.f,  15.f, 13.f, 1.f,  13.f, 13.f,
    1.f,  13.f, 14.f, 5.f,  0.f,  16.f, 14.f, 1.f,  18.f, 14.f, 1.f,  18.f,
    13.f, 1.f,  16.f, 13.f, 1.f,  16.f, 14.f, 5.f,  0.f,  19.f, 14.f, 1.f,
    21.f, 14.f, 1.f,  21.f, 13.f, 1.f,  19.f, 13.f, 1.f,  19.f, 14.f, 5.f,
    0.f,  22.f, 14.f, 1.f,  24.f, 14.f, 1.f,  24.f, 13.f, 1.f,  22.f, 13.f,
    1.f,  22.f, 14.f, 5.f,  0.f,  25.f, 14.f, 1.f,  27.f, 14.f, 1.f,  27.f,
    13.f, 1.f,  25.f, 13.f, 1.f,  25.f, 14.f, 5.f,  0.f,  28.f, 14.f, 1.f,
    30.f, 14.f, 1.f,  30.f, 13.f, 1.f,  28.f, 13.f, 1.f,  28.f, 14.f, 5.f,
    0.f,  31.f, 14.f, 1.f,  33.f, 14.f, 1.f,  33.f, 13.f, 1.f,  31.f, 13.f,
    1.f,  31.f, 14.f, 5.f,  0.f,  34.f, 14.f, 1.f,  36.f, 14.f, 1.f,  36.f,
    13.f, 1.f,  34.f, 13.f, 1.f,  34.f, 14.f, 5.f,  0.f,  39.f, 13.f, 1.f,
    37.f, 13.f, 1.f,  37.f, 14.f, 1.f,  38.f, 14.f, 1.f,  38.f, 16.f, 1.f,
    39.f, 16.f, 1.f,  39.f, 13.f, 5.f,
};

constexpr float kCloseIcon[] = {
    0.f,     9.108f, 7.776f, 1.f,    13.817f, 3.067f, 4.f,    0.626f,
    0.626f,  0.f,    0.f,    0.f,    12.933f, 2.182f, 1.f,    8.244f,
    6.871f,  1.f,    7.756f, 6.871f, 1.f,     3.067f, 2.183f, 4.f,
    0.625f,  0.625f, 0.f,    1.f,    0.f,     2.183f, 3.068f, 1.f,
    6.87f,   7.754f, 1.f,    6.87f,  8.245f,  1.f,    2.183f, 12.932f,
    4.f,     0.626f, 0.626f, 0.f,    0.f,     0.f,    3.067f, 13.817f,
    1.f,     7.754f, 9.13f,  1.f,    8.245f,  9.13f,  1.f,    12.932f,
    13.817f, 4.f,    0.627f, 0.627f, 0.f,     0.f,    0.f,    13.817f,
    13.817f, 4.f,    0.626f, 0.626f, 0.f,     0.f,    0.f,    13.817f,
    12.932f, 1.f,    9.108f, 8.223f, 1.f,     9.108f, 7.776f, 5.f,
};

constexpr float kCopyIcon[] = {
    0.f,     6.035f,  1.25f,   2.f,     5.035f,  1.25f,   4.223f,  2.062f,
    4.223f,  3.063f,  1.f,     5.723f,  3.063f,  2.f,     5.723f,  2.89f,
    5.863f,  2.75f,   6.035f,  2.75f,   1.f,     11.985f, 2.75f,   2.f,
    12.157f, 2.75f,   12.298f, 2.89f,   12.298f, 3.063f,  1.f,     12.298f,
    10.713f, 2.f,     12.298f, 10.885f, 12.158f, 11.025f, 11.985f, 11.025f,
    1.f,     11.985f, 12.525f, 2.f,     12.985f, 12.525f, 13.798f, 11.713f,
    13.798f, 10.712f, 1.f,     13.798f, 3.062f,  2.f,     13.798f, 2.062f,
    12.986f, 1.25f,   11.985f, 1.25f,   1.f,     6.035f,  1.25f,   5.f,
};

constexpr float kCopyIconBody[] = {
    0.f,     3.063f, 4.225f,  2.f,     2.062f,  4.225f,  1.25f,  5.037f,
    1.25f,   6.038f, 1.f,     1.25f,   13.688f, 2.f,     1.25f,  14.688f,
    2.062f,  15.5f,  3.063f,  15.5f,   1.f,     9.013f,  15.5f,  2.f,
    10.013f, 15.5f,  10.825f, 14.688f, 10.825f, 13.688f, 1.f,    10.825f,
    6.038f,  2.f,    10.825f, 5.037f,  10.013f, 4.225f,  9.012f, 4.225f,
    1.f,     3.062f, 4.225f,  5.f,     0.f,     2.75f,   6.038f, 2.f,
    2.75f,   5.865f, 2.89f,   5.725f,  3.063f,  5.725f,  1.f,    9.013f,
    5.725f,  2.f,    9.185f,  5.725f,  9.325f,  5.865f,  9.325f, 6.038f,
    1.f,     9.325f, 13.688f, 2.f,     9.325f,  13.86f,  9.185f, 14.f,
    9.012f,  14.f,   1.f,     3.062f,  14.f,    4.f,     0.313f, 0.313f,
    0.f,     0.f,    1.f,     2.75f,   13.688f, 1.f,     2.75f,  6.038f,
    5.f,
};

constexpr float kDownloadIcon[] = {
    0.f,    8.25f,  0.625f, 4.f,     0.625f,  0.625f,  0.f,    0.f,     0.f,
    7.f,    0.625f, 1.f,    7.f,     8.948f,  1.f,     3.692f, 5.639f,  4.f,
    0.626f, 0.626f, 0.f,    0.f,     0.f,     2.807f,  6.524f, 1.f,     7.285f,
    11.f,   1.f,    7.966f, 11.f,    1.f,     12.443f, 6.524f, 4.f,     0.626f,
    0.626f, 0.f,    0.f,    0.f,     11.558f, 5.639f,  1.f,    8.25f,   8.947f,
    1.f,    8.25f,  0.625f, 5.f,     0.f,     12.25f,  15.f,   4.f,     2.f,
    2.f,    0.f,    0.f,    0.f,     14.25f,  13.f,    1.f,    14.25f,  11.625f,
    4.f,    0.625f, 0.625f, 0.f,     0.f,     0.f,     13.f,   11.625f, 1.f,
    13.f,   13.15f, 1.f,    12.4f,   13.75f,  1.f,     2.85f,  13.75f,  1.f,
    2.25f,  13.15f, 1.f,    2.25f,   11.625f, 4.f,     0.625f, 0.625f,  0.f,
    0.f,    0.f,    1.f,    11.625f, 1.f,     1.f,     13.f,   4.f,     2.f,
    2.f,    0.f,    0.f,    0.f,     3.f,     15.f,    1.f,    12.25f,  15.f,
    5.f,
};

SkPath Build(base::span<const float> ops, SkPathFillType fill) {
  SkPathBuilder builder(fill);
  for (size_t i = 0; i < ops.size();) {
    switch (static_cast<int>(ops[i++])) {
      case 0:
        builder.moveTo(ops[i], ops[i + 1]);
        i += 2;
        break;
      case 1:
        builder.lineTo(ops[i], ops[i + 1]);
        i += 2;
        break;
      case 2:
        builder.cubicTo(ops[i], ops[i + 1], ops[i + 2], ops[i + 3], ops[i + 4],
                        ops[i + 5]);
        i += 6;
        break;
      case 3:
        builder.quadTo(ops[i], ops[i + 1], ops[i + 2], ops[i + 3]);
        i += 4;
        break;
      case 4:
        // An SVG sweep flag of 1 is clockwise.
        builder.arcTo(
            {ops[i], ops[i + 1]}, ops[i + 2],
            ops[i + 3] != 0 ? SkPathBuilder::kLarge_ArcSize
                            : SkPathBuilder::kSmall_ArcSize,
            ops[i + 4] != 0 ? SkPathDirection::kCW : SkPathDirection::kCCW,
            {ops[i + 5], ops[i + 6]});
        i += 7;
        break;
      default:
        builder.close();
        break;
    }
  }
  return builder.detach();
}
}  // namespace

const SkPath& Path(Shape shape) {
  static const base::NoDestructor<std::array<SkPath, kShapeCount>> paths([] {
    constexpr auto kWinding = SkPathFillType::kWinding;
    return std::array<SkPath, kShapeCount>{
        Build(kFaceLines, kWinding),
        Build(kFullPageViewport, kWinding),
        Build(kFullPageFrame, kWinding),
        Build(kFullPageFrameTop, kWinding),
        Build(kFullPageFrameBottom, kWinding),
        Build(kFullPageDash, kWinding),
        Build(kVisibleFrame, kWinding),
        Build(kVisibleViewport, kWinding),
        Build(kVisibleDash, kWinding),
        Build(kCloseIcon, kWinding),
        Build(kCopyIcon, kWinding),
        Build(kCopyIconBody, SkPathFillType::kEvenOdd),
        Build(kDownloadIcon, kWinding),
    };
  }());
  return (*paths)[static_cast<size_t>(shape)];
}
}  // namespace krypton_ui::capture_icons
