#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

inline Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3f operator*(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot3(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross3(Vec3f a, Vec3f b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3f norm3(Vec3f a) {
    float l = std::sqrt(dot3(a, a));
    return l > 1e-6f ? Vec3f{a.x / l, a.y / l, a.z / l} : Vec3f{0, 1, 0};
}

struct MeshVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t color;
};

struct SceneNode {
    std::string id;
    std::string name;
    std::string className;
    bool expanded = true;

    float posX = 0, posY = 0, posZ = 0;
    float sizeX = 4, sizeY = 2, sizeZ = 4;
    float rotX = 0, rotY = 0, rotZ = 0;
    uint32_t argbColor = 0xFFA3A2A5;
    std::string material = "Plastic";
    float transparency = 0.0f;
    bool anchored = true;
    bool canCollide = true;
    bool castShadow = true;
    bool locked = false;
    bool disabled = false;

    std::string scriptPath;
    std::string text = "FPS Arena";
    int textSize = 18;
    std::string fontName = "bold";
    float clockTime = 14.0f;
    float brightness = 2.0f;

    std::vector<SceneNode> children;

    bool is3DPart() const {
        return className == "Part" || className == "WedgePart" ||
               className == "Sphere" || className == "Cylinder" ||
               className == "SpawnLocation";
    }
    bool isScript() const { return className == "Script" || className == "LocalScript"; }
    bool isUi() const { return className == "TextLabel" || className == "Frame"; }
    std::string shape() const {
        if (className == "WedgePart") return "Wedge";
        if (className == "Sphere") return "Sphere";
        if (className == "Cylinder") return "Cylinder";
        return "Block";
    }
    void setShape(const std::string &s) {
        if (s == "Wedge") className = "WedgePart";
        else if (s == "Sphere") className = "Sphere";
        else if (s == "Cylinder") className = "Cylinder";
        else className = "Part";
    }
};

// Minimal JSON parser for scene.json and luac frame output
struct JsonVal {
    enum Type { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } type = J_NULL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonVal> arr;
    std::vector<std::pair<std::string, JsonVal>> obj;

    const JsonVal *get(const char *key) const {
        if (type != J_OBJ) return nullptr;
        for (const auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string s(const char *key, const char *def = "") const {
        const JsonVal *v = get(key);
        return (v && v->type == J_STR) ? v->str : std::string(def);
    }
    double d(const char *key, double def = 0.0) const {
        const JsonVal *v = get(key);
        return (v && v->type == J_NUM) ? v->num : def;
    }
    bool bl(const char *key, bool def = false) const {
        const JsonVal *v = get(key);
        return (v && v->type == J_BOOL) ? v->b : def;
    }
};

namespace MiniJson {
inline void skip(const char *&p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}
inline std::string parseStr(const char *&p, const char *end) {
    std::string out;
    if (p >= end || *p != '"') return out;
    ++p;
    while (p < end) {
        char c = *p++;
        if (c == '"') break;
        if (c == '\\' && p < end) {
            char e = *p++;
            if (e == '"' || e == '\\' || e == '/') out.push_back(e);
            else if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else if (e == 'b') out.push_back('\b');
            else if (e == 'f') out.push_back('\f');
            else if (e == 'u' && p + 4 <= end) {
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = p[i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (10 + h - 'a');
                    else if (h >= 'A' && h <= 'F') cp |= (10 + h - 'A');
                }
                p += 4;
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}
inline JsonVal parseVal(const char *&p, const char *end, int depth = 0) {
    JsonVal v;
    if (depth > 64) return v;
    skip(p, end);
    if (p >= end) return v;
    char c = *p;
    if (c == '"') {
        v.type = JsonVal::J_STR;
        v.str = parseStr(p, end);
        return v;
    }
    if (c == '{') {
        v.type = JsonVal::J_OBJ;
        ++p; skip(p, end);
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end) {
            skip(p, end);
            std::string k = parseStr(p, end);
            skip(p, end);
            if (p < end && *p == ':') ++p;
            JsonVal val = parseVal(p, end, depth + 1);
            v.obj.emplace_back(std::move(k), std::move(val));
            skip(p, end);
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; break; }
            break;
        }
        return v;
    }
    if (c == '[') {
        v.type = JsonVal::J_ARR;
        ++p; skip(p, end);
        if (p < end && *p == ']') { ++p; return v; }
        while (p < end) {
            v.arr.push_back(parseVal(p, end, depth + 1));
            skip(p, end);
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; break; }
            break;
        }
        return v;
    }
    if (c == 't' && p + 4 <= end && !std::memcmp(p, "true", 4)) { v.type = JsonVal::J_BOOL; v.b = true; p += 4; return v; }
    if (c == 'f' && p + 5 <= end && !std::memcmp(p, "false", 5)) { v.type = JsonVal::J_BOOL; v.b = false; p += 5; return v; }
    if (c == 'n' && p + 4 <= end && !std::memcmp(p, "null", 4)) { v.type = JsonVal::J_NULL; p += 4; return v; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *ep = nullptr;
        v.num = std::strtod(p, &ep);
        if (ep > p) { v.type = JsonVal::J_NUM; p = ep; }
        return v;
    }
    return v;
}
inline JsonVal parse(const std::string &src) {
    const char *p = src.data();
    return parseVal(p, p + src.size(), 0);
}
inline std::string esc(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 32) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
        else o.push_back((char)c);
    }
    o.push_back('"');
    return o;
}
} // namespace MiniJson

class StudioScene {
public:
    SceneNode root;
    int nextId = 1;

    StudioScene() {
        root.id = "n0";
        root.name = "DataModel";
        root.className = "DataModel";
    }

    std::string genId() { return "n" + std::to_string(nextId++); }

    static SceneNode makeNode(const std::string &id, const std::string &name, const std::string &cls) {
        SceneNode n;
        n.id = id; n.name = name; n.className = cls;
        return n;
    }

    static StudioScene createFpsArenaDefaults() {
        StudioScene m;
        SceneNode ws = makeNode(m.genId(), "Workspace", "Workspace");

        auto base = makeNode(m.genId(), "Baseplate", "Part");
        base.posY = -1.0f; base.sizeX = 120; base.sizeY = 2; base.sizeZ = 120;
        base.argbColor = 0xFF848992; base.material = "SmoothPlastic";
        ws.children.push_back(base);

        auto spawn = makeNode(m.genId(), "SpawnLocation", "SpawnLocation");
        spawn.posX = 0; spawn.posY = 0.25f; spawn.posZ = 10;
        spawn.sizeX = 6; spawn.sizeY = 0.5f; spawn.sizeZ = 6;
        spawn.argbColor = 0xFFC6CBD4;
        ws.children.push_back(spawn);

        auto deck = makeNode(m.genId(), "UpperDeck", "Part");
        deck.posX = 0; deck.posY = 3.0f; deck.posZ = -16;
        deck.sizeX = 34; deck.sizeY = 1.5f; deck.sizeZ = 16;
        deck.argbColor = 0xFFA1A8B4; deck.material = "SmoothPlastic";
        ws.children.push_back(deck);

        auto rampL = makeNode(m.genId(), "RampLeft", "WedgePart");
        rampL.posX = -19; rampL.posY = 2.0f; rampL.posZ = -16;
        rampL.sizeX = 10; rampL.sizeY = 4; rampL.sizeZ = 10; rampL.rotY = 90;
        rampL.argbColor = 0xFFA2ABB8; rampL.material = "SmoothPlastic";
        ws.children.push_back(rampL);

        auto rampR = makeNode(m.genId(), "RampRight", "WedgePart");
        rampR.posX = 19; rampR.posY = 2.0f; rampR.posZ = -16;
        rampR.sizeX = 10; rampR.sizeY = 4; rampR.sizeZ = 10; rampR.rotY = -90;
        rampR.argbColor = 0xFFA2ABB8; rampR.material = "SmoothPlastic";
        ws.children.push_back(rampR);

        auto back = makeNode(m.genId(), "BackWall", "Part");
        back.posX = 0; back.posY = 5.5f; back.posZ = -24;
        back.sizeX = 58; back.sizeY = 11; back.sizeZ = 2;
        back.argbColor = 0xFF949CA8;
        ws.children.push_back(back);

        auto leftW = makeNode(m.genId(), "LeftWall", "Part");
        leftW.posX = -28; leftW.posY = 5.5f; leftW.posZ = -5;
        leftW.sizeX = 2; leftW.sizeY = 11; leftW.sizeZ = 38;
        leftW.argbColor = 0xFF838C99; leftW.material = "Slate";
        ws.children.push_back(leftW);

        auto rightW = makeNode(m.genId(), "RightWall", "Part");
        rightW.posX = 28; rightW.posY = 5.5f; rightW.posZ = -5;
        rightW.sizeX = 2; rightW.sizeY = 11; rightW.sizeZ = 38;
        rightW.argbColor = 0xFFA0A8B4; rightW.material = "Slate";
        ws.children.push_back(rightW);

        auto c1 = makeNode(m.genId(), "CoverBoxA", "Part");
        c1.posX = -10; c1.posY = 1.5f; c1.posZ = -7; c1.sizeX = 4; c1.sizeY = 3; c1.sizeZ = 4;
        c1.argbColor = 0xFFB8635A;
        ws.children.push_back(c1);

        auto c2 = makeNode(m.genId(), "CoverBoxB", "Part");
        c2.posX = -9.5f; c2.posY = 4.0f; c2.posZ = -7; c2.sizeX = 2.6f; c2.sizeY = 2; c2.sizeZ = 2.6f;
        c2.argbColor = 0xFFAE5A52;
        ws.children.push_back(c2);

        auto c3 = makeNode(m.genId(), "CoverBoxC", "Part");
        c3.posX = 11; c3.posY = 2.5f; c3.posZ = -18; c3.sizeX = 3.5f; c3.sizeY = 5; c3.sizeZ = 3.5f;
        c3.argbColor = 0xFFB8635A;
        ws.children.push_back(c3);

        float tx[4] = {-12, -3, 4, 9};
        float ty[4] = {5.5f, 2.2f, 2.2f, 5.5f};
        float tz[4] = {-15, -10, -10, -15};
        for (int i = 0; i < 4; ++i) {
            auto t = makeNode(m.genId(), "Target" + std::to_string(i + 1), "Part");
            t.posX = tx[i]; t.posY = ty[i]; t.posZ = tz[i];
            t.sizeX = 1.6f; t.sizeY = 3.4f; t.sizeZ = 1.0f;
            t.argbColor = 0xFFDC4C46; t.material = "SmoothPlastic";
            ws.children.push_back(t);
        }

        auto ms = makeNode(m.genId(), "ArenaController", "Script");
        ms.scriptPath = "main.lua";
        ws.children.push_back(ms);
        m.root.children.push_back(ws);

        auto players = makeNode(m.genId(), "Players", "Players"); players.expanded = false;
        m.root.children.push_back(players);

        auto lighting = makeNode(m.genId(), "Lighting", "Lighting"); lighting.expanded = false;
        m.root.children.push_back(lighting);

        for (const char *s : {"MaterialService", "ReplicatedFirst", "ReplicatedStorage"}) {
            auto svc = makeNode(m.genId(), s, s); svc.expanded = false;
            m.root.children.push_back(svc);
        }

        auto sss = makeNode(m.genId(), "ServerScriptService", "ServerScriptService"); sss.expanded = false;
        auto gs = makeNode(m.genId(), "GameScript", "Script"); gs.scriptPath = "scripts/game.lua";
        sss.children.push_back(gs);
        m.root.children.push_back(sss);

        auto ss = makeNode(m.genId(), "ServerStorage", "ServerStorage"); ss.expanded = false;
        m.root.children.push_back(ss);

        auto sg = makeNode(m.genId(), "StarterGui", "StarterGui"); sg.expanded = false;
        auto gui = makeNode(m.genId(), "ArenaHud", "ScreenGui");
        auto lbl = makeNode(m.genId(), "TitleLabel", "TextLabel");
        lbl.text = "FPS Arena"; lbl.textSize = 20; lbl.fontName = "bold"; lbl.argbColor = 0xFFFFFFFF;
        lbl.posX = 16; lbl.posY = 12;
        gui.children.push_back(lbl);
        sg.children.push_back(gui);
        m.root.children.push_back(sg);

        for (const char *s : {"StarterPack", "StarterPlayer", "Teams", "SoundService"}) {
            auto svc = makeNode(m.genId(), s, s); svc.expanded = false;
            m.root.children.push_back(svc);
        }
        return m;
    }

    SceneNode *findService(const std::string &cls) {
        for (auto &c : root.children) if (c.className == cls || c.name == cls) return &c;
        return nullptr;
    }

    SceneNode *findById(const std::string &id) {
        return findByIdRec(root, id);
    }

    SceneNode *findParentOf(const std::string &id) {
        return findParentRec(root, id);
    }

    void collect3DParts(std::vector<const SceneNode *> &out) const {
        collect3DRec(root, out);
    }

    void collectUi(std::vector<const SceneNode *> &out) const {
        collectUiRec(root, out);
    }

    const SceneNode *spawnLocation() const {
        return findClassRec(root, "SpawnLocation");
    }

    const SceneNode *lighting() const {
        for (const auto &c : root.children) if (c.className == "Lighting") return &c;
        return nullptr;
    }

    SceneNode *createPart(SceneNode *parent, const std::string &cls, const std::string &baseName) {
        if (!parent) parent = findService("Workspace");
        if (!parent) return nullptr;
        SceneNode n = makeNode(genId(), baseName, cls);
        n.posX = 0; n.posY = 3.0f; n.posZ = -2.0f;
        n.sizeX = 4; n.sizeY = 2; n.sizeZ = 4;
        if (cls == "Sphere") { n.sizeX = n.sizeY = n.sizeZ = 3; n.argbColor = 0xFF4ADE80; }
        else if (cls == "Cylinder") { n.sizeX = n.sizeZ = 3; n.sizeY = 4; n.argbColor = 0xFF38BDF8; }
        else if (cls == "WedgePart") { n.sizeX = 4; n.sizeY = 3; n.sizeZ = 5; n.argbColor = 0xFF94A3B8; }
        else if (cls == "SpawnLocation") { n.sizeX = 6; n.sizeY = 0.5f; n.sizeZ = 6; n.posY = 0.25f; n.argbColor = 0xFFC6CBD4; }
        else { n.argbColor = 0xFFA3A2A5; }
        parent->expanded = true;
        parent->children.push_back(n);
        return &parent->children.back();
    }

    bool removeById(const std::string &id) {
        if (id == root.id) return false;
        for (const auto &svc : root.children) if (svc.id == id) return false;
        return removeRec(root, id);
    }

    std::string toJson() const {
        std::ostringstream os;
        os << "{\"nextId\":" << nextId << ",\"root\":";
        writeNodeJson(os, root);
        os << "}";
        return os.str();
    }

    bool saveToFile(const std::string &path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        return f.good();
    }

    static StudioScene loadOrCreate(const std::string &projectDir) {
        std::string path = projectDir + "/scene.json";
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::ostringstream ss; ss << f.rdbuf();
            JsonVal j = MiniJson::parse(ss.str());
            if (j.type == JsonVal::J_OBJ && j.get("root")) {
                StudioScene sc;
                sc.nextId = (int)j.d("nextId", 100);
                sc.root = nodeFromJson(*j.get("root"));
                if (!sc.root.children.empty()) return sc;
            }
        }
        StudioScene sc = createFpsArenaDefaults();
        sc.saveToFile(path);
        return sc;
    }

    static Vec3f transformVert(const SceneNode &part, float lx, float ly, float lz) {
        constexpr float kDeg = 3.14159265f / 180.0f;
        float rx = part.rotX * kDeg, ry = part.rotY * kDeg, rz = part.rotZ * kDeg;
        if (rx != 0.0f) {
            float c = std::cos(rx), s = std::sin(rx);
            float ny = ly * c - lz * s, nz = ly * s + lz * c;
            ly = ny; lz = nz;
        }
        if (ry != 0.0f) {
            float c = std::cos(ry), s = std::sin(ry);
            float nx = lx * c + lz * s, nz = -lx * s + lz * c;
            lx = nx; lz = nz;
        }
        if (rz != 0.0f) {
            float c = std::cos(rz), s = std::sin(rz);
            float nx = lx * c - ly * s, ny = lx * s + ly * c;
            lx = nx; ly = ny;
        }
        return {part.posX + lx, part.posY + ly, part.posZ + lz};
    }

    // Verified CCW outward-facing mesh generation (fixes see-through faces!)
    static void appendPartMesh(const SceneNode &part, std::vector<MeshVertex> &out) {
        float hx = part.sizeX * 0.5f, hy = part.sizeY * 0.5f, hz = part.sizeZ * 0.5f;
        uint8_t r = (part.argbColor >> 16) & 0xFF;
        uint8_t g = (part.argbColor >> 8) & 0xFF;
        uint8_t b = part.argbColor & 0xFF;
        uint8_t a = (uint8_t)std::max(25, std::min(255, (int)std::lround((1.0f - part.transparency) * 255.0f)));
        uint32_t col = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
        Vec3f center{part.posX, part.posY, part.posZ};

        auto emitTri = [&](Vec3f p0, Vec3f p1, Vec3f p2) {
            Vec3f n = cross3(p1 - p0, p2 - p0);
            Vec3f triCenter = (p0 + p1 + p2) * (1.0f / 3.0f);
            // Enforce outward winding!
            if (dot3(n, triCenter - center) < 0.0f) {
                std::swap(p1, p2);
                n = cross3(p1 - p0, p2 - p0);
            }
            n = norm3(n);
            out.push_back({p0.x, p0.y, p0.z, n.x, n.y, n.z, col});
            out.push_back({p1.x, p1.y, p1.z, n.x, n.y, n.z, col});
            out.push_back({p2.x, p2.y, p2.z, n.x, n.y, n.z, col});
        };

        auto emitQuad = [&](Vec3f a0, Vec3f a1, Vec3f a2, Vec3f a3) {
            emitTri(a0, a1, a2);
            emitTri(a0, a2, a3);
        };

        if (part.className == "WedgePart") {
            Vec3f v[6] = {
                transformVert(part, -hx, -hy, -hz), transformVert(part,  hx, -hy, -hz),
                transformVert(part,  hx, -hy,  hz), transformVert(part, -hx, -hy,  hz),
                transformVert(part, -hx,  hy,  hz), transformVert(part,  hx,  hy,  hz)
            };
            emitQuad(v[0], v[1], v[2], v[3]); // bottom
            emitQuad(v[3], v[2], v[5], v[4]); // tall back
            emitQuad(v[0], v[4], v[5], v[1]); // slope
            emitTri(v[0], v[3], v[4]);        // left
            emitTri(v[1], v[5], v[2]);        // right
            return;
        }

        if (part.className == "Sphere") {
            const int stacks = 8, slices = 14;
            constexpr float kPi = 3.14159265f;
            auto pt = [&](float phi, float theta) {
                return transformVert(part,
                    std::cos(phi) * std::cos(theta) * hx,
                    std::sin(phi) * hy,
                    std::cos(phi) * std::sin(theta) * hz);
            };
            for (int i = 0; i < stacks; ++i) {
                float p0 = kPi * (-0.5f + (float)i / stacks);
                float p1 = kPi * (-0.5f + (float)(i + 1) / stacks);
                for (int j = 0; j < slices; ++j) {
                    float t0 = 2.0f * kPi * j / slices;
                    float t1 = 2.0f * kPi * (j + 1) / slices;
                    emitQuad(pt(p0, t0), pt(p0, t1), pt(p1, t1), pt(p1, t0));
                }
            }
            return;
        }

        if (part.className == "Cylinder") {
            const int segs = 16;
            constexpr float kTwoPi = 6.28318530f;
            Vec3f topC = transformVert(part, 0, hy, 0);
            Vec3f botC = transformVert(part, 0, -hy, 0);
            for (int i = 0; i < segs; ++i) {
                float a0 = kTwoPi * i / segs, a1 = kTwoPi * (i + 1) / segs;
                Vec3f b0 = transformVert(part, std::cos(a0) * hx, -hy, std::sin(a0) * hz);
                Vec3f b1 = transformVert(part, std::cos(a1) * hx, -hy, std::sin(a1) * hz);
                Vec3f t0 = transformVert(part, std::cos(a0) * hx,  hy, std::sin(a0) * hz);
                Vec3f t1 = transformVert(part, std::cos(a1) * hx,  hy, std::sin(a1) * hz);
                emitQuad(b0, b1, t1, t0);
                emitTri(topC, t1, t0);
                emitTri(botC, b0, b1);
            }
            return;
        }

        // Standard Box (Part / SpawnLocation)
        Vec3f v[8] = {
            transformVert(part, -hx, -hy, -hz), transformVert(part,  hx, -hy, -hz),
            transformVert(part,  hx,  hy, -hz), transformVert(part, -hx,  hy, -hz),
            transformVert(part, -hx, -hy,  hz), transformVert(part,  hx, -hy,  hz),
            transformVert(part,  hx,  hy,  hz), transformVert(part, -hx,  hy,  hz)
        };
        emitQuad(v[7], v[6], v[2], v[3]); // +Y top
        emitQuad(v[0], v[1], v[5], v[4]); // -Y bottom
        emitQuad(v[4], v[5], v[6], v[7]); // +Z front
        emitQuad(v[1], v[0], v[3], v[2]); // -Z back
        emitQuad(v[5], v[1], v[2], v[6]); // +X right
        emitQuad(v[0], v[4], v[7], v[3]); // -X left
    }

private:
    static SceneNode *findByIdRec(SceneNode &n, const std::string &id) {
        if (n.id == id) return &n;
        for (auto &c : n.children) if (auto *r = findByIdRec(c, id)) return r;
        return nullptr;
    }
    static SceneNode *findParentRec(SceneNode &n, const std::string &id) {
        for (auto &c : n.children) {
            if (c.id == id) return &n;
            if (auto *r = findParentRec(c, id)) return r;
        }
        return nullptr;
    }
    static void collect3DRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.is3DPart()) out.push_back(&n);
        for (const auto &c : n.children) collect3DRec(c, out);
    }
    static void collectUiRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.isUi()) out.push_back(&n);
        for (const auto &c : n.children) collectUiRec(c, out);
    }
    static const SceneNode *findClassRec(const SceneNode &n, const std::string &cls) {
        if (n.className == cls) return &n;
        for (const auto &c : n.children) if (auto *r = findClassRec(c, cls)) return r;
        return nullptr;
    }
    static bool removeRec(SceneNode &n, const std::string &id) {
        for (auto it = n.children.begin(); it != n.children.end(); ++it) {
            if (it->id == id) { n.children.erase(it); return true; }
            if (removeRec(*it, id)) return true;
        }
        return false;
    }
    static void writeNodeJson(std::ostringstream &os, const SceneNode &n) {
        os << "{\"id\":" << MiniJson::esc(n.id)
           << ",\"name\":" << MiniJson::esc(n.name)
           << ",\"className\":" << MiniJson::esc(n.className)
           << ",\"expanded\":" << (n.expanded ? "true" : "false")
           << ",\"posX\":" << n.posX << ",\"posY\":" << n.posY << ",\"posZ\":" << n.posZ
           << ",\"sizeX\":" << n.sizeX << ",\"sizeY\":" << n.sizeY << ",\"sizeZ\":" << n.sizeZ
           << ",\"rotX\":" << n.rotX << ",\"rotY\":" << n.rotY << ",\"rotZ\":" << n.rotZ
           << ",\"color\":" << (int32_t)n.argbColor
           << ",\"material\":" << MiniJson::esc(n.material)
           << ",\"transparency\":" << n.transparency
           << ",\"anchored\":" << (n.anchored ? "true" : "false")
           << ",\"canCollide\":" << (n.canCollide ? "true" : "false")
           << ",\"castShadow\":" << (n.castShadow ? "true" : "false")
           << ",\"locked\":" << (n.locked ? "true" : "false")
           << ",\"disabled\":" << (n.disabled ? "true" : "false")
           << ",\"scriptPath\":" << MiniJson::esc(n.scriptPath)
           << ",\"text\":" << MiniJson::esc(n.text)
           << ",\"textSize\":" << n.textSize
           << ",\"fontName\":" << MiniJson::esc(n.fontName)
           << ",\"clockTime\":" << n.clockTime
           << ",\"brightness\":" << n.brightness
           << ",\"children\":[";
        for (size_t i = 0; i < n.children.size(); ++i) {
            if (i) os << ",";
            writeNodeJson(os, n.children[i]);
        }
        os << "]}";
    }
    static SceneNode nodeFromJson(const JsonVal &j) {
        SceneNode n;
        n.id = j.s("id", "n");
        n.name = j.s("name", "Part");
        n.className = j.s("className", "Part");
        n.expanded = j.bl("expanded", false);
        n.posX = (float)j.d("posX", 0); n.posY = (float)j.d("posY", 0); n.posZ = (float)j.d("posZ", 0);
        n.sizeX = (float)j.d("sizeX", 4); n.sizeY = (float)j.d("sizeY", 2); n.sizeZ = (float)j.d("sizeZ", 4);
        n.rotX = (float)j.d("rotX", 0); n.rotY = (float)j.d("rotY", 0); n.rotZ = (float)j.d("rotZ", 0);
        n.argbColor = (uint32_t)(int64_t)j.d("color", (double)(int32_t)0xFFA3A2A5);
        n.material = j.s("material", "Plastic");
        n.transparency = (float)j.d("transparency", 0);
        n.anchored = j.bl("anchored", true);
        n.canCollide = j.bl("canCollide", true);
        n.castShadow = j.bl("castShadow", true);
        n.locked = j.bl("locked", false);
        n.disabled = j.bl("disabled", false);
        n.scriptPath = j.s("scriptPath", "");
        n.text = j.s("text", "FPS Arena");
        n.textSize = (int)j.d("textSize", 18);
        n.fontName = j.s("fontName", "bold");
        n.clockTime = (float)j.d("clockTime", 14.0);
        n.brightness = (float)j.d("brightness", 2.0);
        if (const JsonVal *kids = j.get("children")) {
            if (kids->type == JsonVal::J_ARR) {
                for (const auto &kc : kids->arr) n.children.push_back(nodeFromJson(kc));
            }
        }
        return n;
    }
};
