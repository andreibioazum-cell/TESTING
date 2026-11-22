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
    float u, v;
};

struct SceneNode {
    std::string id;
    std::string name;
    std::string className;
    bool expanded = true;

    float posX = 0, posY = 0, posZ = 0;
    float sizeX = 2, sizeY = 2, sizeZ = 2;
    float rotX = 0, rotY = 0, rotZ = 0;
    uint32_t argbColor = 0xFFA3A2A5;
    std::string material = "Plastic";
    float transparency = 0.0f;

    // Physics / collision / node-specific attributes
    bool staticBody = true;
    bool collidable = true;
    float mass = 1.0f;
    float gravityScale = 1.0f;
    float fov = 60.0f;
    float energy = 1.2f;
    float range = 16.0f;
    bool visible = true;
    bool castShadow = true;
    bool locked = false;
    bool disabled = false;

    std::string scriptPath;
    std::string animPath;
    bool animAutoPlay = false;
    bool animLoop = true;
    std::string text = "3D Scene";
    int textSize = 18;
    float clockTime = 14.0f;
    std::string skyPreset = "Day";
    uint32_t skyTopColor = 0xFF1E6EC4;
    uint32_t skyBotColor = 0xFF8CC4F2;
    std::string texturePath;
    std::string modelPath;
    int modelAnimIndex = 0;
    int segments = 24;
    float cornerRadius = 0.0f;

    std::vector<SceneNode> children;

    bool isMeshNode() const {
        return className == "Box" || className == "Wedge" ||
               className == "Sphere" || className == "Cylinder" ||
               className == "Plane" || className == "GltfModel" ||
               className == "Part" || className == "WedgePart";
    }
    bool isCollisionNode() const {
        return className == "CollisionBox" || className == "CollisionSphere" ||
               className == "StaticBody3D" || className == "RigidBody3D" ||
               className == "Area3D";
    }
    bool is3DNode() const {
        return isMeshNode() || isCollisionNode() ||
               className == "Node3D" || className == "Camera3D" ||
               className == "DirectionalLight3D" || className == "PointLight3D";
    }
    bool isScript() const { return className == "Script" || className == "LocalScript"; }
    bool isUi() const { return className == "TextLabel" || className == "Frame"; }
    bool isGroup() const { return className == "Folder" || className == "Model"; }

    std::string shape() const {
        if (className == "Wedge" || className == "WedgePart") return "Wedge";
        if (className == "Sphere" || className == "CollisionSphere") return "Sphere";
        if (className == "Cylinder") return "Cylinder";
        if (className == "Plane") return "Plane";
        if (className == "GltfModel") return "GltfModel";
        return "Box";
    }
    void setShape(const std::string &s) {
        if (s == "Wedge") className = "Wedge";
        else if (s == "Sphere") className = "Sphere";
        else if (s == "Cylinder") className = "Cylinder";
        else if (s == "Plane") className = "Plane";
        else className = "Box";
    }
};

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
    if (c == '"') { v.type = JsonVal::J_STR; v.str = parseStr(p, end); return v; }
    if (c == '{') {
        v.type = JsonVal::J_OBJ;
        ++p; skip(p, end);
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end) {
            skip(p, end);
            std::string k = parseStr(p, end);
            skip(p, end);
            if (p < end && *p == ':') ++p;
            v.obj.emplace_back(std::move(k), parseVal(p, end, depth + 1));
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

struct AnimKeyframe {
    float t = 0.0f;
    float px = 0, py = 0, pz = 0;
    float rx = 0, ry = 0, rz = 0;
    float sx = 2, sy = 2, sz = 2;
};

struct AnimClip {
    std::string name = "anim";
    bool loop = true;
    std::vector<AnimKeyframe> frames;

    float duration() const {
        if (frames.empty()) return 0.0f;
        return std::max(0.05f, frames.back().t);
    }

    void sortFrames() {
        std::sort(frames.begin(), frames.end(), [](const AnimKeyframe &a, const AnimKeyframe &b) {
            return a.t < b.t;
        });
    }

    void sample(float timeSec, SceneNode &dst) const {
        if (frames.empty()) return;
        float dur = duration();
        float t = loop ? std::fmod(std::max(0.0f, timeSec), dur) : std::clamp(timeSec, 0.0f, dur);
        if (frames.size() == 1 || t <= frames.front().t) {
            const auto &f = frames.front();
            dst.posX = f.px; dst.posY = f.py; dst.posZ = f.pz;
            dst.rotX = f.rx; dst.rotY = f.ry; dst.rotZ = f.rz;
            dst.sizeX = std::max(0.1f, f.sx); dst.sizeY = std::max(0.1f, f.sy); dst.sizeZ = std::max(0.1f, f.sz);
            return;
        }
        if (t >= frames.back().t) {
            const auto &f = frames.back();
            dst.posX = f.px; dst.posY = f.py; dst.posZ = f.pz;
            dst.rotX = f.rx; dst.rotY = f.ry; dst.rotZ = f.rz;
            dst.sizeX = std::max(0.1f, f.sx); dst.sizeY = std::max(0.1f, f.sy); dst.sizeZ = std::max(0.1f, f.sz);
            return;
        }
        for (size_t i = 0; i + 1 < frames.size(); ++i) {
            const auto &a = frames[i];
            const auto &b = frames[i + 1];
            if (t >= a.t && t <= b.t) {
                float span = std::max(1e-4f, b.t - a.t);
                float u = (t - a.t) / span;
                dst.posX = a.px + (b.px - a.px) * u;
                dst.posY = a.py + (b.py - a.py) * u;
                dst.posZ = a.pz + (b.pz - a.pz) * u;
                dst.rotX = a.rx + (b.rx - a.rx) * u;
                dst.rotY = a.ry + (b.ry - a.ry) * u;
                dst.rotZ = a.rz + (b.rz - a.rz) * u;
                dst.sizeX = std::max(0.1f, a.sx + (b.sx - a.sx) * u);
                dst.sizeY = std::max(0.1f, a.sy + (b.sy - a.sy) * u);
                dst.sizeZ = std::max(0.1f, a.sz + (b.sz - a.sz) * u);
                return;
            }
        }
    }

    std::string toJson() const {
        std::ostringstream os;
        os << "{\"name\":" << MiniJson::esc(name)
           << ",\"loop\":" << (loop ? "true" : "false")
           << ",\"duration\":" << duration()
           << ",\"keyframes\":[";
        for (size_t i = 0; i < frames.size(); ++i) {
            const auto &f = frames[i];
            if (i) os << ",";
            os << "{\"t\":" << f.t
               << ",\"px\":" << f.px << ",\"py\":" << f.py << ",\"pz\":" << f.pz
               << ",\"rx\":" << f.rx << ",\"ry\":" << f.ry << ",\"rz\":" << f.rz
               << ",\"sx\":" << f.sx << ",\"sy\":" << f.sy << ",\"sz\":" << f.sz << "}";
        }
        os << "]}";
        return os.str();
    }

    bool saveToFile(const std::string &path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson();
        return f.good();
    }

    static bool loadFromFile(const std::string &path, AnimClip &out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf();
        JsonVal j = MiniJson::parse(ss.str());
        if (j.type != JsonVal::J_OBJ) return false;
        out = AnimClip{};
        out.name = j.s("name", "anim");
        out.loop = j.bl("loop", true);
        if (const JsonVal *kf = j.get("keyframes"); kf && kf->type == JsonVal::J_ARR) {
            for (const auto &it : kf->arr) {
                AnimKeyframe k;
                k.t = (float)it.d("t", 0.0);
                k.px = (float)it.d("px", 0.0); k.py = (float)it.d("py", 0.0); k.pz = (float)it.d("pz", 0.0);
                k.rx = (float)it.d("rx", 0.0); k.ry = (float)it.d("ry", 0.0); k.rz = (float)it.d("rz", 0.0);
                k.sx = (float)it.d("sx", 2.0); k.sy = (float)it.d("sy", 2.0); k.sz = (float)it.d("sz", 2.0);
                out.frames.push_back(k);
            }
        }
        out.sortFrames();
        return true;
    }
};

class StudioScene {
public:
    SceneNode root;
    int nextId = 1;

    StudioScene() {
        root.id = "n0";
        root.name = "SceneTree";
        root.className = "SceneTree";
    }

    std::string genId() { return "n" + std::to_string(nextId++); }

    static SceneNode makeNode(const std::string &id, const std::string &name, const std::string &cls) {
        SceneNode n;
        n.id = id; n.name = name; n.className = cls;
        return n;
    }

    static StudioScene createDefault3DScene() {
        StudioScene m;
        SceneNode world = makeNode(m.genId(), "World3D", "World3D");

        // Ground body + mesh + collider (Godot-style hierarchy)
        auto ground = makeNode(m.genId(), "Ground", "StaticBody3D");
        ground.posY = -1.0f; ground.sizeX = 80; ground.sizeY = 2; ground.sizeZ = 80;
        ground.argbColor = 0xFF7B8390; ground.material = "SmoothPlastic";
        auto gMesh = makeNode(m.genId(), "GroundMesh", "Box");
        gMesh.posY = -1.0f; gMesh.sizeX = 80; gMesh.sizeY = 2; gMesh.sizeZ = 80;
        gMesh.argbColor = 0xFF7B8390; gMesh.material = "SmoothPlastic";
        gMesh.castShadow = false;
        auto gCol = makeNode(m.genId(), "GroundCollider", "CollisionBox");
        gCol.posY = -1.0f; gCol.sizeX = 80; gCol.sizeY = 2; gCol.sizeZ = 80;
        ground.children.push_back(gMesh);
        ground.children.push_back(gCol);
        world.children.push_back(ground);

        auto cube = makeNode(m.genId(), "DemoCube", "Box");
        cube.posX = 0; cube.posY = 2.0f; cube.posZ = -4;
        cube.sizeX = 3; cube.sizeY = 3; cube.sizeZ = 3;
        cube.argbColor = 0xFF3B82F6; cube.material = "Plastic";
        auto cubeCol = makeNode(m.genId(), "CubeCollider", "CollisionBox");
        cubeCol.posX = 0; cubeCol.posY = 2.0f; cubeCol.posZ = -4;
        cubeCol.sizeX = 3; cubeCol.sizeY = 3; cubeCol.sizeZ = 3;
        cube.children.push_back(cubeCol);
        world.children.push_back(cube);

        auto ramp = makeNode(m.genId(), "Ramp", "Wedge");
        ramp.posX = -9; ramp.posY = 1.5f; ramp.posZ = -8;
        ramp.sizeX = 6; ramp.sizeY = 3; ramp.sizeZ = 8;
        ramp.argbColor = 0xFFA1A8B4;
        world.children.push_back(ramp);

        auto sph = makeNode(m.genId(), "Sphere", "Sphere");
        sph.posX = 8; sph.posY = 2.0f; sph.posZ = -6;
        sph.sizeX = 3; sph.sizeY = 3; sph.sizeZ = 3;
        sph.argbColor = 0xFF22C55E;
        world.children.push_back(sph);

        auto cam = makeNode(m.genId(), "Camera3D", "Camera3D");
        cam.posX = 0; cam.posY = 7; cam.posZ = 16;
        cam.rotX = -18; cam.fov = 60.0f;
        world.children.push_back(cam);

        auto sun = makeNode(m.genId(), "DirectionalLight3D", "DirectionalLight3D");
        sun.posX = 8; sun.posY = 14; sun.posZ = 8;
        sun.rotX = -45; sun.rotY = 30; sun.energy = 1.2f;
        world.children.push_back(sun);

        auto ms = makeNode(m.genId(), "MainScript", "Script");
        ms.scriptPath = "main.obs";
        world.children.push_back(ms);
        m.root.children.push_back(world);

        auto scripts = makeNode(m.genId(), "Scripts", "Scripts"); scripts.expanded = true;
        auto gs = makeNode(m.genId(), "GameScript", "Script"); gs.scriptPath = "scripts/game.obs";
        scripts.children.push_back(gs);
        m.root.children.push_back(scripts);

        auto lit = makeNode(m.genId(), "Environment", "Environment"); lit.expanded = false;
        m.root.children.push_back(lit);

        auto hud = makeNode(m.genId(), "HUD", "HUD"); hud.expanded = false;
        m.root.children.push_back(hud);

        return m;
    }

    SceneNode *findService(const std::string &cls) {
        for (auto &c : root.children) if (c.className == cls || c.name == cls) return &c;
        if (cls == "World3D") {
            for (auto &c : root.children) {
                if (c.className == "World" || c.name == "World" || c.name == "Workspace") return &c;
            }
        }
        return nullptr;
    }

    SceneNode *findById(const std::string &id) { return findByIdRec(root, id); }
    SceneNode *findParentOf(const std::string &id) { return findParentRec(root, id); }

    void collectMeshes(std::vector<const SceneNode *> &out) const { collectMeshesRec(root, out); }
    void collect3DNodes(std::vector<const SceneNode *> &out) const { collect3DRec(root, out); }
    void collectUi(std::vector<const SceneNode *> &out) const { collectUiRec(root, out); }

    const SceneNode *findActiveCamera() const { return findClassRec(root, "Camera3D"); }
    const SceneNode *environment() const {
        for (const auto &c : root.children) {
            if (c.className == "Environment" || c.className == "Lighting") return &c;
        }
        return nullptr;
    }
    SceneNode *findEnvironment() {
        for (auto &c : root.children) {
            if (c.className == "Environment" || c.className == "Lighting") return &c;
        }
        return nullptr;
    }

    SceneNode *createNode(SceneNode *parent, const std::string &cls, const std::string &baseName) {
        if (!parent) parent = findService("World3D");
        if (!parent) return nullptr;
        SceneNode n = makeNode(genId(), baseName, cls);
        n.posX = 0; n.posY = 2.0f; n.posZ = -2.0f;
        n.sizeX = 2; n.sizeY = 2; n.sizeZ = 2;
        if (cls == "Sphere") { n.sizeX = n.sizeY = n.sizeZ = 2.5f; n.argbColor = 0xFF4ADE80; }
        else if (cls == "Cylinder") { n.sizeX = n.sizeZ = 2.5f; n.sizeY = 3.5f; n.argbColor = 0xFF38BDF8; }
        else if (cls == "Wedge") { n.sizeX = 3; n.sizeY = 2.5f; n.sizeZ = 4; n.argbColor = 0xFF94A3B8; }
        else if (cls == "Plane") { n.sizeX = 12; n.sizeY = 0.2f; n.sizeZ = 12; n.posY = 0.0f; n.argbColor = 0xFF64748B; }
        else if (cls == "CollisionBox" || cls == "CollisionSphere" || cls == "Area3D") {
            n.sizeX = 2.5f; n.sizeY = 2.5f; n.sizeZ = 2.5f; n.argbColor = 0xFF22D3EE;
        } else if (cls == "StaticBody3D" || cls == "RigidBody3D") {
            n.staticBody = (cls == "StaticBody3D");
            n.argbColor = 0xFFA3A2A5;
        } else if (cls == "Camera3D") {
            n.posX = 0; n.posY = 5; n.posZ = 10; n.fov = 60.0f; n.argbColor = 0xFF38BDF8;
        } else if (cls == "DirectionalLight3D" || cls == "PointLight3D") {
            n.posX = 4; n.posY = 8; n.posZ = 4; n.argbColor = 0xFFFACC15;
        } else {
            n.argbColor = 0xFFA3A2A5;
        }
        parent->expanded = true;
        parent->children.push_back(n);
        return &parent->children.back();
    }

    bool removeById(const std::string &id) {
        if (id == root.id) return false;
        for (const auto &svc : root.children) if (svc.id == id) return false;
        return removeRec(root, id);
    }

    static void moveNodeDelta(SceneNode &node, float dx, float dy, float dz) {
        if (node.is3DNode()) {
            node.posX += dx;
            node.posY += dy;
            node.posZ += dz;
        }
        for (auto &c : node.children) moveNodeDelta(c, dx, dy, dz);
    }

    static bool isDescendantOrSelf(const SceneNode &anc, const std::string &id) {
        if (anc.id == id) return true;
        for (const auto &c : anc.children) if (isDescendantOrSelf(c, id)) return true;
        return false;
    }

    bool reparentNode(const std::string &id, const std::string &newParentId) {
        if (id.empty() || newParentId.empty() || id == newParentId) return false;
        for (const auto &svc : root.children) if (svc.id == id) return false;
        SceneNode *src = findById(id);
        SceneNode *dst = findById(newParentId);
        if (!src || !dst || isDescendantOrSelf(*src, newParentId)) return false;
        SceneNode extracted;
        if (!extractRec(root, id, extracted)) return false;
        dst = findById(newParentId);
        if (!dst) return false;
        dst->expanded = true;
        dst->children.push_back(std::move(extracted));
        return true;
    }

    std::string groupNodes(const std::vector<std::string> &ids, const std::string &groupClass) {
        if (ids.empty()) return "";
        SceneNode *firstParent = nullptr;
        for (const auto &id : ids) {
            bool isSvc = false;
            for (const auto &svc : root.children) if (svc.id == id) isSvc = true;
            if (!isSvc) {
                firstParent = findParentOf(id);
                if (firstParent) break;
            }
        }
        if (!firstParent) firstParent = findService("World3D");
        if (!firstParent) return "";
        std::string gName = groupClass + std::to_string(nextId);
        SceneNode grp = makeNode(genId(), gName, groupClass);
        grp.expanded = true;
        firstParent->expanded = true;
        firstParent->children.push_back(std::move(grp));
        std::string gid = firstParent->children.back().id;
        for (const auto &id : ids) reparentNode(id, gid);
        return gid;
    }

    bool ungroupNode(const std::string &groupId) {
        SceneNode *grp = findById(groupId);
        SceneNode *parent = findParentOf(groupId);
        if (!grp || !parent || !grp->isGroup()) return false;
        std::string parentId = parent->id;
        std::vector<SceneNode> kids = std::move(grp->children);
        if (!removeById(groupId)) return false;
        parent = findById(parentId);
        if (!parent) return false;
        for (auto &k : kids) parent->children.push_back(std::move(k));
        return true;
    }

    void collectContainers(std::vector<const SceneNode *> &out) const {
        for (const auto &c : root.children) collectContainersRec(c, out);
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
        StudioScene sc = createDefault3DScene();
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

    static void appendPartMesh(const SceneNode &part, std::vector<MeshVertex> &out,
                               float uvU0 = 0.0f, float uvV0 = 0.0f, float uvU1 = 1.0f, float uvV1 = 1.0f) {
        float hx = part.sizeX * 0.5f, hy = part.sizeY * 0.5f, hz = part.sizeZ * 0.5f;
        if (part.className == "Plane") hy = std::max(0.05f, hy);
        uint8_t r = (part.argbColor >> 16) & 0xFF;
        uint8_t g = (part.argbColor >> 8) & 0xFF;
        uint8_t b = part.argbColor & 0xFF;
        uint8_t a = (uint8_t)std::max(25, std::min(255, (int)std::lround((1.0f - part.transparency) * 255.0f)));
        uint32_t col = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
        Vec3f center{part.posX, part.posY, part.posZ};
        auto mapU = [&](float lu) { return uvU0 + (uvU1 - uvU0) * std::clamp(lu, 0.0f, 1.0f); };
        auto mapV = [&](float lv) { return uvV0 + (uvV1 - uvV0) * std::clamp(lv, 0.0f, 1.0f); };

        auto emitTriUV = [&](Vec3f p0, float u0, float v0, Vec3f p1, float u1, float v1, Vec3f p2, float u2, float v2) {
            Vec3f n = cross3(p1 - p0, p2 - p0);
            Vec3f triCenter = (p0 + p1 + p2) * (1.0f / 3.0f);
            if (dot3(n, triCenter - center) < 0.0f) {
                std::swap(p1, p2);
                std::swap(u1, u2);
                std::swap(v1, v2);
                n = cross3(p1 - p0, p2 - p0);
            }
            n = norm3(n);
            out.push_back({p0.x, p0.y, p0.z, n.x, n.y, n.z, col, mapU(u0), mapV(v0)});
            out.push_back({p1.x, p1.y, p1.z, n.x, n.y, n.z, col, mapU(u1), mapV(v1)});
            out.push_back({p2.x, p2.y, p2.z, n.x, n.y, n.z, col, mapU(u2), mapV(v2)});
        };

        auto emitQuadUV = [&](Vec3f a0, float u0, float v0, Vec3f a1, float u1, float v1,
                              Vec3f a2, float u2, float v2, Vec3f a3, float u3, float v3) {
            emitTriUV(a0, u0, v0, a1, u1, v1, a2, u2, v2);
            emitTriUV(a0, u0, v0, a2, u2, v2, a3, u3, v3);
        };

        auto emitQuad = [&](Vec3f a0, Vec3f a1, Vec3f a2, Vec3f a3) {
            emitQuadUV(a0, 0.0f, 1.0f, a1, 1.0f, 1.0f, a2, 1.0f, 0.0f, a3, 0.0f, 0.0f);
        };

        int segs = std::clamp(part.segments, 6, 64);

        if (part.className == "Wedge" || part.className == "WedgePart") {
            Vec3f v[6] = {
                transformVert(part, -hx, -hy, -hz), transformVert(part,  hx, -hy, -hz),
                transformVert(part,  hx, -hy,  hz), transformVert(part, -hx, -hy,  hz),
                transformVert(part, -hx,  hy,  hz), transformVert(part,  hx,  hy,  hz)
            };
            emitQuad(v[0], v[1], v[2], v[3]);
            emitQuad(v[3], v[2], v[5], v[4]);
            emitQuad(v[0], v[4], v[5], v[1]);
            emitTriUV(v[0], 0.0f, 1.0f, v[3], 1.0f, 1.0f, v[4], 1.0f, 0.0f);
            emitTriUV(v[1], 1.0f, 1.0f, v[5], 0.0f, 0.0f, v[2], 0.0f, 1.0f);
            return;
        }

        if (part.className == "Sphere") {
            int stacks = std::max(4, segs / 2);
            int slices = segs;
            constexpr float kPi = 3.14159265f;
            auto pt = [&](float phi, float theta) {
                return transformVert(part,
                    std::cos(phi) * std::cos(theta) * hx,
                    std::sin(phi) * hy,
                    std::cos(phi) * std::sin(theta) * hz);
            };
            for (int i = 0; i < stacks; ++i) {
                float fi0 = (float)i / stacks, fi1 = (float)(i + 1) / stacks;
                float p0 = kPi * (-0.5f + fi0);
                float p1 = kPi * (-0.5f + fi1);
                for (int j = 0; j < slices; ++j) {
                    float fj0 = (float)j / slices, fj1 = (float)(j + 1) / slices;
                    float t0 = 2.0f * kPi * fj0;
                    float t1 = 2.0f * kPi * fj1;
                    emitQuadUV(pt(p0, t0), fj0, 1.0f - fi0, pt(p0, t1), fj1, 1.0f - fi0,
                               pt(p1, t1), fj1, 1.0f - fi1, pt(p1, t0), fj0, 1.0f - fi1);
                }
            }
            return;
        }

        if (part.className == "Cylinder") {
            constexpr float kTwoPi = 6.28318530f;
            Vec3f topC = transformVert(part, 0, hy, 0);
            Vec3f botC = transformVert(part, 0, -hy, 0);
            for (int i = 0; i < segs; ++i) {
                float f0 = (float)i / segs, f1 = (float)(i + 1) / segs;
                float a0 = kTwoPi * f0, a1 = kTwoPi * f1;
                Vec3f b0 = transformVert(part, std::cos(a0) * hx, -hy, std::sin(a0) * hz);
                Vec3f b1 = transformVert(part, std::cos(a1) * hx, -hy, std::sin(a1) * hz);
                Vec3f t0 = transformVert(part, std::cos(a0) * hx,  hy, std::sin(a0) * hz);
                Vec3f t1 = transformVert(part, std::cos(a1) * hx,  hy, std::sin(a1) * hz);
                emitQuadUV(b0, f0, 1.0f, b1, f1, 1.0f, t1, f1, 0.0f, t0, f0, 0.0f);
                emitTriUV(topC, 0.5f, 0.5f, t1, 0.5f + 0.5f * std::cos(a1), 0.5f + 0.5f * std::sin(a1),
                          t0, 0.5f + 0.5f * std::cos(a0), 0.5f + 0.5f * std::sin(a0));
                emitTriUV(botC, 0.5f, 0.5f, b0, 0.5f + 0.5f * std::cos(a0), 0.5f + 0.5f * std::sin(a0),
                          b1, 0.5f + 0.5f * std::cos(a1), 0.5f + 0.5f * std::sin(a1));
            }
            return;
        }

        float rad = std::clamp(part.cornerRadius, 0.0f, 0.48f) * std::min({hx, hy, hz});
        if (rad > 0.005f) {
            float ix = std::max(0.0f, hx - rad);
            float iy = std::max(0.0f, hy - rad);
            float iz = std::max(0.0f, hz - rad);
            int sub = std::clamp(segs / 4, 2, 10);
            auto buildAxis = [&](float h, float inH) {
                std::vector<float> vals;
                for (int i = 0; i <= sub; ++i) vals.push_back(-h + (h - inH) * (float)i / sub);
                if (inH > 1e-4f) {
                    for (int i = 1; i < sub; ++i) vals.push_back(inH + (h - inH) * (float)i / sub);
                    vals.push_back(h);
                } else {
                    for (int i = 1; i <= sub; ++i) vals.push_back((h - inH) * (float)i / sub);
                }
                return vals;
            };
            std::vector<float> xs = buildAxis(hx, ix);
            std::vector<float> ys = buildAxis(hy, iy);
            std::vector<float> zs = buildAxis(hz, iz);
            auto roundPt = [&](float lx, float ly, float lz) {
                float qx = std::clamp(lx, -ix, ix);
                float qy = std::clamp(ly, -iy, iy);
                float qz = std::clamp(lz, -iz, iz);
                Vec3f d{lx - qx, ly - qy, lz - qz};
                float dl = std::sqrt(dot3(d, d));
                if (dl > 1e-6f) {
                    Vec3f p = Vec3f{qx, qy, qz} + d * (rad / dl);
                    return transformVert(part, p.x, p.y, p.z);
                }
                return transformVert(part, lx, ly, lz);
            };
            auto emitGridFace = [&](int axis, float sign) {
                const auto &us = (axis == 0) ? zs : xs;
                const auto &vs = (axis == 1) ? zs : ys;
                float uh = (axis == 0) ? hz : hx;
                float vh = (axis == 1) ? hz : hy;
                auto eval = [&](size_t i, size_t j) {
                    float u = us[i], v = vs[j];
                    if (axis == 0) return roundPt(sign * hx, v, u);
                    if (axis == 1) return roundPt(u, sign * hy, v);
                    return roundPt(u, v, sign * hz);
                };
                for (size_t i = 0; i + 1 < us.size(); ++i) {
                    for (size_t j = 0; j + 1 < vs.size(); ++j) {
                        float u0 = (us[i] + uh) / (2.0f * uh), u1 = (us[i + 1] + uh) / (2.0f * uh);
                        float v0 = 1.0f - (vs[j] + vh) / (2.0f * vh), v1 = 1.0f - (vs[j + 1] + vh) / (2.0f * vh);
                        emitQuadUV(eval(i, j), u0, v0, eval(i + 1, j), u1, v0,
                                   eval(i + 1, j + 1), u1, v1, eval(i, j + 1), u0, v1);
                    }
                }
            };
            emitGridFace(0,  1.0f); emitGridFace(0, -1.0f);
            emitGridFace(1,  1.0f); emitGridFace(1, -1.0f);
            emitGridFace(2,  1.0f); emitGridFace(2, -1.0f);
            return;
        }

        Vec3f v[8] = {
            transformVert(part, -hx, -hy, -hz), transformVert(part,  hx, -hy, -hz),
            transformVert(part,  hx,  hy, -hz), transformVert(part, -hx,  hy, -hz),
            transformVert(part, -hx, -hy,  hz), transformVert(part,  hx, -hy,  hz),
            transformVert(part,  hx,  hy,  hz), transformVert(part, -hx,  hy,  hz)
        };
        emitQuad(v[7], v[6], v[2], v[3]);
        emitQuad(v[0], v[1], v[5], v[4]);
        emitQuad(v[4], v[5], v[6], v[7]);
        emitQuad(v[1], v[0], v[3], v[2]);
        emitQuad(v[5], v[1], v[2], v[6]);
        emitQuad(v[0], v[4], v[7], v[3]);
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
    static void collectMeshesRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.isMeshNode() && n.visible) out.push_back(&n);
        for (const auto &c : n.children) collectMeshesRec(c, out);
    }
    static void collect3DRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.is3DNode()) out.push_back(&n);
        for (const auto &c : n.children) collect3DRec(c, out);
    }
    static void collectUiRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.isUi()) out.push_back(&n);
        for (const auto &c : n.children) collectUiRec(c, out);
    }
    static void collectContainersRec(const SceneNode &n, std::vector<const SceneNode *> &out) {
        if (n.className == "World3D" || n.className == "Scripts" || n.isGroup()) out.push_back(&n);
        for (const auto &c : n.children) collectContainersRec(c, out);
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
    static bool extractRec(SceneNode &n, const std::string &id, SceneNode &out) {
        for (auto it = n.children.begin(); it != n.children.end(); ++it) {
            if (it->id == id) {
                out = std::move(*it);
                n.children.erase(it);
                return true;
            }
            if (extractRec(*it, id, out)) return true;
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
           << ",\"staticBody\":" << (n.staticBody ? "true" : "false")
           << ",\"collidable\":" << (n.collidable ? "true" : "false")
           << ",\"mass\":" << n.mass
           << ",\"gravityScale\":" << n.gravityScale
           << ",\"fov\":" << n.fov
           << ",\"energy\":" << n.energy
           << ",\"range\":" << n.range
           << ",\"visible\":" << (n.visible ? "true" : "false")
           << ",\"castShadow\":" << (n.castShadow ? "true" : "false")
           << ",\"locked\":" << (n.locked ? "true" : "false")
           << ",\"disabled\":" << (n.disabled ? "true" : "false")
           << ",\"scriptPath\":" << MiniJson::esc(n.scriptPath)
           << ",\"animPath\":" << MiniJson::esc(n.animPath)
           << ",\"animAutoPlay\":" << (n.animAutoPlay ? "true" : "false")
           << ",\"animLoop\":" << (n.animLoop ? "true" : "false")
           << ",\"text\":" << MiniJson::esc(n.text)
           << ",\"textSize\":" << n.textSize
           << ",\"clockTime\":" << n.clockTime
           << ",\"skyPreset\":" << MiniJson::esc(n.skyPreset)
           << ",\"skyTopColor\":" << (int32_t)n.skyTopColor
           << ",\"skyBotColor\":" << (int32_t)n.skyBotColor
           << ",\"texturePath\":" << MiniJson::esc(n.texturePath)
           << ",\"modelPath\":" << MiniJson::esc(n.modelPath)
           << ",\"modelAnimIndex\":" << n.modelAnimIndex
           << ",\"segments\":" << n.segments
           << ",\"cornerRadius\":" << n.cornerRadius
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
        n.name = j.s("name", "Box");
        n.className = j.s("className", "Box");
        n.expanded = j.bl("expanded", false);
        n.posX = (float)j.d("posX", 0); n.posY = (float)j.d("posY", 0); n.posZ = (float)j.d("posZ", 0);
        n.sizeX = (float)j.d("sizeX", 2); n.sizeY = (float)j.d("sizeY", 2); n.sizeZ = (float)j.d("sizeZ", 2);
        n.rotX = (float)j.d("rotX", 0); n.rotY = (float)j.d("rotY", 0); n.rotZ = (float)j.d("rotZ", 0);
        n.argbColor = (uint32_t)(int64_t)j.d("color", (double)(int32_t)0xFFA3A2A5);
        n.material = j.s("material", "Plastic");
        n.transparency = (float)j.d("transparency", 0);
        n.staticBody = j.bl("staticBody", j.bl("anchored", true));
        n.collidable = j.bl("collidable", j.bl("canCollide", true));
        n.mass = (float)j.d("mass", 1.0);
        n.gravityScale = (float)j.d("gravityScale", 1.0);
        n.fov = (float)j.d("fov", 60.0);
        n.energy = (float)j.d("energy", 1.2);
        n.range = (float)j.d("range", 16.0);
        n.visible = j.bl("visible", true);
        n.castShadow = j.bl("castShadow", true);
        n.locked = j.bl("locked", false);
        n.disabled = j.bl("disabled", false);
        n.scriptPath = j.s("scriptPath", "");
        n.animPath = j.s("animPath", "");
        n.animAutoPlay = j.bl("animAutoPlay", false);
        n.animLoop = j.bl("animLoop", true);
        n.text = j.s("text", "3D Scene");
        n.textSize = (int)j.d("textSize", 18);
        n.clockTime = (float)j.d("clockTime", 14.0);
        n.skyPreset = j.s("skyPreset", "Day");
        n.skyTopColor = (uint32_t)(int64_t)j.d("skyTopColor", (double)(int32_t)0xFF1E6EC4);
        n.skyBotColor = (uint32_t)(int64_t)j.d("skyBotColor", (double)(int32_t)0xFF8CC4F2);
        n.texturePath = j.s("texturePath", "");
        n.modelPath = j.s("modelPath", "");
        n.modelAnimIndex = (int)j.d("modelAnimIndex", 0);
        n.segments = std::clamp((int)j.d("segments", 24), 6, 64);
        n.cornerRadius = std::clamp((float)j.d("cornerRadius", 0.0), 0.0f, 0.48f);
        if (const JsonVal *kids = j.get("children")) {
            if (kids->type == JsonVal::J_ARR) {
                for (const auto &kc : kids->arr) n.children.push_back(nodeFromJson(kc));
            }
        }
        return n;
    }
};
