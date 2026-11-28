#pragma once

// Цветовые фильтры и «плёночные» эффекты для кадра камеры (без зависимостей от JUCE, чтобы можно было тестировать отдельно).
// Работает на месте над 8-битным буфером с произвольным порядком каналов.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace looks
{

enum class Look { none = 0, bw, sepia, warm, cool, contrast, vhs };

struct ChannelOffsets { int r, g, b; }; // смещение каналов внутри пикселя в байтах

namespace detail
{
    inline uint8_t clamp8 (int v) { return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v)); }

    inline uint32_t hash (uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    inline int luma (int r, int g, int b) { return (299 * r + 587 * g + 114 * b) / 1000; }
}

inline void process (uint8_t* data, int width, int height, int lineStride, int pixelStride,
                     ChannelOffsets ch, Look look, bool vignette, bool grain, uint32_t seed)
{
    using namespace detail;

    if (data == nullptr || width < 2 || height < 2 || (look == Look::none && ! vignette && ! grain))
        return;

    std::vector<uint8_t> tmp ((size_t) width * 3);

    const float cx = (float) (width - 1) * 0.5f, cy = (float) (height - 1) * 0.5f;

    for (int y = 0; y < height; ++y)
    {
        uint8_t* row = data + (size_t) y * (size_t) lineStride;

        // --- тон / цвет ---
        if (look == Look::vhs)
        {
            for (int x = 0; x < width; ++x)
            {
                const uint8_t* p = row + (size_t) x * (size_t) pixelStride;
                tmp[(size_t) x * 3]     = p[ch.r];
                tmp[(size_t) x * 3 + 1] = p[ch.g];
                tmp[(size_t) x * 3 + 2] = p[ch.b];
            }

            auto sample = [&] (int x, int c) { return (int) tmp[(size_t) std::clamp (x, 0, width - 1) * 3 + (size_t) c]; };

            for (int x = 0; x < width; ++x)
            {
                // красный уезжает влево, синий вправо, затем мягкое размытие по горизонтали
                int c[3];

                for (int k = 0; k < 3; ++k)
                {
                    const int shift = (k == 0 ? 2 : (k == 2 ? -2 : 0));
                    const int sx = x + shift;
                    c[k] = (sample (sx - 1, k) + 2 * sample (sx, k) + sample (sx + 1, k)) / 4;
                }

                const int gray = luma (c[0], c[1], c[2]);

                for (int k = 0; k < 3; ++k)
                {
                    int v = gray + ((c[k] - gray) * 8) / 10;           // чуть бледнее цвета
                    v = (v * 92) / 100 + 14;                            // приподнятые чёрные
                    if (y & 1) v = (v * 93) / 100;                      // строчная развёртка
                    v += (int) (hash ((uint32_t) x, (uint32_t) y, seed + 7u) & 15u) - 8; // шум
                    c[k] = v;
                }

                uint8_t* p = row + (size_t) x * (size_t) pixelStride;
                p[ch.r] = clamp8 (c[0]);
                p[ch.g] = clamp8 (c[1]);
                p[ch.b] = clamp8 (c[2]);
            }
        }
        else if (look != Look::none)
        {
            for (int x = 0; x < width; ++x)
            {
                uint8_t* p = row + (size_t) x * (size_t) pixelStride;
                int r = p[ch.r], g = p[ch.g], b = p[ch.b];

                switch (look)
                {
                    case Look::bw:
                        r = g = b = luma (r, g, b);
                        break;

                    case Look::sepia:
                    {
                        const int nr = (393 * r + 769 * g + 189 * b) / 1000;
                        const int ng = (349 * r + 686 * g + 168 * b) / 1000;
                        const int nb = (272 * r + 534 * g + 131 * b) / 1000;
                        r = nr; g = ng; b = nb;
                        break;
                    }

                    case Look::warm:
                        r = (r * 108) / 100 + 6;
                        g = (g * 101) / 100;
                        b = (b * 90) / 100;
                        break;

                    case Look::cool:
                        r = (r * 91) / 100;
                        g = (g * 101) / 100 + 2;
                        b = (b * 108) / 100 + 6;
                        break;

                    case Look::contrast:
                    {
                        const int gray = luma (r, g, b);
                        auto f = [&] (int v)
                        {
                            v = gray + ((v - gray) * 115) / 100;        // чуть насыщеннее
                            return ((v - 128) * 135) / 100 + 128;       // выше контраст
                        };
                        r = f (r); g = f (g); b = f (b);
                        break;
                    }

                    default:
                        break;
                }

                p[ch.r] = clamp8 (r);
                p[ch.g] = clamp8 (g);
                p[ch.b] = clamp8 (b);
            }
        }

        // --- виньетка и зерно ---
        if (vignette || grain)
        {
            const float ny = ((float) y - cy) / cy;

            for (int x = 0; x < width; ++x)
            {
                uint8_t* p = row + (size_t) x * (size_t) pixelStride;
                int r = p[ch.r], g = p[ch.g], b = p[ch.b];

                if (vignette)
                {
                    const float nx = ((float) x - cx) / cx;
                    const float r2 = (nx * nx + ny * ny) * 0.5f;           // 0 в центре, 1 в углу
                    const float t = std::clamp ((r2 - 0.15f) / 0.85f, 0.0f, 1.0f);
                    const float f = 1.0f - 0.65f * t * t;
                    r = (int) ((float) r * f);
                    g = (int) ((float) g * f);
                    b = (int) ((float) b * f);
                }

                if (grain)
                {
                    const int n = (int) (hash ((uint32_t) x, (uint32_t) y, seed) & 255u) - 128;
                    const int d = (n * 14) / 128;
                    r += d; g += d; b += d;
                }

                p[ch.r] = clamp8 (r);
                p[ch.g] = clamp8 (g);
                p[ch.b] = clamp8 (b);
            }
        }
    }
}

} // namespace looks
