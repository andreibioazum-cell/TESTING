#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "stb_truetype.h"

struct FontGlyph {
    float x0, y0, x1, y1;   // offset from baseline-top
    float u0, v0, u1, v1;   // UV in atlas
    float advance;
};

struct UiVertex {
    float x, y;
    float u, v;
    uint32_t color; // RGBA8 packed (little-endian ABGR -> shader reads vec4 normalized)
};

class StudioFont {
public:
    static constexpr int kAtlasSize = 1024;

    std::vector<uint8_t> atlasRgba;
    float basePixelHeight = 28.0f;
    float ascent = 22.0f;
    float lineHeight = 32.0f;
    bool loadedTtf = false;
    std::string activePath;
    uint64_t atlasVersion = 1;

    std::unordered_map<uint32_t, FontGlyph> glyphs;

    static uint32_t nextUtf8(const char *&p, const char *end) {
        if (p >= end) return 0;
        uint8_t c = (uint8_t)*p++;
        if (c < 0x80) return c;
        if ((c & 0xE0) == 0xC0 && p < end) {
            uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)*p++ & 0x3F);
            return cp;
        }
        if ((c & 0xF0) == 0xE0 && p + 1 < end) {
            uint32_t cp = ((c & 0x0F) << 12) | (((uint8_t)p[0] & 0x3F) << 6) | ((uint8_t)p[1] & 0x3F);
            p += 2;
            return cp;
        }
        if ((c & 0xF8) == 0xF0 && p + 2 < end) {
            uint32_t cp = ((c & 0x07) << 18) | (((uint8_t)p[0] & 0x3F) << 12) | (((uint8_t)p[1] & 0x3F) << 6) | ((uint8_t)p[2] & 0x3F);
            p += 3;
            return cp;
        }
        return '?';
    }

    bool loadBestSystemOrCustom(const std::string &customPath, int familyChoice) {
        std::vector<std::string> candidates;
        if (!customPath.empty()) candidates.push_back(customPath);
        if (familyChoice == 0 || familyChoice == 3) { // mono / bold mono
            candidates.push_back("/system/fonts/DroidSansMono.ttf");
            candidates.push_back("/system/fonts/RobotoMono-Regular.ttf");
            candidates.push_back("/system/fonts/CutiveMono.ttf");
        } else if (familyChoice == 2) { // serif
            candidates.push_back("/system/fonts/NotoSerif-Regular.ttf");
            candidates.push_back("/system/fonts/DroidSerif-Regular.ttf");
        }
        candidates.push_back("/system/fonts/Roboto-Regular.ttf");
        candidates.push_back("/system/fonts/NotoSans-Regular.ttf");
        candidates.push_back("/system/fonts/DroidSans.ttf");

        for (const auto &path : candidates) {
            if (loadFromFile(path)) return true;
        }
        buildFallback8x8();
        return false;
    }

    bool loadFromFile(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 16 * 1024 * 1024) { std::fclose(f); return false; }
        std::vector<uint8_t> ttf((size_t)sz);
        if (std::fread(ttf.data(), 1, (size_t)sz, f) != (size_t)sz) { std::fclose(f); return false; }
        std::fclose(f);

        stbtt_fontinfo info{};
        if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return false;

        atlasRgba.assign(kAtlasSize * kAtlasSize * 4, 0);
        // Solid white 4x4 block at top-left for solid UI quads
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int idx = (y * kAtlasSize + x) * 4;
                atlasRgba[idx + 0] = 255; atlasRgba[idx + 1] = 255;
                atlasRgba[idx + 2] = 255; atlasRgba[idx + 3] = 255;
            }

        glyphs.clear();
        basePixelHeight = 28.0f;
        float scale = stbtt_ScaleForPixelHeight(&info, basePixelHeight);
        int a, d, lg;
        stbtt_GetFontVMetrics(&info, &a, &d, &lg);
        ascent = a * scale;
        lineHeight = (a - d + lg) * scale;
        if (lineHeight < 18.0f) lineHeight = basePixelHeight * 1.15f;

        int penX = 6, penY = 2, rowH = 0;
        auto packFrom = [&](stbtt_fontinfo &finfo, float fscale, float fascent, uint32_t first, uint32_t last) {
            for (uint32_t cp = first; cp <= last; ++cp) {
                if (glyphs.find(cp) != glyphs.end()) continue;
                int g = stbtt_FindGlyphIndex(&finfo, (int)cp);
                if (g == 0 && cp != ' ') continue;
                int adv, lsb;
                stbtt_GetGlyphHMetrics(&finfo, g, &adv, &lsb);
                int ix0, iy0, ix1, iy1;
                stbtt_GetGlyphBitmapBox(&finfo, g, fscale, fscale, &ix0, &iy0, &ix1, &iy1);
                int gw = ix1 - ix0;
                int gh = iy1 - iy0;
                if (penX + gw + 2 >= kAtlasSize) {
                    penX = 2;
                    penY += rowH + 2;
                    rowH = 0;
                }
                if (penY + gh + 2 >= kAtlasSize) break;
                if (gw > 0 && gh > 0) {
                    std::vector<uint8_t> bmp((size_t)(gw * gh));
                    stbtt_MakeGlyphBitmap(&finfo, bmp.data(), gw, gh, gw, fscale, fscale, g);
                    for (int y = 0; y < gh; ++y) {
                        for (int x = 0; x < gw; ++x) {
                            int idx = ((penY + y) * kAtlasSize + (penX + x)) * 4;
                            atlasRgba[idx + 0] = 255;
                            atlasRgba[idx + 1] = 255;
                            atlasRgba[idx + 2] = 255;
                            atlasRgba[idx + 3] = bmp[(size_t)(y * gw + x)];
                        }
                    }
                }
                FontGlyph fg{};
                fg.x0 = (float)ix0;
                fg.y0 = (float)iy0 + fascent;
                fg.x1 = (float)ix1;
                fg.y1 = (float)iy1 + fascent;
                fg.u0 = (float)penX / (float)kAtlasSize;
                fg.v0 = (float)penY / (float)kAtlasSize;
                fg.u1 = (float)(penX + gw) / (float)kAtlasSize;
                fg.v1 = (float)(penY + gh) / (float)kAtlasSize;
                fg.advance = adv * fscale;
                glyphs[cp] = fg;
                penX += gw + 2;
                rowH = std::max(rowH, gh);
            }
        };

        packFrom(info, scale, ascent, 0x0020, 0x00FF); // ASCII + Latin-1
        packFrom(info, scale, ascent, 0x0400, 0x045F); // Cyrillic
        packFrom(info, scale, ascent, 0x2022, 0x2022); // Bullet

        // Fallback for Cyrillic/symbols if primary monospace TTF omits them
        if (glyphs.find(0x0417) == glyphs.end() || glyphs.find(0x2022) == glyphs.end()) {
            const char *fbPaths[] = {
                "/system/fonts/Roboto-Regular.ttf",
                "/system/fonts/NotoSans-Regular.ttf",
                "/system/fonts/DroidSans.ttf"
            };
            for (const char *fbp : fbPaths) {
                FILE *fbf = std::fopen(fbp, "rb");
                if (!fbf) continue;
                std::fseek(fbf, 0, SEEK_END);
                long fsz = std::ftell(fbf);
                std::fseek(fbf, 0, SEEK_SET);
                if (fsz > 0 && fsz <= 16 * 1024 * 1024) {
                    std::vector<uint8_t> fbttf((size_t)fsz);
                    if (std::fread(fbttf.data(), 1, (size_t)fsz, fbf) == (size_t)fsz) {
                        stbtt_fontinfo fbinfo{};
                        if (stbtt_InitFont(&fbinfo, fbttf.data(), stbtt_GetFontOffsetForIndex(fbttf.data(), 0))) {
                            float fscale = stbtt_ScaleForPixelHeight(&fbinfo, basePixelHeight);
                            int fa, fd, flg;
                            stbtt_GetFontVMetrics(&fbinfo, &fa, &fd, &flg);
                            float fascent = fa * fscale;
                            packFrom(fbinfo, fscale, fascent, 0x0400, 0x045F);
                            packFrom(fbinfo, fscale, fascent, 0x2022, 0x2022);
                        }
                    }
                }
                std::fclose(fbf);
                if (glyphs.find(0x0417) != glyphs.end()) break;
            }
        }

        loadedTtf = true;
        activePath = path;
        atlasVersion++;
        return true;
    }

    void buildFallback8x8() {
        atlasRgba.assign(kAtlasSize * kAtlasSize * 4, 0);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int idx = (y * kAtlasSize + x) * 4;
                atlasRgba[idx + 0] = atlasRgba[idx + 1] = atlasRgba[idx + 2] = atlasRgba[idx + 3] = 255;
            }
        glyphs.clear();
        basePixelHeight = 16.0f;
        ascent = 13.0f;
        lineHeight = 18.0f;
        FontGlyph sp{0, 0, 0, 0, 0, 0, 0, 0, 9.0f};
        glyphs[' '] = sp;
        loadedTtf = false;
        atlasVersion++;
    }

    float measureWidth(const std::string &text, float pixelSize) const {
        float s = pixelSize / basePixelHeight;
        float w = 0.0f;
        const char *p = text.data();
        const char *end = p + text.size();
        while (p < end) {
            uint32_t cp = nextUtf8(p, end);
            auto it = glyphs.find(cp);
            if (it == glyphs.end()) it = glyphs.find('?');
            if (it != glyphs.end()) w += it->second.advance * s;
            else w += pixelSize * 0.55f;
        }
        return w;
    }

    static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
    }
};
