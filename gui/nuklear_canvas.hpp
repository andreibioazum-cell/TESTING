#pragma once

#include "text.hpp"
#include "geometry.hpp"
/* Nuklear supplies its own extern "C" guards for C++ consumers. */
#include "third_party/nuklear.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

/* Nuklear command-list renderer for LuaC's Vulkan vertex pipeline.
   Unicode glyphs and images use texture batches; solid geometry uses white. */
class NuklearCanvas {
public:
    std::vector<LcVertex> vertices;
    std::vector<LcDraw> draws;

    NuklearCanvas() {
        vertices.reserve(65536);
        draws.reserve(256);
    }
    const LcTextAtlas &atlas() const { return atlas_; }
    LcTextAtlas &atlas() { return atlas_; }

    void render(nk_context &context, int pixelWidth, int pixelHeight) {
        vertices.clear();
        draws.clear();
        clip_ = {0, 0, std::max(pixelWidth, 0), std::max(pixelHeight, 0)};
        const nk_command *command = nullptr;
        nk_foreach(command, &context) {
            switch (command->type) {
            case NK_COMMAND_NOP: break;
            case NK_COMMAND_SCISSOR: {
                const auto *c = reinterpret_cast<const nk_command_scissor *>(command);
                clip_ = intersect(c->x, c->y, c->w, c->h);
                break;
            }
            case NK_COMMAND_LINE: {
                const auto *c = reinterpret_cast<const nk_command_line *>(command);
                line(c->begin.x, c->begin.y, c->end.x, c->end.y,
                     static_cast<float>(c->line_thickness), color(c->color));
                break;
            }
            case NK_COMMAND_CURVE: {
                const auto *c = reinterpret_cast<const nk_command_curve *>(command);
                float x = static_cast<float>(c->begin.x), y = static_cast<float>(c->begin.y);
                for (int i = 1; i <= 20; ++i) {
                    float t = i / 20.0f, u = 1.0f - t;
                    float nx = u*u*u*c->begin.x + 3*u*u*t*c->ctrl[0].x +
                               3*u*t*t*c->ctrl[1].x + t*t*t*c->end.x;
                    float ny = u*u*u*c->begin.y + 3*u*u*t*c->ctrl[0].y +
                               3*u*t*t*c->ctrl[1].y + t*t*t*c->end.y;
                    line(x, y, nx, ny, static_cast<float>(c->line_thickness), color(c->color));
                    x = nx; y = ny;
                }
                break;
            }
            case NK_COMMAND_RECT: {
                const auto *c = reinterpret_cast<const nk_command_rect *>(command);
                if (!visible(c->x - c->line_thickness, c->y - c->line_thickness,
                             c->w + c->line_thickness * 2, c->h + c->line_thickness * 2)) break;
                roundedRect(c->x, c->y, c->w, c->h, c->rounding,
                            static_cast<float>(c->line_thickness), color(c->color), false);
                break;
            }
            case NK_COMMAND_RECT_FILLED: {
                const auto *c = reinterpret_cast<const nk_command_rect_filled *>(command);
                if (!visible(c->x, c->y, c->w, c->h)) break;
                roundedRect(c->x, c->y, c->w, c->h, c->rounding, 0, color(c->color), true);
                break;
            }
            case NK_COMMAND_RECT_MULTI_COLOR: {
                const auto *c = reinterpret_cast<const nk_command_rect_multi_color *>(command);
                if (!visible(c->x, c->y, c->w, c->h) || c->w <= 0 || c->h <= 0) break;
                Color tl = color(c->left), tr = color(c->top);
                Color br = color(c->right), bl = color(c->bottom);
                float x0 = static_cast<float>(c->x), y0 = static_cast<float>(c->y);
                float x1 = x0 + c->w, y1 = y0 + c->h;
                uint32_t first = static_cast<uint32_t>(vertices.size());
                vertex(x0, y0, tl); vertex(x1, y0, tr); vertex(x0, y1, bl);
                vertex(x1, y0, tr); vertex(x1, y1, br); vertex(x0, y1, bl);
                batch(first);
                break;
            }
            case NK_COMMAND_TRIANGLE: {
                const auto *c = reinterpret_cast<const nk_command_triangle *>(command);
                line(c->a.x, c->a.y, c->b.x, c->b.y, c->line_thickness, color(c->color));
                line(c->b.x, c->b.y, c->c.x, c->c.y, c->line_thickness, color(c->color));
                line(c->c.x, c->c.y, c->a.x, c->a.y, c->line_thickness, color(c->color));
                break;
            }
            case NK_COMMAND_TRIANGLE_FILLED: {
                const auto *c = reinterpret_cast<const nk_command_triangle_filled *>(command);
                triangle(c->a.x, c->a.y, c->b.x, c->b.y, c->c.x, c->c.y, color(c->color));
                break;
            }
            case NK_COMMAND_CIRCLE: {
                const auto *c = reinterpret_cast<const nk_command_circle *>(command);
                if (!visible(c->x - c->line_thickness, c->y - c->line_thickness,
                             c->w + c->line_thickness * 2, c->h + c->line_thickness * 2)) break;
                circle(c->x, c->y, c->w, c->h, c->line_thickness, color(c->color), false);
                break;
            }
            case NK_COMMAND_CIRCLE_FILLED: {
                const auto *c = reinterpret_cast<const nk_command_circle_filled *>(command);
                if (!visible(c->x, c->y, c->w, c->h)) break;
                circle(c->x, c->y, c->w, c->h, 0, color(c->color), true);
                break;
            }
            case NK_COMMAND_ARC: {
                const auto *c = reinterpret_cast<const nk_command_arc *>(command);
                arc(c->cx, c->cy, c->r, c->r, c->a[0], c->a[1], c->line_thickness,
                    color(c->color), false);
                break;
            }
            case NK_COMMAND_ARC_FILLED: {
                const auto *c = reinterpret_cast<const nk_command_arc_filled *>(command);
                arc(c->cx, c->cy, c->r, c->r, c->a[0], c->a[1], 0,
                    color(c->color), true);
                break;
            }
            case NK_COMMAND_POLYGON: {
                const auto *c = reinterpret_cast<const nk_command_polygon *>(command);
                polyline(c->points, c->point_count, c->line_thickness, color(c->color), true);
                break;
            }
            case NK_COMMAND_POLYGON_FILLED: {
                const auto *c = reinterpret_cast<const nk_command_polygon_filled *>(command);
                if (c->point_count >= 3)
                    for (unsigned i = 1; i + 1 < c->point_count; ++i)
                        triangle(c->points[0].x, c->points[0].y, c->points[i].x, c->points[i].y,
                                 c->points[i+1].x, c->points[i+1].y, color(c->color));
                break;
            }
            case NK_COMMAND_POLYLINE: {
                const auto *c = reinterpret_cast<const nk_command_polyline *>(command);
                polyline(c->points, c->point_count, c->line_thickness, color(c->color), false);
                break;
            }
            case NK_COMMAND_TEXT: {
                const auto *c = reinterpret_cast<const nk_command_text *>(command);
                if (!visible(c->x, c->y, c->w, c->h)) break;
                text(c->x, c->y, c->string, c->length, c->height, color(c->foreground));
                break;
            }
            case NK_COMMAND_IMAGE: {
                const auto *c = reinterpret_cast<const nk_command_image *>(command);
                if (!visible(c->x, c->y, c->w, c->h)) break;
                image(c->x, c->y, c->w, c->h, c->img, color(c->col));
                break;
            }
            case NK_COMMAND_CUSTOM: break;
            }
        }
        if (vertices.size() > UINT32_MAX) vertices.resize(UINT32_MAX);
    }

    float textWidth(float height, const char *value, int length) {
        float width = 0;
        for (int i = 0; i < length;) {
            int bytes = 1;
            std::uint32_t cp = decodeUtf8(value, length, i, bytes);
            width += atlas_.glyph(cp, height).advance;
            i += bytes;
        }
        return width;
    }

private:
    struct Color { float r, g, b, a; };
    struct Clip { int x, y, w, h; };
    Clip clip_{};
    LcTextAtlas atlas_;

    bool visible(float x, float y, float w, float h) const {
        if (clip_.w <= 0 || clip_.h <= 0 || w <= 0 || h <= 0) return false;
        return !(x + w <= clip_.x || x >= clip_.x + clip_.w ||
                 y + h <= clip_.y || y >= clip_.y + clip_.h);
    }

    static std::uint32_t decodeUtf8(const char *value, int length, int offset, int &bytes) {
        unsigned char lead = static_cast<unsigned char>(value[offset]);
        std::uint32_t cp = 0xfffd;
        if (lead < 0x80) cp = lead;
        else if ((lead & 0xe0) == 0xc0 && offset + 1 < length) { cp = lead & 0x1f; bytes = 2; }
        else if ((lead & 0xf0) == 0xe0 && offset + 2 < length) { cp = lead & 0x0f; bytes = 3; }
        else if ((lead & 0xf8) == 0xf0 && offset + 3 < length) { cp = lead & 0x07; bytes = 4; }
        for (int j = 1; j < bytes; ++j) {
            unsigned char continuation = static_cast<unsigned char>(value[offset+j]);
            if ((continuation & 0xc0) != 0x80) { cp = 0xfffd; bytes = 1; break; }
            cp = (cp << 6) | (continuation & 0x3f);
        }
        return cp;
    }
    static Color color(nk_color c) {
        constexpr float inv255 = 1.0f / 255.0f;
        return {c.r * inv255, c.g * inv255, c.b * inv255, c.a * inv255};
    }
    static Clip intersect(int x, int y, int w, int h) {
        int x2 = std::max(x, 0), y2 = std::max(y, 0);
        int right = std::max(x2, x + std::max(w, 0));
        int bottom = std::max(y2, y + std::max(h, 0));
        return {x2, y2, right - x2, bottom - y2};
    }
    void vertex(float x, float y, Color c) {
        if (clip_.w <= 0 || clip_.h <= 0) return;
        vertices.push_back({x, y, c.r, c.g, c.b, c.a, 0, 0});
    }
    void vertexUv(float x, float y, float u, float v, Color c) {
        if (clip_.w <= 0 || clip_.h <= 0) return;
        vertices.push_back({x, y, c.r, c.g, c.b, c.a, u, v});
    }
    void batch(uint32_t first, uint32_t texture = 0) {
        uint32_t count = static_cast<uint32_t>(vertices.size()) - first;
        if (!count) return;
        if (!draws.empty()) {
            LcDraw &last = draws.back();
            if (last.first + last.count == first && last.x == clip_.x && last.y == clip_.y &&
                last.width == static_cast<uint32_t>(clip_.w) &&
                last.height == static_cast<uint32_t>(clip_.h) && last.texture == texture) {
                last.count += count;
                return;
            }
        }
        draws.push_back({first, count, clip_.x, clip_.y,
                         static_cast<uint32_t>(clip_.w), static_cast<uint32_t>(clip_.h), texture});
    }
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
        uint32_t first = static_cast<uint32_t>(vertices.size());
        vertex(x0, y0, c); vertex(x1, y1, c); vertex(x2, y2, c); batch(first);
    }
    void rect(float x, float y, float w, float h, Color c) {
        if (w <= 0 || h <= 0) return;
        uint32_t first = static_cast<uint32_t>(vertices.size());
        vertex(x, y, c); vertex(x + w, y, c); vertex(x, y + h, c);
        vertex(x + w, y, c); vertex(x + w, y + h, c); vertex(x, y + h, c); batch(first);
    }
    void line(float x0, float y0, float x1, float y1, float thickness, Color c) {
        float dx = x1 - x0, dy = y1 - y0, lenSq = dx * dx + dy * dy;
        if (lenSq < 1e-6f) return;
        float invLen = 1.0f / std::sqrt(lenSq);
        float half = std::max(0.5f, thickness) * 0.5f * invLen;
        float nx = -dy * half, ny = dx * half;
        uint32_t first = static_cast<uint32_t>(vertices.size());
        vertex(x0 + nx, y0 + ny, c); vertex(x1 + nx, y1 + ny, c); vertex(x0 - nx, y0 - ny, c);
        vertex(x1 + nx, y1 + ny, c); vertex(x1 - nx, y1 - ny, c); vertex(x0 - nx, y0 - ny, c); batch(first);
    }
    void roundedRect(float x, float y, float w, float h, float radius,
                     float thickness, Color c, bool filled) {
        if (w <= 0 || h <= 0) return;
        float r = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
        if (r < 0.5f) {
            if (filled) rect(x, y, w, h, c);
            else {
                line(x, y, x + w, y, thickness, c); line(x + w, y, x + w, y + h, thickness, c);
                line(x + w, y + h, x, y + h, thickness, c); line(x, y + h, x, y, thickness, c);
            }
            return;
        }
        /* Precomputed unit quarter-circle (5 segments): cos/sin(i * pi / 10). */
        constexpr int segments = 5;
        static constexpr float qx[segments + 1] = {
            1.0f, 0.95105652f, 0.80901699f, 0.58778525f, 0.30901699f, 0.0f
        };
        static constexpr float qy[segments + 1] = {
            0.0f, 0.30901699f, 0.58778525f, 0.80901699f, 0.95105652f, 1.0f
        };
        if (filled) {
            rect(x + r, y, w - 2 * r, h, c);
            rect(x, y + r, r, h - 2 * r, c);
            rect(x + w - r, y + r, r, h - 2 * r, c);
            const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
            const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
            const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            const float sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
            uint32_t first = static_cast<uint32_t>(vertices.size());
            for (int corner = 0; corner < 4; ++corner) {
                for (int i = 0; i < segments; ++i) {
                    float px0 = (corner & 1) ? qx[i] : qy[i];
                    float py0 = (corner & 1) ? qy[i] : qx[i];
                    float px1 = (corner & 1) ? qx[i + 1] : qy[i + 1];
                    float py1 = (corner & 1) ? qy[i + 1] : qx[i + 1];
                    vertex(cx[corner], cy[corner], c);
                    vertex(cx[corner] + sx[corner] * r * px0, cy[corner] + sy[corner] * r * py0, c);
                    vertex(cx[corner] + sx[corner] * r * px1, cy[corner] + sy[corner] * r * py1, c);
                }
            }
            batch(first);
        } else {
            struct nk_vec2i points[4 * (segments + 1)];
            /* Clockwise outline: top-right -> bottom-right -> bottom-left -> top-left. */
            const float centers[4][2] = {{x + w - r, y + r}, {x + w - r, y + h - r}, {x + r, y + h - r}, {x + r, y + r}};
            const float sx[4] = {1.0f, 1.0f, -1.0f, -1.0f};
            const float sy[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            for (int corner = 0; corner < 4; ++corner) {
                for (int i = 0; i <= segments; ++i) {
                    float px = (corner & 1) ? qx[i] : qy[i];
                    float py = (corner & 1) ? qy[i] : qx[i];
                    points[corner * (segments + 1) + i] = {
                        static_cast<short>(centers[corner][0] + sx[corner] * r * px),
                        static_cast<short>(centers[corner][1] + sy[corner] * r * py)};
                }
            }
            polyline(points, 4 * (segments + 1), thickness, c, true);
        }
    }
    void circle(float x, float y, float w, float h, float thickness, Color c, bool filled) {
        arc(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, 0, 6.2831853f, thickness, c, filled);
    }
    void arc(float cx, float cy, float rx, float ry, float begin, float end,
             float thickness, Color c, bool filled) {
        constexpr int segments = 32;
        float step = (end - begin) / segments;
        float cs = std::cos(step), sn = std::sin(step);
        float dx = std::cos(begin), dy = std::sin(begin);
        for (int i = 0; i < segments; ++i) {
            float ndx = dx * cs - dy * sn;
            float ndy = dx * sn + dy * cs;
            float x0 = cx + rx * dx, y0 = cy + ry * dy;
            float x1 = cx + rx * ndx, y1 = cy + ry * ndy;
            if (filled) triangle(cx, cy, x0, y0, x1, y1, c);
            else line(x0, y0, x1, y1, thickness, c);
            dx = ndx; dy = ndy;
        }
    }
    void polyline(const struct nk_vec2i *points, unsigned count, float thickness, Color c, bool closed) {
        if (count < 2) return;
        for (unsigned i = 1; i < count; ++i) line(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, thickness, c);
        if (closed) line(points[count - 1].x, points[count - 1].y, points[0].x, points[0].y, thickness, c);
    }
    void image(float x, float y, float w, float h, const struct nk_image &img, Color c) {
        if (w <= 0 || h <= 0 || img.handle.id <= 0) return;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (img.w && img.h) {
            u0 = static_cast<float>(img.region[0]) / img.w;
            v0 = static_cast<float>(img.region[1]) / img.h;
            u1 = static_cast<float>(img.region[0] + img.region[2]) / img.w;
            v1 = static_cast<float>(img.region[1] + img.region[3]) / img.h;
        }
        uint32_t first = static_cast<uint32_t>(vertices.size());
        vertexUv(x, y, u0, v0, c); vertexUv(x + w, y, u1, v0, c); vertexUv(x, y + h, u0, v1, c);
        vertexUv(x + w, y, u1, v0, c); vertexUv(x + w, y + h, u1, v1, c); vertexUv(x, y + h, u0, v1, c);
        batch(first, static_cast<uint32_t>(img.handle.id));
    }
    void text(float x, float y, const char *value, int length, float height, Color c) {
        float baseline = y + atlas_.ascent(height);
        for (int i = 0; i < length;) {
            int bytes = 1;
            std::uint32_t cp = decodeUtf8(value, length, i, bytes);
            LcGlyph g = atlas_.glyph(cp, height);
            if (g.u1 > g.u0) {
                float x0 = x + g.x0, y0 = baseline + g.y0, x1 = x + g.x1, y1 = baseline + g.y1;
                uint32_t first = static_cast<uint32_t>(vertices.size());
                vertexUv(x0, y0, g.u0, g.v0, c); vertexUv(x1, y0, g.u1, g.v0, c); vertexUv(x0, y1, g.u0, g.v1, c);
                vertexUv(x1, y0, g.u1, g.v0, c); vertexUv(x1, y1, g.u1, g.v1, c); vertexUv(x0, y1, g.u0, g.v1, c);
                batch(first, 1);
            }
            x += g.advance;
            i += bytes;
        }
    }
};
