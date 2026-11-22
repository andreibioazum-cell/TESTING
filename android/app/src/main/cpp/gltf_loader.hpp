#pragma once
#include "studio_scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

struct GltfKeyframe {
    float t = 0.0f;
    float v[4]{0, 0, 0, 1};
};

struct GltfChannel {
    int targetNode = -1;
    int path = 0;   // 0=translation, 1=rotation, 2=scale
    int interp = 0; // 0=LINEAR, 1=STEP, 2=CUBICSPLINE
    std::vector<GltfKeyframe> keys;
};

struct GltfAnimation {
    std::string name;
    float duration = 0.0f;
    std::vector<GltfChannel> channels;

    void recomputeDuration() {
        duration = 0.0f;
        for (const auto &ch : channels) {
            if (!ch.keys.empty()) duration = std::max(duration, ch.keys.back().t);
        }
    }
};

struct GltfVertex {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 1, nz = 0;
    float u = 0, v = 0;
    uint16_t j[4]{0, 0, 0, 0};
    float w[4]{0, 0, 0, 0};
    bool hasSkin = false;
};

struct GltfPrimitive {
    std::vector<GltfVertex> verts;
    std::vector<uint32_t> indices;
    uint32_t baseColorArgb = 0xFFFFFFFF;
    std::string texRelPath;
    int skinIndex = -1;
    int nodeIndex = -1;
};

struct GltfSkin {
    std::vector<int> joints;
    std::vector<std::array<float, 16>> inverseBind;
};

struct GltfNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    float t[3]{0, 0, 0};
    float r[4]{0, 0, 0, 1};
    float s[3]{1, 1, 1};
    bool hasMatrix = false;
    float matrix[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int meshIndex = -1;
    int skinIndex = -1;
};

struct GltfModelAsset {
    bool loaded = false;
    bool isGlb = false;
    std::string fullPath;
    std::string projectDir;
    std::vector<GltfNode> nodes;
    std::vector<int> rootNodes;
    std::vector<GltfSkin> skins;
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfAnimation> animations;
    float normCenter[3]{0, 0, 0};
    float normScale = 1.0f;

    static void matIdentity(float m[16]) {
        std::memset(m, 0, 16 * sizeof(float));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    static void matMul(float out[16], const float a[16], const float b[16]) {
        float r[16];
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                r[col * 4 + row] =
                    a[0 * 4 + row] * b[col * 4 + 0] +
                    a[1 * 4 + row] * b[col * 4 + 1] +
                    a[2 * 4 + row] * b[col * 4 + 2] +
                    a[3 * 4 + row] * b[col * 4 + 3];
            }
        }
        std::memcpy(out, r, sizeof(r));
    }

    static void trsToMat(const float t[3], const float qIn[4], const float s[3], float m[16]) {
        float ql = std::sqrt(qIn[0]*qIn[0] + qIn[1]*qIn[1] + qIn[2]*qIn[2] + qIn[3]*qIn[3]);
        float x = ql > 1e-6f ? qIn[0] / ql : 0.0f;
        float y = ql > 1e-6f ? qIn[1] / ql : 0.0f;
        float z = ql > 1e-6f ? qIn[2] / ql : 0.0f;
        float w = ql > 1e-6f ? qIn[3] / ql : 1.0f;
        float xx = x*x, yy = y*y, zz = z*z;
        float xy = x*y, xz = x*z, yz = y*z;
        float wx = w*x, wy = w*y, wz = w*z;

        m[0]  = (1.0f - 2.0f * (yy + zz)) * s[0];
        m[1]  = (2.0f * (xy + wz)) * s[0];
        m[2]  = (2.0f * (xz - wy)) * s[0];
        m[3]  = 0.0f;
        m[4]  = (2.0f * (xy - wz)) * s[1];
        m[5]  = (1.0f - 2.0f * (xx + zz)) * s[1];
        m[6]  = (2.0f * (yz + wx)) * s[1];
        m[7]  = 0.0f;
        m[8]  = (2.0f * (xz + wy)) * s[2];
        m[9]  = (2.0f * (yz - wx)) * s[2];
        m[10] = (1.0f - 2.0f * (xx + yy)) * s[2];
        m[11] = 0.0f;
        m[12] = t[0];
        m[13] = t[1];
        m[14] = t[2];
        m[15] = 1.0f;
    }

    static void quatFromEulerDeg(float rxDeg, float ryDeg, float rzDeg, float out[4]) {
        constexpr float kHalf = 3.14159265f / 360.0f;
        float cx = std::cos(rxDeg * kHalf), sx = std::sin(rxDeg * kHalf);
        float cy = std::cos(ryDeg * kHalf), sy = std::sin(ryDeg * kHalf);
        float cz = std::cos(rzDeg * kHalf), sz = std::sin(rzDeg * kHalf);
        out[0] = sx * cy * cz - cx * sy * sz;
        out[1] = cx * sy * cz + sx * cy * sz;
        out[2] = cx * cy * sz - sx * sy * cz;
        out[3] = cx * cy * cz + sx * sy * sz;
    }

    static void eulerDegFromQuat(const float q[4], float &rxDeg, float &ryDeg, float &rzDeg) {
        constexpr float kRad = 180.0f / 3.14159265f;
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float sinr = 2.0f * (w * x + y * z);
        float cosr = 1.0f - 2.0f * (x * x + y * y);
        rxDeg = std::atan2(sinr, cosr) * kRad;
        float sinp = std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
        ryDeg = std::asin(sinp) * kRad;
        float siny = 2.0f * (w * z + x * y);
        float cosy = 1.0f - 2.0f * (y * y + z * z);
        rzDeg = std::atan2(siny, cosy) * kRad;
    }

    static void slerpQuat(const float a[4], const float bIn[4], float u, float out[4]) {
        float b[4]{bIn[0], bIn[1], bIn[2], bIn[3]};
        float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
        if (dot < 0.0f) {
            dot = -dot;
            for (int i = 0; i < 4; ++i) b[i] = -b[i];
        }
        if (dot > 0.9995f) {
            for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * u;
        } else {
            float th0 = std::acos(std::clamp(dot, -1.0f, 1.0f));
            float th = th0 * u;
            float s0 = std::cos(th) - dot * std::sin(th) / std::sin(th0);
            float s1 = std::sin(th) / std::sin(th0);
            for (int i = 0; i < 4; ++i) out[i] = s0 * a[i] + s1 * b[i];
        }
        float l = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
        if (l > 1e-6f) for (int i = 0; i < 4; ++i) out[i] /= l;
    }

    static std::vector<uint8_t> decodeBase64(const std::string &in) {
        static const int T[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };
        std::vector<uint8_t> out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            int d = T[c];
            if (d == -1) continue;
            val = (val << 6) + d;
            valb += 6;
            if (valb >= 0) {
                out.push_back((uint8_t)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    static std::string encodeBase64(const uint8_t *data, size_t len) {
        static const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)data[i] << 16;
            if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
            if (i + 2 < len) v |= (uint32_t)data[i + 2];
            out.push_back(B[(v >> 18) & 63]);
            out.push_back(B[(v >> 12) & 63]);
            out.push_back((i + 1 < len) ? B[(v >> 6) & 63] : '=');
            out.push_back((i + 2 < len) ? B[v & 63] : '=');
        }
        return out;
    }

    static bool readAllBytes(const std::string &path, std::vector<uint8_t> &out) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        std::streamsize sz = f.tellg();
        if (sz <= 0) return false;
        out.resize((size_t)sz);
        f.seekg(0, std::ios::beg);
        return (bool)f.read(reinterpret_cast<char *>(out.data()), sz);
    }

    static uint32_t readU32LE(const uint8_t *p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static void writeU32LE(uint8_t *p, uint32_t v) {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)((v >> 8) & 0xFF);
        p[2] = (uint8_t)((v >> 16) & 0xFF);
        p[3] = (uint8_t)((v >> 24) & 0xFF);
    }

    static int numCompsForType(const std::string &tp) {
        if (tp == "VEC2") return 2;
        if (tp == "VEC3") return 3;
        if (tp == "VEC4") return 4;
        if (tp == "MAT4") return 16;
        return 1;
    }

    static size_t compByteSize(int ct) {
        if (ct == 5120 || ct == 5121) return 1;
        if (ct == 5122 || ct == 5123) return 2;
        return 4;
    }

    static float readComponentFloat(const uint8_t *p, int ct, bool norm) {
        if (ct == 5126) {
            float v = 0; std::memcpy(&v, p, 4); return v;
        }
        if (ct == 5121) { // UNSIGNED_BYTE
            return norm ? (float)p[0] / 255.0f : (float)p[0];
        }
        if (ct == 5120) { // BYTE
            int8_t v = (int8_t)p[0];
            return norm ? std::max((float)v / 127.0f, -1.0f) : (float)v;
        }
        if (ct == 5123) { // UNSIGNED_SHORT
            uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            return norm ? (float)v / 65535.0f : (float)v;
        }
        if (ct == 5122) { // SHORT
            int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            return norm ? std::max((float)v / 32767.0f, -1.0f) : (float)v;
        }
        if (ct == 5125) { // UNSIGNED_INT
            return (float)readU32LE(p);
        }
        return 0.0f;
    }

    static uint32_t readComponentU32(const uint8_t *p, int ct) {
        if (ct == 5121) return p[0];
        if (ct == 5123) return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        if (ct == 5125) return readU32LE(p);
        return 0;
    }

    static bool readAccessorFloats(const JsonVal &root, const std::vector<std::vector<uint8_t>> &bufs,
                                   int accIdx, int &outCount, int &outComps, std::vector<float> &out) {
        const JsonVal *accs = root.get("accessors");
        const JsonVal *bvs  = root.get("bufferViews");
        if (!accs || accs->type != JsonVal::J_ARR || accIdx < 0 || accIdx >= (int)accs->arr.size()) return false;
        const JsonVal &acc = accs->arr[(size_t)accIdx];
        outCount = (int)acc.d("count", 0);
        outComps = numCompsForType(acc.s("type", "SCALAR"));
        int ct = (int)acc.d("componentType", 5126);
        bool norm = acc.bl("normalized", false);
        int bvIdx = (int)acc.d("bufferView", -1);
        size_t accOff = (size_t)std::max(0.0, acc.d("byteOffset", 0));
        out.assign((size_t)outCount * (size_t)outComps, 0.0f);
        if (bvIdx < 0 || !bvs || bvs->type != JsonVal::J_ARR || bvIdx >= (int)bvs->arr.size()) return true;
        const JsonVal &bv = bvs->arr[(size_t)bvIdx];
        int bufIdx = (int)bv.d("buffer", 0);
        if (bufIdx < 0 || bufIdx >= (int)bufs.size()) return false;
        const auto &buf = bufs[(size_t)bufIdx];
        size_t bvOff = (size_t)std::max(0.0, bv.d("byteOffset", 0));
        size_t csz = compByteSize(ct);
        size_t stride = (size_t)bv.d("byteStride", 0);
        if (stride < csz * (size_t)outComps) stride = csz * (size_t)outComps;
        for (int i = 0; i < outCount; ++i) {
            size_t base = bvOff + accOff + (size_t)i * stride;
            for (int c = 0; c < outComps; ++c) {
                size_t p = base + (size_t)c * csz;
                if (p + csz <= buf.size()) {
                    out[(size_t)i * (size_t)outComps + (size_t)c] = readComponentFloat(&buf[p], ct, norm);
                }
            }
        }
        return true;
    }

    static bool readAccessorU32(const JsonVal &root, const std::vector<std::vector<uint8_t>> &bufs,
                                int accIdx, std::vector<uint32_t> &out) {
        const JsonVal *accs = root.get("accessors");
        const JsonVal *bvs  = root.get("bufferViews");
        if (!accs || accs->type != JsonVal::J_ARR || accIdx < 0 || accIdx >= (int)accs->arr.size()) return false;
        const JsonVal &acc = accs->arr[(size_t)accIdx];
        int count = (int)acc.d("count", 0);
        int comps = numCompsForType(acc.s("type", "SCALAR"));
        int ct = (int)acc.d("componentType", 5123);
        int bvIdx = (int)acc.d("bufferView", -1);
        size_t accOff = (size_t)std::max(0.0, acc.d("byteOffset", 0));
        out.assign((size_t)count * (size_t)comps, 0);
        if (bvIdx < 0 || !bvs || bvs->type != JsonVal::J_ARR || bvIdx >= (int)bvs->arr.size()) return false;
        const JsonVal &bv = bvs->arr[(size_t)bvIdx];
        int bufIdx = (int)bv.d("buffer", 0);
        if (bufIdx < 0 || bufIdx >= (int)bufs.size()) return false;
        const auto &buf = bufs[(size_t)bufIdx];
        size_t bvOff = (size_t)std::max(0.0, bv.d("byteOffset", 0));
        size_t csz = compByteSize(ct);
        size_t stride = (size_t)bv.d("byteStride", 0);
        if (stride < csz * (size_t)comps) stride = csz * (size_t)comps;
        for (int i = 0; i < count; ++i) {
            size_t base = bvOff + accOff + (size_t)i * stride;
            for (int c = 0; c < comps; ++c) {
                size_t p = base + (size_t)c * csz;
                if (p + csz <= buf.size()) out[(size_t)i * (size_t)comps + (size_t)c] = readComponentU32(&buf[p], ct);
            }
        }
        return true;
    }

    static bool load(const std::string &path, const std::string &projDir, GltfModelAsset &out) {
        out = GltfModelAsset{};
        out.fullPath = path;
        out.projectDir = projDir;
        std::vector<uint8_t> raw;
        if (!readAllBytes(path, raw) || raw.size() < 12) return false;

        std::string dir = ".";
        if (auto sl = path.find_last_of('/'); sl != std::string::npos) dir = path.substr(0, sl);

        std::string jsonStr;
        std::vector<std::vector<uint8_t>> buffers;

        if (readU32LE(raw.data()) == 0x46546C67u) { // "glTF" GLB magic
            out.isGlb = true;
            size_t off = 12;
            std::vector<uint8_t> binChunk;
            while (off + 8 <= raw.size()) {
                uint32_t cLen = readU32LE(&raw[off]);
                uint32_t cType = readU32LE(&raw[off + 4]);
                off += 8;
                if (off + cLen > raw.size()) break;
                if (cType == 0x4E4F534Au) { // JSON
                    jsonStr.assign(reinterpret_cast<const char *>(&raw[off]), cLen);
                } else if (cType == 0x004E4942u) { // BIN
                    binChunk.assign(raw.begin() + (ptrdiff_t)off, raw.begin() + (ptrdiff_t)(off + cLen));
                }
                off += cLen;
            }
            if (jsonStr.empty()) return false;
            buffers.push_back(std::move(binChunk));
        } else {
            out.isGlb = false;
            jsonStr.assign(raw.begin(), raw.end());
        }

        JsonVal root = MiniJson::parse(jsonStr);
        if (root.type != JsonVal::J_OBJ) return false;

        if (const JsonVal *bufArr = root.get("buffers"); bufArr && bufArr->type == JsonVal::J_ARR) {
            for (size_t bi = 0; bi < bufArr->arr.size(); ++bi) {
                std::string uri = bufArr->arr[bi].s("uri", "");
                if (out.isGlb && bi == 0 && uri.empty()) continue;
                std::vector<uint8_t> bdata;
                if (uri.rfind("data:", 0) == 0) {
                    auto comma = uri.find(',');
                    if (comma != std::string::npos) bdata = decodeBase64(uri.substr(comma + 1));
                } else if (!uri.empty()) {
                    readAllBytes(dir + "/" + uri, bdata);
                }
                if (bi < buffers.size()) buffers[bi] = std::move(bdata);
                else buffers.push_back(std::move(bdata));
            }
        }

        // Extract embedded images for materials into projectDir/assets/
        std::vector<std::string> imageRelPaths;
        if (const JsonVal *imgs = root.get("images"); imgs && imgs->type == JsonVal::J_ARR) {
            const JsonVal *bvs = root.get("bufferViews");
            for (size_t ii = 0; ii < imgs->arr.size(); ++ii) {
                const JsonVal &im = imgs->arr[ii];
                std::string uri = im.s("uri", "");
                std::string rel;
                if (!uri.empty() && uri.rfind("data:", 0) != 0) {
                    std::string abs = dir + "/" + uri;
                    if (!projDir.empty() && abs.rfind(projDir + "/", 0) == 0) {
                        rel = abs.substr(projDir.size() + 1);
                    }
                } else if (!projDir.empty()) {
                    std::vector<uint8_t> imgBytes;
                    std::string mime = im.s("mimeType", "image/png");
                    if (uri.rfind("data:", 0) == 0) {
                        auto comma = uri.find(',');
                        if (comma != std::string::npos) imgBytes = decodeBase64(uri.substr(comma + 1));
                        if (uri.find("jpeg") != std::string::npos) mime = "image/jpeg";
                    } else if (const JsonVal *bvIdxV = im.get("bufferView"); bvIdxV && bvIdxV->type == JsonVal::J_NUM && bvs && bvs->type == JsonVal::J_ARR) {
                        int bvi = (int)bvIdxV->num;
                        if (bvi >= 0 && bvi < (int)bvs->arr.size()) {
                            const JsonVal &bv = bvs->arr[(size_t)bvi];
                            int bi = (int)bv.d("buffer", 0);
                            size_t bo = (size_t)std::max(0.0, bv.d("byteOffset", 0));
                            size_t bl = (size_t)std::max(0.0, bv.d("byteLength", 0));
                            if (bi >= 0 && bi < (int)buffers.size() && bo + bl <= buffers[(size_t)bi].size()) {
                                imgBytes.assign(buffers[(size_t)bi].begin() + (ptrdiff_t)bo,
                                                buffers[(size_t)bi].begin() + (ptrdiff_t)(bo + bl));
                            }
                        }
                    }
                    if (!imgBytes.empty()) {
                        uint32_t h = 2166136261u;
                        for (uint8_t b : imgBytes) { h ^= b; h *= 16777619u; }
                        std::string ext = (mime.find("jpeg") != std::string::npos || mime.find("jpg") != std::string::npos) ? ".jpg" : ".png";
                        char nameBuf[64];
                        std::snprintf(nameBuf, sizeof(nameBuf), "assets/gltf_tex_%08x_%zu%s", h, ii, ext.c_str());
                        rel = nameBuf;
                        std::ofstream outF(projDir + "/" + rel, std::ios::binary | std::ios::trunc);
                        if (outF) outF.write(reinterpret_cast<const char *>(imgBytes.data()), (std::streamsize)imgBytes.size());
                    }
                }
                imageRelPaths.push_back(rel);
            }
        }

        // Parse nodes
        if (const JsonVal *nds = root.get("nodes"); nds && nds->type == JsonVal::J_ARR) {
            out.nodes.resize(nds->arr.size());
            for (size_t ni = 0; ni < nds->arr.size(); ++ni) {
                const JsonVal &nj = nds->arr[ni];
                GltfNode &gn = out.nodes[ni];
                gn.name = nj.s("name", "");
                if (gn.name.empty()) gn.name = "Node_" + std::to_string(ni);
                gn.meshIndex = (int)nj.d("mesh", -1);
                gn.skinIndex = (int)nj.d("skin", -1);
                if (const JsonVal *tr = nj.get("translation"); tr && tr->type == JsonVal::J_ARR && tr->arr.size() >= 3) {
                    for (int k = 0; k < 3; ++k) gn.t[k] = (float)tr->arr[(size_t)k].num;
                }
                if (const JsonVal *rt = nj.get("rotation"); rt && rt->type == JsonVal::J_ARR && rt->arr.size() >= 4) {
                    for (int k = 0; k < 4; ++k) gn.r[k] = (float)rt->arr[(size_t)k].num;
                }
                if (const JsonVal *sc = nj.get("scale"); sc && sc->type == JsonVal::J_ARR && sc->arr.size() >= 3) {
                    for (int k = 0; k < 3; ++k) gn.s[k] = (float)sc->arr[(size_t)k].num;
                }
                if (const JsonVal *mx = nj.get("matrix"); mx && mx->type == JsonVal::J_ARR && mx->arr.size() >= 16) {
                    gn.hasMatrix = true;
                    for (int k = 0; k < 16; ++k) gn.matrix[k] = (float)mx->arr[(size_t)k].num;
                }
                if (const JsonVal *ch = nj.get("children"); ch && ch->type == JsonVal::J_ARR) {
                    for (const auto &cv : ch->arr) {
                        int ci = (int)cv.num;
                        if (ci >= 0 && ci < (int)out.nodes.size()) {
                            gn.children.push_back(ci);
                            out.nodes[(size_t)ci].parent = (int)ni;
                        }
                    }
                }
            }
        }
        if (const JsonVal *scs = root.get("scenes"); scs && scs->type == JsonVal::J_ARR && !scs->arr.empty()) {
            int sIdx = std::clamp((int)root.d("scene", 0), 0, (int)scs->arr.size() - 1);
            if (const JsonVal *rn = scs->arr[(size_t)sIdx].get("nodes"); rn && rn->type == JsonVal::J_ARR) {
                for (const auto &nv : rn->arr) {
                    int ni = (int)nv.num;
                    if (ni >= 0 && ni < (int)out.nodes.size()) out.rootNodes.push_back(ni);
                }
            }
        }
        if (out.rootNodes.empty()) {
            for (size_t i = 0; i < out.nodes.size(); ++i) {
                if (out.nodes[i].parent == -1) out.rootNodes.push_back((int)i);
            }
        }

        // Parse skins
        if (const JsonVal *sks = root.get("skins"); sks && sks->type == JsonVal::J_ARR) {
            for (const auto &sj : sks->arr) {
                GltfSkin sk;
                if (const JsonVal *jts = sj.get("joints"); jts && jts->type == JsonVal::J_ARR) {
                    for (const auto &jv : jts->arr) sk.joints.push_back((int)jv.num);
                }
                int ibmAcc = (int)sj.d("inverseBindMatrices", -1);
                std::vector<float> mats;
                int cnt = 0, comps = 0;
                if (ibmAcc >= 0 && readAccessorFloats(root, buffers, ibmAcc, cnt, comps, mats) && comps == 16) {
                    sk.inverseBind.resize((size_t)cnt);
                    for (int i = 0; i < cnt; ++i) {
                        std::memcpy(sk.inverseBind[(size_t)i].data(), &mats[(size_t)i * 16], 16 * sizeof(float));
                    }
                } else {
                    sk.inverseBind.resize(sk.joints.size());
                    for (auto &m : sk.inverseBind) matIdentity(m.data());
                }
                out.skins.push_back(std::move(sk));
            }
        }

        // Parse meshes bound to nodes (or fallback unreferenced meshes)
        const JsonVal *meshes = root.get("meshes");
        const JsonVal *materials = root.get("materials");
        const JsonVal *textures = root.get("textures");

        auto loadMeshForNode = [&](int meshIdx, int nodeIdx, int skinIdx) {
            if (!meshes || meshes->type != JsonVal::J_ARR || meshIdx < 0 || meshIdx >= (int)meshes->arr.size()) return;
            const JsonVal *prims = meshes->arr[(size_t)meshIdx].get("primitives");
            if (!prims || prims->type != JsonVal::J_ARR) return;
            for (const auto &pj : prims->arr) {
                int mode = (int)pj.d("mode", 4);
                if (mode != 4) continue; // TRIANGLES only
                const JsonVal *attrs = pj.get("attributes");
                if (!attrs || attrs->type != JsonVal::J_OBJ) continue;
                int posAcc = (int)attrs->d("POSITION", -1);
                if (posAcc < 0) continue;

                std::vector<float> posF, normF, uvF, wgtF;
                std::vector<uint32_t> jntU;
                int pCnt = 0, pComps = 0, nCnt = 0, nComps = 0, uCnt = 0, uComps = 0, wCnt = 0, wComps = 0;
                if (!readAccessorFloats(root, buffers, posAcc, pCnt, pComps, posF) || pComps < 3 || pCnt <= 0) continue;

                int normAcc = (int)attrs->d("NORMAL", -1);
                bool hasNorm = (normAcc >= 0 && readAccessorFloats(root, buffers, normAcc, nCnt, nComps, normF) && nComps >= 3 && nCnt == pCnt);
                int uvAcc = (int)attrs->d("TEXCOORD_0", -1);
                bool hasUv = (uvAcc >= 0 && readAccessorFloats(root, buffers, uvAcc, uCnt, uComps, uvF) && uComps >= 2 && uCnt == pCnt);
                int jntAcc = (int)attrs->d("JOINTS_0", -1);
                int wgtAcc = (int)attrs->d("WEIGHTS_0", -1);
                bool hasSkin = (skinIdx >= 0 && jntAcc >= 0 && wgtAcc >= 0 &&
                                readAccessorU32(root, buffers, jntAcc, jntU) &&
                                readAccessorFloats(root, buffers, wgtAcc, wCnt, wComps, wgtF) &&
                                wCnt == pCnt && wComps >= 4 && jntU.size() >= (size_t)pCnt * 4);

                GltfPrimitive gp;
                gp.nodeIndex = nodeIdx;
                gp.skinIndex = skinIdx;
                gp.verts.resize((size_t)pCnt);
                for (int i = 0; i < pCnt; ++i) {
                    GltfVertex &gv = gp.verts[(size_t)i];
                    gv.x = posF[(size_t)i * (size_t)pComps + 0];
                    gv.y = posF[(size_t)i * (size_t)pComps + 1];
                    gv.z = posF[(size_t)i * (size_t)pComps + 2];
                    if (hasNorm) {
                        gv.nx = normF[(size_t)i * (size_t)nComps + 0];
                        gv.ny = normF[(size_t)i * (size_t)nComps + 1];
                        gv.nz = normF[(size_t)i * (size_t)nComps + 2];
                    }
                    if (hasUv) {
                        gv.u = uvF[(size_t)i * (size_t)uComps + 0];
                        gv.v = uvF[(size_t)i * (size_t)uComps + 1];
                    }
                    if (hasSkin) {
                        gv.hasSkin = true;
                        float ws = 0.0f;
                        for (int k = 0; k < 4; ++k) {
                            gv.j[k] = (uint16_t)jntU[(size_t)i * 4 + (size_t)k];
                            gv.w[k] = wgtF[(size_t)i * (size_t)wComps + (size_t)k];
                            ws += gv.w[k];
                        }
                        if (ws > 1e-6f) for (int k = 0; k < 4; ++k) gv.w[k] /= ws;
                        else gv.w[0] = 1.0f;
                    }
                }

                int idxAcc = (int)pj.d("indices", -1);
                if (idxAcc >= 0) {
                    readAccessorU32(root, buffers, idxAcc, gp.indices);
                } else {
                    gp.indices.resize((size_t)pCnt);
                    for (int i = 0; i < pCnt; ++i) gp.indices[(size_t)i] = (uint32_t)i;
                }

                if (!hasNorm) {
                    for (size_t ti = 0; ti + 2 < gp.indices.size(); ti += 3) {
                        uint32_t i0 = gp.indices[ti], i1 = gp.indices[ti + 1], i2 = gp.indices[ti + 2];
                        if (i0 < gp.verts.size() && i1 < gp.verts.size() && i2 < gp.verts.size()) {
                            Vec3f a{gp.verts[i0].x, gp.verts[i0].y, gp.verts[i0].z};
                            Vec3f b{gp.verts[i1].x, gp.verts[i1].y, gp.verts[i1].z};
                            Vec3f c{gp.verts[i2].x, gp.verts[i2].y, gp.verts[i2].z};
                            Vec3f n = norm3(cross3(b - a, c - a));
                            for (uint32_t idx : {i0, i1, i2}) {
                                gp.verts[idx].nx = n.x; gp.verts[idx].ny = n.y; gp.verts[idx].nz = n.z;
                            }
                        }
                    }
                }

                int matIdx = (int)pj.d("material", -1);
                if (matIdx >= 0 && materials && materials->type == JsonVal::J_ARR && matIdx < (int)materials->arr.size()) {
                    const JsonVal &mat = materials->arr[(size_t)matIdx];
                    if (const JsonVal *pbr = mat.get("pbrMetallicRoughness")) {
                        if (const JsonVal *bcf = pbr->get("baseColorFactor"); bcf && bcf->type == JsonVal::J_ARR && bcf->arr.size() >= 4) {
                            auto c8 = [](double d) -> uint32_t { return (uint32_t)std::clamp((int)std::lround(d * 255.0), 0, 255); };
                            gp.baseColorArgb = (c8(bcf->arr[3].num) << 24) | (c8(bcf->arr[0].num) << 16) |
                                               (c8(bcf->arr[1].num) << 8) | c8(bcf->arr[2].num);
                        }
                        if (const JsonVal *bct = pbr->get("baseColorTexture")) {
                            int texIdx = (int)bct->d("index", -1);
                            if (texIdx >= 0 && textures && textures->type == JsonVal::J_ARR && texIdx < (int)textures->arr.size()) {
                                int srcIdx = (int)textures->arr[(size_t)texIdx].d("source", -1);
                                if (srcIdx >= 0 && srcIdx < (int)imageRelPaths.size()) gp.texRelPath = imageRelPaths[(size_t)srcIdx];
                            }
                        }
                    }
                }
                out.primitives.push_back(std::move(gp));
            }
        };

        for (size_t ni = 0; ni < out.nodes.size(); ++ni) {
            if (out.nodes[ni].meshIndex >= 0) {
                loadMeshForNode(out.nodes[ni].meshIndex, (int)ni, out.nodes[ni].skinIndex);
            }
        }
        if (out.primitives.empty() && meshes && meshes->type == JsonVal::J_ARR) {
            for (size_t mi = 0; mi < meshes->arr.size(); ++mi) loadMeshForNode((int)mi, -1, -1);
        }
        if (out.primitives.empty()) return false;

        // Parse animations
        if (const JsonVal *anims = root.get("animations"); anims && anims->type == JsonVal::J_ARR) {
            for (size_t ai = 0; ai < anims->arr.size(); ++ai) {
                const JsonVal &aj = anims->arr[ai];
                GltfAnimation ga;
                ga.name = aj.s("name", "");
                if (ga.name.empty()) ga.name = "Anim_" + std::to_string(ai);
                const JsonVal *chans = aj.get("channels");
                const JsonVal *samps = aj.get("samplers");
                if (chans && chans->type == JsonVal::J_ARR && samps && samps->type == JsonVal::J_ARR) {
                    for (const auto &cj : chans->arr) {
                        const JsonVal *tgt = cj.get("target");
                        int sampIdx = (int)cj.d("sampler", -1);
                        if (!tgt || sampIdx < 0 || sampIdx >= (int)samps->arr.size()) continue;
                        int targetNode = (int)tgt->d("node", -1);
                        if (targetNode < 0 || targetNode >= (int)out.nodes.size()) continue;
                        std::string pathStr = tgt->s("path", "");
                        int pathCode = -1;
                        if (pathStr == "translation") pathCode = 0;
                        else if (pathStr == "rotation") pathCode = 1;
                        else if (pathStr == "scale") pathCode = 2;
                        if (pathCode < 0) continue;

                        const JsonVal &sj = samps->arr[(size_t)sampIdx];
                        int inAcc = (int)sj.d("input", -1);
                        int outAcc = (int)sj.d("output", -1);
                        std::vector<float> times, vals;
                        int tCnt = 0, tComps = 0, vCnt = 0, vComps = 0;
                        if (!readAccessorFloats(root, buffers, inAcc, tCnt, tComps, times) ||
                            !readAccessorFloats(root, buffers, outAcc, vCnt, vComps, vals) || tCnt <= 0) continue;

                        GltfChannel ch;
                        ch.targetNode = targetNode;
                        ch.path = pathCode;
                        std::string interpStr = sj.s("interpolation", "LINEAR");
                        ch.interp = (interpStr == "STEP") ? 1 : (interpStr == "CUBICSPLINE" ? 2 : 0);

                        int stride = (ch.interp == 2) ? 3 : 1;
                        for (int ki = 0; ki < tCnt; ++ki) {
                            int vi = ki * stride + (ch.interp == 2 ? 1 : 0);
                            if (vi >= vCnt) break;
                            GltfKeyframe kf;
                            kf.t = times[(size_t)ki];
                            for (int c = 0; c < std::min(4, vComps); ++c) {
                                kf.v[c] = vals[(size_t)vi * (size_t)vComps + (size_t)c];
                            }
                            if (pathCode == 1 && vComps < 4) kf.v[3] = 1.0f;
                            ch.keys.push_back(kf);
                        }
                        if (!ch.keys.empty()) ga.channels.push_back(std::move(ch));
                    }
                }
                ga.recomputeDuration();
                out.animations.push_back(std::move(ga));
            }
        }

        // Compute rest-pose normalization bounds so any scale model fits cleanly inside hostNode size
        std::vector<std::array<float, 16>> restMats;
        out.evaluatePose(-1, 0.0f, restMats);
        float minP[3]{1e9f, 1e9f, 1e9f}, maxP[3]{-1e9f, -1e9f, -1e9f};
        bool anyVert = false;
        for (const auto &prim : out.primitives) {
            float m[16];
            if (prim.nodeIndex >= 0 && prim.nodeIndex < (int)restMats.size() && prim.skinIndex < 0) {
                std::memcpy(m, restMats[(size_t)prim.nodeIndex].data(), 64);
            } else {
                matIdentity(m);
            }
            for (const auto &gv : prim.verts) {
                float wx = m[0]*gv.x + m[4]*gv.y + m[8]*gv.z + m[12];
                float wy = m[1]*gv.x + m[5]*gv.y + m[9]*gv.z + m[13];
                float wz = m[2]*gv.x + m[6]*gv.y + m[10]*gv.z + m[14];
                minP[0] = std::min(minP[0], wx); maxP[0] = std::max(maxP[0], wx);
                minP[1] = std::min(minP[1], wy); maxP[1] = std::max(maxP[1], wy);
                minP[2] = std::min(minP[2], wz); maxP[2] = std::max(maxP[2], wz);
                anyVert = true;
            }
        }
        if (anyVert) {
            for (int k = 0; k < 3; ++k) out.normCenter[k] = 0.5f * (minP[k] + maxP[k]);
            float ext = std::max({maxP[0] - minP[0], maxP[1] - minP[1], maxP[2] - minP[2], 1e-4f});
            out.normScale = 1.0f / ext;
        }
        out.loaded = true;
        return true;
    }

    static void sampleChannel(const GltfChannel &ch, float t, float out[4]) {
        const auto &ks = ch.keys;
        if (ks.empty()) return;
        if (ks.size() == 1 || t <= ks.front().t) {
            std::memcpy(out, ks.front().v, 16);
            return;
        }
        if (t >= ks.back().t) {
            std::memcpy(out, ks.back().v, 16);
            return;
        }
        size_t hi = 1;
        while (hi + 1 < ks.size() && ks[hi].t < t) hi++;
        size_t lo = hi - 1;
        if (ch.interp == 1) {
            std::memcpy(out, ks[lo].v, 16);
            return;
        }
        float dt = std::max(1e-6f, ks[hi].t - ks[lo].t);
        float u = std::clamp((t - ks[lo].t) / dt, 0.0f, 1.0f);
        if (ch.path == 1) slerpQuat(ks[lo].v, ks[hi].v, u, out);
        else for (int c = 0; c < 4; ++c) out[c] = ks[lo].v[c] + (ks[hi].v[c] - ks[lo].v[c]) * u;
    }

    void evaluatePose(int animIndex, float timeSec, std::vector<std::array<float, 16>> &outGlobal) const {
        size_t n = nodes.size();
        outGlobal.resize(n);
        std::vector<std::array<float, 16>> local(n);
        for (size_t i = 0; i < n; ++i) {
            const GltfNode &nd = nodes[i];
            if (nd.hasMatrix && animIndex < 0) {
                std::memcpy(local[i].data(), nd.matrix, 64);
            } else {
                trsToMat(nd.t, nd.r, nd.s, local[i].data());
            }
        }
        if (animIndex >= 0 && animIndex < (int)animations.size()) {
            const GltfAnimation &anim = animations[(size_t)animIndex];
            struct LocalTRS { float t[3], r[4], s[3]; bool ht = false, hr = false, hs = false; };
            std::vector<LocalTRS> ov(n);
            for (size_t i = 0; i < n; ++i) {
                std::memcpy(ov[i].t, nodes[i].t, 12);
                std::memcpy(ov[i].r, nodes[i].r, 16);
                std::memcpy(ov[i].s, nodes[i].s, 12);
            }
            float t = anim.duration > 1e-5f ? std::fmod(std::max(0.0f, timeSec), anim.duration) : 0.0f;
            for (const auto &ch : anim.channels) {
                if (ch.targetNode < 0 || ch.targetNode >= (int)n || ch.keys.empty()) continue;
                float v[4]{0, 0, 0, 1};
                sampleChannel(ch, t, v);
                LocalTRS &d = ov[(size_t)ch.targetNode];
                if (ch.path == 0) { std::memcpy(d.t, v, 12); d.ht = true; }
                else if (ch.path == 1) { std::memcpy(d.r, v, 16); d.hr = true; }
                else if (ch.path == 2) { std::memcpy(d.s, v, 12); d.hs = true; }
            }
            for (size_t i = 0; i < n; ++i) {
                if (ov[i].ht || ov[i].hr || ov[i].hs) {
                    trsToMat(ov[i].t, ov[i].r, ov[i].s, local[i].data());
                }
            }
        }

        std::vector<uint8_t> vis(n, 0);
        std::function<void(int, const float *)> walk = [&](int idx, const float *parentM) {
            if (idx < 0 || idx >= (int)n || vis[(size_t)idx]) return;
            vis[(size_t)idx] = 1;
            matMul(outGlobal[(size_t)idx].data(), parentM, local[(size_t)idx].data());
            for (int c : nodes[(size_t)idx].children) walk(c, outGlobal[(size_t)idx].data());
        };
        float ident[16]; matIdentity(ident);
        for (int r : rootNodes) walk(r, ident);
        for (size_t i = 0; i < n; ++i) if (!vis[i]) walk((int)i, ident);
    }

    void appendMesh(const SceneNode &hostNode, int animIndex, float timeSec, std::vector<MeshVertex> &out,
                    float fallbackU0, float fallbackV0, float fallbackU1, float fallbackV1,
                    const std::function<void(const std::string &, float &, float &, float &, float &)> &resolveTexUv) const {
        if (!loaded) return;
        std::vector<std::array<float, 16>> gMats;
        evaluatePose(animIndex, timeSec, gMats);

        // Precompute skin matrices for each skin
        std::vector<std::vector<std::array<float, 16>>> skinMats(skins.size());
        for (size_t si = 0; si < skins.size(); ++si) {
            const auto &sk = skins[si];
            skinMats[si].resize(sk.joints.size());
            for (size_t ji = 0; ji < sk.joints.size(); ++ji) {
                int jNode = sk.joints[ji];
                if (jNode >= 0 && jNode < (int)gMats.size() && ji < sk.inverseBind.size()) {
                    matMul(skinMats[si][ji].data(), gMats[(size_t)jNode].data(), sk.inverseBind[ji].data());
                } else {
                    matIdentity(skinMats[si][ji].data());
                }
            }
        }

        float hostR = (float)((hostNode.argbColor >> 16) & 0xFF) / 255.0f;
        float hostG = (float)((hostNode.argbColor >> 8)  & 0xFF) / 255.0f;
        float hostB = (float)( hostNode.argbColor        & 0xFF) / 255.0f;
        uint8_t hostA = (uint8_t)std::max(25, std::min(255, (int)std::lround((1.0f - hostNode.transparency) * 255.0f)));

        float scaleDim = std::max({hostNode.sizeX, hostNode.sizeY, hostNode.sizeZ}) * normScale;

        for (const auto &prim : primitives) {
            float u0 = fallbackU0, v0 = fallbackV0, u1 = fallbackU1, v1 = fallbackV1;
            if (hostNode.texturePath.empty() && !prim.texRelPath.empty() && resolveTexUv) {
                resolveTexUv(prim.texRelPath, u0, v0, u1, v1);
            }
            uint8_t r = (uint8_t)std::clamp((int)std::lround(((prim.baseColorArgb >> 16) & 0xFF) * hostR), 0, 255);
            uint8_t g = (uint8_t)std::clamp((int)std::lround(((prim.baseColorArgb >> 8)  & 0xFF) * hostG), 0, 255);
            uint8_t b = (uint8_t)std::clamp((int)std::lround(( prim.baseColorArgb        & 0xFF) * hostB), 0, 255);
            uint32_t col = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)hostA << 24);

            float nodeM[16];
            if (prim.nodeIndex >= 0 && prim.nodeIndex < (int)gMats.size() && prim.skinIndex < 0) {
                std::memcpy(nodeM, gMats[(size_t)prim.nodeIndex].data(), 64);
            } else {
                matIdentity(nodeM);
            }

            std::vector<Vec3f> xPos(prim.verts.size()), xNrm(prim.verts.size());
            for (size_t vi = 0; vi < prim.verts.size(); ++vi) {
                const GltfVertex &gv = prim.verts[vi];
                float lx = gv.x, ly = gv.y, lz = gv.z;
                float lnx = gv.nx, lny = gv.ny, lnz = gv.nz;
                if (gv.hasSkin && prim.skinIndex >= 0 && prim.skinIndex < (int)skinMats.size()) {
                    const auto &sm = skinMats[(size_t)prim.skinIndex];
                    float sx = 0, sy = 0, sz = 0, snx = 0, sny = 0, snz = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (gv.w[k] <= 0.0f || gv.j[k] >= sm.size()) continue;
                        const float *m = sm[gv.j[k]].data();
                        sx  += gv.w[k] * (m[0]*lx  + m[4]*ly  + m[8]*lz  + m[12]);
                        sy  += gv.w[k] * (m[1]*lx  + m[5]*ly  + m[9]*lz  + m[13]);
                        sz  += gv.w[k] * (m[2]*lx  + m[6]*ly  + m[10]*lz + m[14]);
                        snx += gv.w[k] * (m[0]*lnx + m[4]*lny + m[8]*lnz);
                        sny += gv.w[k] * (m[1]*lnx + m[5]*lny + m[9]*lnz);
                        snz += gv.w[k] * (m[2]*lnx + m[6]*lny + m[10]*lnz);
                    }
                    lx = sx; ly = sy; lz = sz;
                    lnx = snx; lny = sny; lnz = snz;
                } else {
                    float px = nodeM[0]*lx + nodeM[4]*ly + nodeM[8]*lz + nodeM[12];
                    float py = nodeM[1]*lx + nodeM[5]*ly + nodeM[9]*lz + nodeM[13];
                    float pz = nodeM[2]*lx + nodeM[6]*ly + nodeM[10]*lz + nodeM[14];
                    float pnx = nodeM[0]*lnx + nodeM[4]*lny + nodeM[8]*lnz;
                    float pny = nodeM[1]*lnx + nodeM[5]*lny + nodeM[9]*lnz;
                    float pnz = nodeM[2]*lnx + nodeM[6]*lny + nodeM[10]*lnz;
                    lx = px; ly = py; lz = pz;
                    lnx = pnx; lny = pny; lnz = pnz;
                }
                lx = (lx - normCenter[0]) * scaleDim;
                ly = (ly - normCenter[1]) * scaleDim;
                lz = (lz - normCenter[2]) * scaleDim;
                xPos[vi] = StudioScene::transformVert(hostNode, lx, ly, lz);
                Vec3f wn = StudioScene::transformVert(hostNode, lnx, lny, lnz) -
                           Vec3f{hostNode.posX, hostNode.posY, hostNode.posZ};
                xNrm[vi] = norm3(wn);
            }

            for (size_t ti = 0; ti + 2 < prim.indices.size(); ti += 3) {
                uint32_t i0 = prim.indices[ti], i1 = prim.indices[ti + 1], i2 = prim.indices[ti + 2];
                if (i0 >= xPos.size() || i1 >= xPos.size() || i2 >= xPos.size()) continue;
                auto emitV = [&](uint32_t idx) {
                    float lu = prim.verts[idx].u - std::floor(prim.verts[idx].u);
                    float lv = prim.verts[idx].v - std::floor(prim.verts[idx].v);
                    float mu = u0 + (u1 - u0) * lu;
                    float mv = v0 + (v1 - v0) * lv;
                    out.push_back({xPos[idx].x, xPos[idx].y, xPos[idx].z,
                                   xNrm[idx].x, xNrm[idx].y, xNrm[idx].z, col, mu, mv});
                };
                emitV(i0); emitV(i1); emitV(i2);
            }
        }
    }

    int addAnimationClip(const std::string &name) {
        GltfAnimation ga;
        ga.name = name.empty() ? ("Clip_" + std::to_string(animations.size())) : name;
        animations.push_back(std::move(ga));
        return (int)animations.size() - 1;
    }

    void sampleNodeTRS(int animIndex, int nodeIndex, float timeSec, float outT[3], float outR[4], float outS[3]) const {
        if (nodeIndex < 0 || nodeIndex >= (int)nodes.size()) return;
        std::memcpy(outT, nodes[(size_t)nodeIndex].t, 12);
        std::memcpy(outR, nodes[(size_t)nodeIndex].r, 16);
        std::memcpy(outS, nodes[(size_t)nodeIndex].s, 12);
        if (animIndex < 0 || animIndex >= (int)animations.size()) return;
        const auto &anim = animations[(size_t)animIndex];
        for (const auto &ch : anim.channels) {
            if (ch.targetNode != nodeIndex || ch.keys.empty()) continue;
            float v[4]{0, 0, 0, 1};
            sampleChannel(ch, timeSec, v);
            if (ch.path == 0) std::memcpy(outT, v, 12);
            else if (ch.path == 1) std::memcpy(outR, v, 16);
            else if (ch.path == 2) std::memcpy(outS, v, 12);
        }
    }

    void setNodeKeyframe(int animIndex, int nodeIndex, float t, const float tr[3], const float rq[4], const float sc[3]) {
        if (animIndex < 0 || animIndex >= (int)animations.size() || nodeIndex < 0 || nodeIndex >= (int)nodes.size()) return;
        GltfAnimation &anim = animations[(size_t)animIndex];
        auto upsert = [&](int path, const float val[4]) {
            GltfChannel *ch = nullptr;
            for (auto &c : anim.channels) {
                if (c.targetNode == nodeIndex && c.path == path) { ch = &c; break; }
            }
            if (!ch) {
                anim.channels.push_back({nodeIndex, path, 0, {}});
                ch = &anim.channels.back();
            }
            for (auto &k : ch->keys) {
                if (std::fabs(k.t - t) < 0.04f) {
                    std::memcpy(k.v, val, 16);
                    return;
                }
            }
            GltfKeyframe nk; nk.t = t; std::memcpy(nk.v, val, 16);
            ch->keys.push_back(nk);
            std::sort(ch->keys.begin(), ch->keys.end(), [](const GltfKeyframe &a, const GltfKeyframe &b) { return a.t < b.t; });
        };
        float t4[4]{tr[0], tr[1], tr[2], 0}, s4[4]{sc[0], sc[1], sc[2], 0};
        upsert(0, t4);
        upsert(1, rq);
        upsert(2, s4);
        anim.recomputeDuration();
    }

    void removeKeyframeNear(int animIndex, int nodeIndex, float t) {
        if (animIndex < 0 || animIndex >= (int)animations.size()) return;
        GltfAnimation &anim = animations[(size_t)animIndex];
        for (auto &ch : anim.channels) {
            if (ch.targetNode != nodeIndex) continue;
            ch.keys.erase(std::remove_if(ch.keys.begin(), ch.keys.end(),
                                         [&](const GltfKeyframe &k) { return std::fabs(k.t - t) < 0.15f; }),
                          ch.keys.end());
        }
        anim.recomputeDuration();
    }

    static void writeJsonVal(std::ostringstream &os, const JsonVal &v) {
        if (v.type == JsonVal::J_NULL) os << "null";
        else if (v.type == JsonVal::J_BOOL) os << (v.b ? "true" : "false");
        else if (v.type == JsonVal::J_NUM) os << v.num;
        else if (v.type == JsonVal::J_STR) os << MiniJson::esc(v.str);
        else if (v.type == JsonVal::J_ARR) {
            os << "[";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) os << ",";
                writeJsonVal(os, v.arr[i]);
            }
            os << "]";
        } else if (v.type == JsonVal::J_OBJ) {
            os << "{";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) os << ",";
                os << MiniJson::esc(v.obj[i].first) << ":";
                writeJsonVal(os, v.obj[i].second);
            }
            os << "}";
        }
    }

    static JsonVal *ensureField(JsonVal &obj, const char *key, JsonVal::Type tp) {
        for (auto &kv : obj.obj) {
            if (kv.first == key) return &kv.second;
        }
        JsonVal nv; nv.type = tp;
        obj.obj.emplace_back(key, std::move(nv));
        return &obj.obj.back().second;
    }

    bool saveAnimationsToFile() const {
        if (!loaded || fullPath.empty()) return false;
        std::vector<uint8_t> raw;
        if (!readAllBytes(fullPath, raw)) return false;

        std::string jsonStr;
        std::vector<uint8_t> bin;
        if (isGlb && raw.size() >= 12 && readU32LE(raw.data()) == 0x46546C67u) {
            size_t off = 12;
            while (off + 8 <= raw.size()) {
                uint32_t cLen = readU32LE(&raw[off]);
                uint32_t cType = readU32LE(&raw[off + 4]);
                off += 8;
                if (off + cLen > raw.size()) break;
                if (cType == 0x4E4F534Au) jsonStr.assign(reinterpret_cast<const char *>(&raw[off]), cLen);
                else if (cType == 0x004E4942u) bin.assign(raw.begin() + (ptrdiff_t)off, raw.begin() + (ptrdiff_t)(off + cLen));
                off += cLen;
            }
        } else {
            jsonStr.assign(raw.begin(), raw.end());
        }
        JsonVal root = MiniJson::parse(jsonStr);
        if (root.type != JsonVal::J_OBJ) return false;

        if (!isGlb) {
            std::string dir = ".";
            if (auto sl = fullPath.find_last_of('/'); sl != std::string::npos) dir = fullPath.substr(0, sl);
            if (const JsonVal *bufArr = root.get("buffers"); bufArr && bufArr->type == JsonVal::J_ARR && !bufArr->arr.empty()) {
                std::string uri = bufArr->arr[0].s("uri", "");
                if (uri.rfind("data:", 0) == 0) {
                    auto comma = uri.find(',');
                    if (comma != std::string::npos) bin = decodeBase64(uri.substr(comma + 1));
                } else if (!uri.empty()) {
                    readAllBytes(dir + "/" + uri, bin);
                }
            }
        }
        while (bin.size() % 4 != 0) bin.push_back(0);

        JsonVal *bvs = ensureField(root, "bufferViews", JsonVal::J_ARR);
        JsonVal *accs = ensureField(root, "accessors", JsonVal::J_ARR);
        JsonVal *anims = ensureField(root, "animations", JsonVal::J_ARR);
        anims->arr.clear();

        auto appendFloats = [&](const std::vector<float> &vals, const char *accType, bool minMax) -> int {
            while (bin.size() % 4 != 0) bin.push_back(0);
            size_t bvOff = bin.size();
            size_t byteLen = vals.size() * 4;
            bin.resize(bvOff + byteLen);
            if (!vals.empty()) std::memcpy(bin.data() + bvOff, vals.data(), byteLen);

            JsonVal bv; bv.type = JsonVal::J_OBJ;
            JsonVal f0; f0.type = JsonVal::J_NUM; f0.num = 0;
            JsonVal f1; f1.type = JsonVal::J_NUM; f1.num = (double)bvOff;
            JsonVal f2; f2.type = JsonVal::J_NUM; f2.num = (double)byteLen;
            bv.obj.emplace_back("buffer", f0);
            bv.obj.emplace_back("byteOffset", f1);
            bv.obj.emplace_back("byteLength", f2);
            int bvIdx = (int)bvs->arr.size();
            bvs->arr.push_back(std::move(bv));

            int comps = numCompsForType(accType);
            int cnt = (int)vals.size() / comps;
            JsonVal acc; acc.type = JsonVal::J_OBJ;
            JsonVal abv; abv.type = JsonVal::J_NUM; abv.num = bvIdx;
            JsonVal act; act.type = JsonVal::J_NUM; act.num = 5126;
            JsonVal acn; acn.type = JsonVal::J_NUM; acn.num = cnt;
            JsonVal atp; atp.type = JsonVal::J_STR; atp.str = accType;
            acc.obj.emplace_back("bufferView", abv);
            acc.obj.emplace_back("componentType", act);
            acc.obj.emplace_back("count", acn);
            acc.obj.emplace_back("type", atp);
            if (minMax && !vals.empty()) {
                JsonVal mn; mn.type = JsonVal::J_ARR;
                JsonVal mx; mx.type = JsonVal::J_ARR;
                float lo = *std::min_element(vals.begin(), vals.end());
                float hi = *std::max_element(vals.begin(), vals.end());
                JsonVal jlo; jlo.type = JsonVal::J_NUM; jlo.num = lo;
                JsonVal jhi; jhi.type = JsonVal::J_NUM; jhi.num = hi;
                mn.arr.push_back(jlo); mx.arr.push_back(jhi);
                acc.obj.emplace_back("min", std::move(mn));
                acc.obj.emplace_back("max", std::move(mx));
            }
            int accIdx = (int)accs->arr.size();
            accs->arr.push_back(std::move(acc));
            return accIdx;
        };

        for (const auto &ga : animations) {
            JsonVal aj; aj.type = JsonVal::J_OBJ;
            JsonVal nm; nm.type = JsonVal::J_STR; nm.str = ga.name;
            JsonVal chans; chans.type = JsonVal::J_ARR;
            JsonVal samps; samps.type = JsonVal::J_ARR;

            for (const auto &ch : ga.channels) {
                if (ch.keys.empty()) continue;
                std::vector<float> times, vals;
                int comps = (ch.path == 1) ? 4 : 3;
                for (const auto &kf : ch.keys) {
                    times.push_back(kf.t);
                    for (int c = 0; c < comps; ++c) vals.push_back(kf.v[c]);
                }
                int inAcc = appendFloats(times, "SCALAR", true);
                int outAcc = appendFloats(vals, (ch.path == 1) ? "VEC4" : "VEC3", false);

                JsonVal sj; sj.type = JsonVal::J_OBJ;
                JsonVal si; si.type = JsonVal::J_NUM; si.num = inAcc;
                JsonVal so; so.type = JsonVal::J_NUM; so.num = outAcc;
                JsonVal sp; sp.type = JsonVal::J_STR; sp.str = (ch.interp == 1) ? "STEP" : "LINEAR";
                sj.obj.emplace_back("input", si);
                sj.obj.emplace_back("output", so);
                sj.obj.emplace_back("interpolation", sp);
                int sIdx = (int)samps.arr.size();
                samps.arr.push_back(std::move(sj));

                JsonVal cj; cj.type = JsonVal::J_OBJ;
                JsonVal cs; cs.type = JsonVal::J_NUM; cs.num = sIdx;
                JsonVal ct; ct.type = JsonVal::J_OBJ;
                JsonVal tn; tn.type = JsonVal::J_NUM; tn.num = ch.targetNode;
                JsonVal tp; tp.type = JsonVal::J_STR;
                tp.str = (ch.path == 0) ? "translation" : (ch.path == 1 ? "rotation" : "scale");
                ct.obj.emplace_back("node", tn);
                ct.obj.emplace_back("path", tp);
                cj.obj.emplace_back("sampler", cs);
                cj.obj.emplace_back("target", std::move(ct));
                chans.arr.push_back(std::move(cj));
            }
            aj.obj.emplace_back("name", nm);
            aj.obj.emplace_back("channels", std::move(chans));
            aj.obj.emplace_back("samplers", std::move(samps));
            anims->arr.push_back(std::move(aj));
        }

        JsonVal *bufs = ensureField(root, "buffers", JsonVal::J_ARR);
        if (bufs->arr.empty()) {
            JsonVal b0; b0.type = JsonVal::J_OBJ;
            bufs->arr.push_back(std::move(b0));
        }
        JsonVal *blen = ensureField(bufs->arr[0], "byteLength", JsonVal::J_NUM);
        blen->type = JsonVal::J_NUM;
        blen->num = (double)bin.size();
        if (!isGlb) {
            JsonVal *buri = ensureField(bufs->arr[0], "uri", JsonVal::J_STR);
            buri->type = JsonVal::J_STR;
            buri->str = "data:application/octet-stream;base64," + encodeBase64(bin.data(), bin.size());
        }

        std::ostringstream jos;
        writeJsonVal(jos, root);
        std::string newJson = jos.str();

        if (isGlb) {
            while (newJson.size() % 4 != 0) newJson.push_back(' ');
            while (bin.size() % 4 != 0) bin.push_back(0);
            uint32_t totalLen = 12 + 8 + (uint32_t)newJson.size() + 8 + (uint32_t)bin.size();
            std::vector<uint8_t> outBytes(totalLen, 0);
            writeU32LE(&outBytes[0], 0x46546C67u);
            writeU32LE(&outBytes[4], 2);
            writeU32LE(&outBytes[8], totalLen);
            size_t p = 12;
            writeU32LE(&outBytes[p], (uint32_t)newJson.size());
            writeU32LE(&outBytes[p + 4], 0x4E4F534Au);
            std::memcpy(&outBytes[p + 8], newJson.data(), newJson.size());
            p += 8 + newJson.size();
            writeU32LE(&outBytes[p], (uint32_t)bin.size());
            writeU32LE(&outBytes[p + 4], 0x004E4942u);
            if (!bin.empty()) std::memcpy(&outBytes[p + 8], bin.data(), bin.size());
            std::ofstream fout(fullPath, std::ios::binary | std::ios::trunc);
            return (bool)(fout && fout.write(reinterpret_cast<const char *>(outBytes.data()), (std::streamsize)outBytes.size()));
        } else {
            std::ofstream fout(fullPath, std::ios::binary | std::ios::trunc);
            return (bool)(fout && fout.write(newJson.data(), (std::streamsize)newJson.size()));
        }
    }
};
