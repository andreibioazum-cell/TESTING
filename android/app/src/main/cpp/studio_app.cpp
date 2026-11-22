#define STB_TRUETYPE_IMPLEMENTATION
#include "studio_app.hpp"
#include "obsidian_core.h"
#include "third_party/stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

extern "C" {
void lc_editor_init(void);
void lc_editor_free(void);
void lc_editor_set_text(const char *utf8);
const char *lc_editor_get_text(void);
void lc_editor_insert(const char *utf8);
void lc_editor_newline(void);
void lc_editor_backspace(void);
void lc_editor_delete(void);
int lc_editor_line_count(void);
int lc_editor_cursor_line(void);
int lc_editor_cursor_col(void);
void lc_editor_set_cursor_line_col(int line, int col);
void lc_editor_move_cursor(int dx, int dy);
int lc_editor_get_line(int line, char *out, int cap);
int lc_editor_undo(void);
int lc_editor_redo(void);
int lc_editor_tokenize_line(int line, int *out_triplets, int max_triplets);
}

static void *g_obsidianProj = nullptr;

namespace Col {
static inline uint32_t bg()       { return StudioFont::rgba(14, 14, 14); }
static inline uint32_t panel()    { return StudioFont::rgba(22, 22, 22); }
static inline uint32_t header()   { return StudioFont::rgba(30, 30, 30); }
static inline uint32_t btn()      { return StudioFont::rgba(42, 42, 42); }
static inline uint32_t btnAct()   { return StudioFont::rgba(82, 82, 82); }
static inline uint32_t green()    { return StudioFont::rgba(72, 72, 72); }
static inline uint32_t red()      { return StudioFont::rgba(56, 56, 56); }
static inline uint32_t border()   { return StudioFont::rgba(66, 66, 66); }
static inline uint32_t text()     { return StudioFont::rgba(244, 244, 244); }
static inline uint32_t muted()    { return StudioFont::rgba(164, 164, 164); }
static inline uint32_t accent()   { return StudioFont::rgba(255, 255, 255); }
static inline uint32_t selRow()   { return StudioFont::rgba(64, 64, 64); }
static inline uint32_t argbToUi(uint32_t argb) {
    uint8_t a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return StudioFont::rgba(r, g, b, a);
}
} // namespace Col

static std::string fmtNum(float v) {
    char b[32];
    if (std::fabs(v - std::round(v)) < 0.01f) std::snprintf(b, sizeof(b), "%d", (int)std::lround(v));
    else std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

static std::string fmtVec3(float x, float y, float z) {
    return fmtNum(x) + ", " + fmtNum(y) + ", " + fmtNum(z);
}

static bool parseVec3(const std::string &s, float &x, float &y, float &z) {
    float a = 0, b = 0, c = 0;
    std::string clean = s;
    for (char &ch : clean) if (ch == ',') ch = ' ';
    if (std::sscanf(clean.c_str(), "%f %f %f", &a, &b, &c) == 3) {
        x = a; y = b; z = c; return true;
    }
    return false;
}

static void mat4Mul(float *out, const float *a, const float *b) {
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

void StudioApp::init(const std::string &rootDir, const std::string &initialProjName, bool is3D,
                     int fontFamily, int fontSize, int theme, int tabSpaces, bool lineNums, const std::string &customFont) {
    playerOnly = false;
    lc_editor_init();
    projectsRoot = rootDir;
    prefFontFamily = fontFamily;
    prefFontSize = fontSize;
    prefTheme = theme;
    prefTabSpaces = tabSpaces;
    prefLineNumbers = lineNums;
    prefCustomFontPath = customFont;

    applyFont(customFont, fontFamily);
    openProject(initialProjName, is3D);
    showProjectsTab = true;
}

void StudioApp::initPlayer(const std::string &rootDir, const std::string &projName, bool is3D) {
    playerOnly = true;
    lc_editor_init();
    projectsRoot = rootDir;
    prefFontFamily = 0;
    prefFontSize = 14;
    prefTheme = 0;
    prefTabSpaces = 4;
    prefLineNumbers = false;
    prefCustomFontPath.clear();
    applyFont("", 0);
    openProject(projName, is3D);
    showProjectsTab = false;
    activeScript.clear();
    startSim();
}

void StudioApp::onApkBuildResult(bool ok, const std::string &msg) {
    appendOutput(msg, ok ? Col::green() : Col::red());
}

void StudioApp::applyFont(const std::string &customPath, int family) {
    prefCustomFontPath = customPath;
    prefFontFamily = family;
    font.loadBestSystemOrCustom(customPath, family);
}

void StudioApp::initTexAtlas() {
    if (!texAtlasRgba.empty()) return;
    texAtlasRgba.assign((size_t)kTexAtlasSize * kTexAtlasSize * 4, 255);
    texAtlasPaths.assign(1, "__white__");
    texAtlasVersion++;
}

int StudioApp::ensureTextureTile(const std::string &relPath) {
    initTexAtlas();
    if (relPath.empty()) return 0;
    for (size_t i = 1; i < texAtlasPaths.size(); ++i) {
        if (texAtlasPaths[i] == relPath) return (int)i;
    }
    if (texAtlasPaths.size() >= 16 || currentProjectDir.empty()) return 0;
    std::string full = currentProjectDir + "/" + relPath;
    int w = 0, h = 0, comp = 0;
    unsigned char *pix = stbi_load(full.c_str(), &w, &h, &comp, 4);
    if (!pix || w <= 0 || h <= 0) {
        if (pix) stbi_image_free(pix);
        return 0;
    }
    int idx = (int)texAtlasPaths.size();
    texAtlasPaths.push_back(relPath);
    int tileX = (idx % 4) * 256;
    int tileY = (idx / 4) * 256;
    for (int py = 0; py < 256; ++py) {
        float sy = (float)std::clamp(py - 1, 0, 253) / 253.0f * (float)(h - 1);
        int y0 = (int)sy, y1 = std::min(h - 1, y0 + 1);
        float fy = sy - (float)y0;
        for (int px = 0; px < 256; ++px) {
            float sx = (float)std::clamp(px - 1, 0, 253) / 253.0f * (float)(w - 1);
            int x0 = (int)sx, x1 = std::min(w - 1, x0 + 1);
            float fx = sx - (float)x0;
            size_t dstOff = ((size_t)(tileY + py) * kTexAtlasSize + (size_t)(tileX + px)) * 4;
            for (int c = 0; c < 4; ++c) {
                float c00 = pix[(y0 * w + x0) * 4 + c];
                float c10 = pix[(y0 * w + x1) * 4 + c];
                float c01 = pix[(y1 * w + x0) * 4 + c];
                float c11 = pix[(y1 * w + x1) * 4 + c];
                float v = (c00 * (1.0f - fx) + c10 * fx) * (1.0f - fy) +
                          (c01 * (1.0f - fx) + c11 * fx) * fy;
                texAtlasRgba[dstOff + (size_t)c] = (uint8_t)std::clamp((int)std::lround(v), 0, 255);
            }
        }
    }
    stbi_image_free(pix);
    texAtlasVersion++;
    return idx;
}

void StudioApp::applyNodeTexture(const std::string &nodeId, const std::string &relPath) {
    if (auto *n = scene.findById(nodeId)) {
        n->texturePath = relPath;
        if (!relPath.empty()) ensureTextureTile(relPath);
        scene.saveToFile(currentProjectDir + "/scene.json");
        appendOutput("Texture applied to " + n->name, Col::muted());
    }
}

GltfModelAsset *StudioApp::getOrLoadGltf(const std::string &relPath) {
    if (relPath.empty() || currentProjectDir.empty()) return nullptr;
    std::string full = currentProjectDir + "/" + relPath;
    auto it = gltfCache.find(full);
    if (it != gltfCache.end()) return it->second.loaded ? &it->second : nullptr;
    GltfModelAsset asset;
    bool ok = GltfModelAsset::load(full, currentProjectDir, asset);
    auto &slot = gltfCache[full];
    if (ok) {
        slot = std::move(asset);
        return &slot;
    }
    slot.loaded = false;
    return nullptr;
}

void StudioApp::applyNodeModel(const std::string &nodeId, const std::string &relPath) {
    if (relPath.empty()) return;
    std::string full = currentProjectDir + "/" + relPath;
    gltfCache.erase(full);
    SceneNode *n = nodeId.empty() ? nullptr : scene.findById(nodeId);
    if (!n) {
        SceneNode *parent = scene.findService("World3D");
        if (!parent) return;
        std::string baseName = relPath;
        if (auto sl = baseName.find_last_of('/'); sl != std::string::npos) baseName = baseName.substr(sl + 1);
        if (auto dot = baseName.find_last_of('.'); dot != std::string::npos) baseName = baseName.substr(0, dot);
        n = scene.createNode(parent, "GltfModel", baseName);
        if (!n) return;
        n->posX = camTargetX; n->posY = 1.8f; n->posZ = camTargetZ;
        n->sizeX = 3.2f; n->sizeY = 3.2f; n->sizeZ = 3.2f;
        n->argbColor = 0xFFFFFFFF;
        selectedId = n->id;
    }
    n->className = "GltfModel";
    n->modelPath = relPath;
    n->modelAnimIndex = 0;
    GltfModelAsset *gm = getOrLoadGltf(relPath);
    if (gm && !gm->animations.empty()) {
        n->animAutoPlay = true;
        n->animLoop = true;
    }
    scene.saveToFile(currentProjectDir + "/scene.json");
    appendOutput(gm ? ("Loaded glTF: " + relPath + " (" + std::to_string(gm->animations.size()) + " anims)")
                    : ("Failed to parse glTF: " + relPath),
                 gm ? Col::text() : Col::muted());
}

void StudioApp::syncOrientation() {
    if (playerOnly) {
        if (lastRequestedOrientation != 0) {
            lastRequestedOrientation = 0;
            if (callbacks.setOrientationPortrait) callbacks.setOrientationPortrait(false);
        }
        return;
    }
    bool wantPortrait = (!showProjectsTab && !activeScript.empty()) ? editorPortrait : false;
    int code = wantPortrait ? 1 : 0;
    if (code != lastRequestedOrientation) {
        lastRequestedOrientation = code;
        if (callbacks.setOrientationPortrait) callbacks.setOrientationPortrait(wantPortrait);
    }
}

void StudioApp::openProject(const std::string &projName, bool is3D) {
    stopSim();
    saveActiveScript();
    openScripts.clear();
    activeScript.clear();
    showProjectsTab = false;
    activeRibbon = RibbonTab::HOME;
    currentProjectName = projName;
    currentProjectDir = projectsRoot + "/" + projName;
    currentIs3D = is3D;
    selectedId.clear();
    multiSelectMode = false;
    multiSelectedIds.clear();
    outputLines.clear();
    editorScrollY = 0;
    explorerScrollY = 0;
    propsScrollY = 0;
    animPlaying = false;
    animTime = 0.0f;
    animBoneNodeIdx = 0;
    gltfLiveAnimTime = 0.0f;
    showAnimator = false;
    activeAnimFile.clear();
    animTargetNodeId.clear();
    gltfCache.clear();
    texAtlasRgba.clear();
    texAtlasPaths.clear();
    initTexAtlas();

    if (is3D) {
        scene = StudioScene::loadOrCreate(currentProjectDir);
        if (auto *ws = scene.findService("World3D")) {
            if (!ws->children.empty()) selectedId = ws->children[0].id;
        }
        camTargetX = 0; camTargetY = 1.5f; camTargetZ = -3.0f;
        camYaw = 0.36f; camPitch = 0.34f; camDist = 28.0f;
        appendOutput("Loaded 3D project: " + projName, Col::muted());
    } else {
        scene = StudioScene();
        std::string entry = "main.obs";
        {
            std::ifstream tf(currentProjectDir + "/main.obs");
            if (!tf.good()) {
                std::ifstream lf(currentProjectDir + "/main.lua");
                if (lf.good()) entry = "main.lua";
            }
        }
        openScript(entry);
        appendOutput("Loaded 2D Obsidronium project: " + projName, Col::muted());
    }
    syncOrientation();
}

void StudioApp::dispose() {
    stopSim();
    saveActiveScript();
    lc_editor_free();
}

void StudioApp::appendOutput(const std::string &txt, uint32_t col) {
    std::istringstream ss(txt);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        outputLines.push_back({line, col});
        if (outputLines.size() > 200) outputLines.erase(outputLines.begin());
    }
    outputScrollY = 1e9f;
}

SceneNode *StudioApp::selectedNode() {
    return selectedId.empty() ? nullptr : scene.findById(selectedId);
}

void StudioApp::openScript(const std::string &relPath) {
    if (relPath.empty()) return;
    saveActiveScript();
    showProjectsTab = false;
    if (std::find(openScripts.begin(), openScripts.end(), relPath) == openScripts.end()) {
        openScripts.push_back(relPath);
    }
    activeScript = relPath;
    std::string full = currentProjectDir + "/" + relPath;
    std::ifstream f(full, std::ios::binary);
    std::string content;
    if (f) {
        std::ostringstream ss; ss << f.rdbuf(); content = ss.str();
    } else {
        content = "-- " + relPath + "\nprint(\"Hello from " + relPath + "\")\n";
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        if (out) out << content;
    }
    lc_editor_set_text(content.c_str());
    editorScrollY = 0;
    editorStatus.clear();
    syncOrientation();
}

void StudioApp::saveActiveScript() {
    if (activeScript.empty() || currentProjectDir.empty()) return;
    std::string full = currentProjectDir + "/" + activeScript;
    const char *txt = lc_editor_get_text();
    if (!txt) return;
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (out) out << txt;
}

void StudioApp::startSim() {
    saveActiveScript();
    if (runningLua) stopSim();
    if (currentIs3D) {
        scene.saveToFile(currentProjectDir + "/scene.json");
        prePlaySceneJson = scene.toJson();
    }
    char *err = nullptr;
    g_obsidianProj = obsidian_project_open(currentProjectDir.c_str(), &err);
    if (!g_obsidianProj) {
        appendOutput(std::string("Runtime error: ") + (err ? err : "failed"), Col::red());
        if (err) obsidian_free(err);
        return;
    }
    if (err) obsidian_free(err);
    runningLua = true;
    showProjectsTab = false;
    activeScript.clear();
    last2DFrameJson.clear();
    syncOrientation();
    computeLayout();
    obsidian_project_set_viewport(g_obsidianProj, (int)std::max(64.0f, rViewport.w), (int)std::max(64.0f, rViewport.h));
    appendOutput(currentIs3D ? "3D Scene running (Obsidian Core)." : "2D Canvas running (Obsidronium).", Col::green());
}

void StudioApp::stopSim() {
    if (g_obsidianProj) {
        obsidian_project_close(g_obsidianProj);
        g_obsidianProj = nullptr;
    }
    bool wasRun = runningLua;
    runningLua = false;
    if (currentIs3D) {
        last2DFrameJson.clear();
        if (!prePlaySceneJson.empty()) {
            JsonVal j = MiniJson::parse(prePlaySceneJson);
            if (j.type == JsonVal::J_OBJ && j.get("root")) {
                std::ofstream out(currentProjectDir + "/scene.json", std::ios::binary | std::ios::trunc);
                if (out) out << prePlaySceneJson;
                scene = StudioScene::loadOrCreate(currentProjectDir);
            }
            prePlaySceneJson.clear();
        }
    }
    syncOrientation();
    computeLayout();
    if (wasRun) appendOutput("Stopped.", Col::muted());
}

void StudioApp::syncFromLua() {
    if (!g_obsidianProj || !currentIs3D) return;
    char *raw = obsidian_project_exec(g_obsidianProj, "return sdk and sdk.sync() or nil");
    if (!raw) return;
    JsonVal res = MiniJson::parse(raw);
    obsidian_free(raw);
    std::string out = res.s("output");
    if (!out.empty()) appendOutput(out, Col::text());
    std::string err = res.s("error");
    if (!err.empty()) appendOutput(err, Col::red());
    std::string val = res.s("result");
    if (val.empty() || val[0] != '[') return;
    JsonVal arr = MiniJson::parse(val);
    if (arr.type != JsonVal::J_ARR) return;
    for (const auto &it : arr.arr) {
        std::string id = it.s("id");
        if (auto *n = scene.findById(id)) {
            n->posX = (float)it.d("px", n->posX);
            n->posY = (float)it.d("py", n->posY);
            n->posZ = (float)it.d("pz", n->posZ);
            n->sizeX = (float)it.d("sx", n->sizeX);
            n->sizeY = (float)it.d("sy", n->sizeY);
            n->sizeZ = (float)it.d("sz", n->sizeZ);
            n->rotX = (float)it.d("rx", n->rotX);
            n->rotY = (float)it.d("ry", n->rotY);
            n->rotZ = (float)it.d("rz", n->rotZ);
            if (it.get("color")) n->argbColor = (uint32_t)(int64_t)it.d("color", (double)(int32_t)n->argbColor);
        }
    }
}

void StudioApp::execCommand(const std::string &cmd) {
    if (cmd.empty()) return;
    appendOutput("> " + cmd, Col::accent());
    if (!g_obsidianProj) {
        char *err = nullptr;
        g_obsidianProj = obsidian_project_open(currentProjectDir.c_str(), &err);
        if (!g_obsidianProj) {
            appendOutput(err ? err : "failed to open Obsidronium runtime", Col::red());
            if (err) obsidian_free(err);
            return;
        }
        if (err) obsidian_free(err);
    }
    char *raw = obsidian_project_exec(g_obsidianProj, cmd.c_str());
    if (!raw) return;
    JsonVal res = MiniJson::parse(raw);
    obsidian_free(raw);
    std::string out = res.s("output");
    if (!out.empty()) appendOutput(out, Col::text());
    std::string val = res.s("result");
    if (!val.empty() && val != "nil") appendOutput("= " + val, Col::green());
    std::string err = res.s("error");
    if (!err.empty()) appendOutput(err, Col::red());
    syncFromLua();
}

void StudioApp::step(float dt) {
    gltfLiveAnimTime += dt;
    if (runningLua && g_obsidianProj) {
        obsidian_project_set_viewport(g_obsidianProj, (int)std::max(64.0f, rViewport.w), (int)std::max(64.0f, rViewport.h));
        char *raw = obsidian_project_step(g_obsidianProj, dt);
        if (raw) {
            last2DFrameJson = raw;
            JsonVal res = MiniJson::parse(raw);
            obsidian_free(raw);
            std::string out = res.s("output");
            if (!out.empty()) appendOutput(out, Col::text());
            std::string err = res.s("error");
            if (!err.empty()) {
                appendOutput(err, Col::red());
                stopSim();
                return;
            }
            if (currentIs3D) syncFromLua();
        }
    } else if (!runningLua && animPlaying) {
        SceneNode *target = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId);
        if (target && target->className == "GltfModel" && !target->modelPath.empty()) {
            if (GltfModelAsset *gm = getOrLoadGltf(target->modelPath);
                gm && target->modelAnimIndex >= 0 && target->modelAnimIndex < (int)gm->animations.size()) {
                animTime += dt;
                float dur = std::max(0.1f, gm->animations[(size_t)target->modelAnimIndex].duration);
                if (animTime >= dur) {
                    if (target->animLoop) animTime = std::fmod(animTime, dur);
                    else { animTime = dur; animPlaying = false; }
                }
            } else {
                animPlaying = false;
            }
        } else if (!activeAnim.frames.empty()) {
            animTime += dt;
            float dur = activeAnim.duration();
            if (animTime >= dur) {
                if (activeAnim.loop) animTime = std::fmod(animTime, dur);
                else { animTime = dur; animPlaying = false; }
            }
            if (target && target->is3DNode()) {
                activeAnim.sample(animTime, *target);
            }
        }
    }
}

// =========================================================================
// Layout (responsive in both Portrait & Landscape!)
// =========================================================================
Rect StudioApp::closePlayBtnRect() const {
    if (playerOnly) return {0, 0, 0, 0};
    float sz = std::round(32.0f * density);
    float pad = std::round(10.0f * density);
    return {std::max(pad, width - sz - pad), pad, sz, sz};
}

void StudioApp::computeLayout() {
    if (playerOnly || (runningLua && !showProjectsTab && activeScript.empty()) || (!showProjectsTab && !activeScript.empty())) {
        rTopTabs = rRibbon = rDocTabs = rRightDock = rExplorer = rProperties = rBottomDock = rOutLog = rCmdBar = {0, 0, 0, 0};
        rViewport = {0, 0, width, height};
        return;
    }
    bool landscape = width > height;
    float topTabsH = std::round(22.0f * density);
    float ribbonH  = std::round(32.0f * density);
    float docTabsH = std::round(20.0f * density);
    float rightW   = (!showProjectsTab && (showExplorer || showProperties))
        ? std::min(std::round(146.0f * density), std::round(width * (landscape ? 0.24f : 0.36f)))
        : 0.0f;
    float botH     = (showAnimator && !showProjectsTab && activeScript.empty())
        ? std::min(std::round(148.0f * density), std::round(height * (landscape ? 0.38f : 0.28f)))
        : (showOutput
            ? std::min(std::round(64.0f * density), std::round(height * (landscape ? 0.22f : 0.16f)))
            : std::round(22.0f * density));

    rTopTabs    = {0, 0, width, topTabsH};
    rRibbon     = {0, topTabsH, width, ribbonH};
    float midTop = topTabsH + ribbonH;
    float midH   = std::max(60.0f, height - midTop - botH);
    float leftW  = std::max(100.0f, width - rightW);

    rDocTabs    = {0, midTop, leftW, docTabsH};
    rViewport   = {0, midTop + docTabsH, leftW, std::max(50.0f, midH - docTabsH)};
    rRightDock  = {leftW, midTop, rightW, midH};

    if (rightW > 1.0f && showExplorer && showProperties) {
        float expH = std::round(midH * 0.48f);
        rExplorer   = {leftW, midTop, rightW, expH};
        rProperties = {leftW, midTop + expH, rightW, midH - expH};
    } else if (rightW > 1.0f && showExplorer) {
        rExplorer   = rRightDock;
        rProperties = {0, 0, 0, 0};
    } else if (rightW > 1.0f && showProperties) {
        rExplorer   = {0, 0, 0, 0};
        rProperties = rRightDock;
    } else {
        rExplorer = rProperties = {0, 0, 0, 0};
    }

    rBottomDock = {0, height - botH, width, botH};
    float cmdH = std::round(22.0f * density);
    float hdrH = std::round(14.0f * density);
    if (showAnimator && !showProjectsTab && activeScript.empty()) {
        rOutLog = {0, 0, 0, 0};
        rCmdBar = {0, 0, 0, 0};
    } else if (showOutput) {
        rOutLog = {0, rBottomDock.y + hdrH, width, std::max(8.0f, botH - hdrH - cmdH)};
        rCmdBar = {0, height - cmdH, width, cmdH};
    } else {
        rOutLog = {0, 0, 0, 0};
        rCmdBar = rBottomDock;
    }
}

void StudioApp::pushBatch(bool scissor, Rect sc) {
    DrawBatch b;
    b.hasScissor = scissor;
    b.scissor = sc;
    uiBatches.push_back(std::move(b));
    hasClipRect = scissor;
    currentClipRect = sc;
}

void StudioApp::addHitBox(Rect r, std::function<void()> cb) {
    if (hasClipRect) {
        float x0 = std::max(r.x, currentClipRect.x);
        float y0 = std::max(r.y, currentClipRect.y);
        float x1 = std::min(r.x + r.w, currentClipRect.x + currentClipRect.w);
        float y1 = std::min(r.y + r.h, currentClipRect.y + currentClipRect.h);
        if (x1 <= x0 || y1 <= y0) return;
        r = {x0, y0, x1 - x0, y1 - y0};
    }
    hitBoxes.push_back({r, std::move(cb)});
}

void StudioApp::emitRect(float x, float y, float w, float h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    UiVertex a{x, y, u, u, col}, b{x + w, y, u, u, col}, c{x + w, y + h, u, u, col}, d{x, y + h, u, u, col};
    v.push_back(a); v.push_back(b); v.push_back(c);
    v.push_back(a); v.push_back(c); v.push_back(d);
}

void StudioApp::emitRectOutline(float x, float y, float w, float h, float t, uint32_t col) {
    emitRect(x, y, w, t, col);
    emitRect(x, y + h - t, w, t, col);
    emitRect(x, y, t, h, col);
    emitRect(x + w - t, y, t, h, col);
}

void StudioApp::emitRoundRect(float x, float y, float w, float h, float rad, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    float r = std::max(0.0f, std::min(rad, std::min(w, h) * 0.5f));
    if (r < 1.0f) { emitRect(x, y, w, h, col); return; }
    emitRect(x + r, y, w - 2.0f * r, h, col);
    emitRect(x, y + r, r, h - 2.0f * r, col);
    emitRect(x + w - r, y + r, r, h - 2.0f * r, col);
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    auto corner = [&](float cx, float cy, float aStart) {
        const int segs = 6;
        for (int i = 0; i < segs; ++i) {
            float a0 = aStart + (1.5707963f * i) / segs;
            float a1 = aStart + (1.5707963f * (i + 1)) / segs;
            v.push_back({cx, cy, u, u, col});
            v.push_back({cx + std::cos(a0) * r, cy + std::sin(a0) * r, u, u, col});
            v.push_back({cx + std::cos(a1) * r, cy + std::sin(a1) * r, u, u, col});
        }
    };
    corner(x + w - r, y + h - r, 0.0f);
    corner(x + r,     y + h - r, 1.5707963f);
    corner(x + r,     y + r,     3.14159265f);
    corner(x + w - r, y + r,     4.71238898f);
}

void StudioApp::emitLine2D(float x0, float y0, float x1, float y1, float thick, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::hypot(dx, dy);
    if (len < 0.5f) return;
    float ht = std::max(0.5f, thick * 0.5f);
    float nx = (-dy / len) * ht, ny = (dx / len) * ht;
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    UiVertex a{x0 + nx, y0 + ny, u, u, col}, b{x1 + nx, y1 + ny, u, u, col};
    UiVertex c{x1 - nx, y1 - ny, u, u, col}, d{x0 - nx, y0 - ny, u, u, col};
    v.push_back(a); v.push_back(b); v.push_back(c);
    v.push_back(a); v.push_back(c); v.push_back(d);
}

void StudioApp::emitCircle(float cx, float cy, float r, uint32_t col) {
    if (r <= 0.0f) return;
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    const int segs = std::clamp(circleSegments, 6, 64);
    for (int i = 0; i < segs; ++i) {
        float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        v.push_back({cx, cy, u, u, col});
        v.push_back({cx + std::cos(a0) * r, cy + std::sin(a0) * r, u, u, col});
        v.push_back({cx + std::cos(a1) * r, cy + std::sin(a1) * r, u, u, col});
    }
}

void StudioApp::emitCircleOutline(float cx, float cy, float r, float thick, uint32_t col) {
    if (r <= 0.0f) return;
    float rIn = std::max(0.5f, r - std::max(1.0f, thick));
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    const int segs = std::clamp(circleSegments, 6, 64);
    for (int i = 0; i < segs; ++i) {
        float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        UiVertex o0{cx + c0 * r, cy + s0 * r, u, u, col};
        UiVertex o1{cx + c1 * r, cy + s1 * r, u, u, col};
        UiVertex i1{cx + c1 * rIn, cy + s1 * rIn, u, u, col};
        UiVertex i0{cx + c0 * rIn, cy + s0 * rIn, u, u, col};
        v.push_back(o0); v.push_back(o1); v.push_back(i1);
        v.push_back(o0); v.push_back(i1); v.push_back(i0);
    }
}

void StudioApp::emitText(const std::string &txt, float x, float y, float pxSize, uint32_t col, float maxW) {
    if (txt.empty()) return;
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float s = pxSize / font.basePixelHeight;
    float penX = x;
    const char *p = txt.data(), *end = p + txt.size();
    while (p < end) {
        uint32_t cp = StudioFont::nextUtf8(p, end);
        auto it = font.glyphs.find(cp);
        if (it == font.glyphs.end()) it = font.glyphs.find('?');
        if (it == font.glyphs.end()) { penX += pxSize * 0.5f; continue; }
        const FontGlyph &g = it->second;
        if (maxW > 0.0f && penX + g.advance * s - x > maxW) break;
        if (g.x1 > g.x0 && g.y1 > g.y0) {
            float x0 = penX + g.x0 * s, y0 = y + g.y0 * s;
            float x1 = penX + g.x1 * s, y1 = y + g.y1 * s;
            UiVertex a{x0, y0, g.u0, g.v0, col}, b{x1, y0, g.u1, g.v0, col};
            UiVertex c{x1, y1, g.u1, g.v1, col}, d{x0, y1, g.u0, g.v1, col};
            v.push_back(a); v.push_back(b); v.push_back(c);
            v.push_back(a); v.push_back(c); v.push_back(d);
        }
        penX += g.advance * s;
    }
}

void StudioApp::emitTextCentered(const std::string &txt, const Rect &r, float pxSize, uint32_t col) {
    float w = font.measureWidth(txt, pxSize);
    float x = r.x + std::max(2.0f, (r.w - w) * 0.5f);
    float y = r.y + (r.h - pxSize * 1.15f) * 0.5f;
    emitText(txt, x, y, pxSize, col, r.w - 4.0f);
}

void StudioApp::addBtn(const Rect &r, const std::string &label, uint32_t bg, uint32_t fg, float pxSize, std::function<void()> cb) {
    emitRect(r.x, r.y, r.w, r.h, bg);
    emitRectOutline(r.x, r.y, r.w, r.h, 1.0f, Col::border());
    emitTextCentered(label, r, pxSize, fg);
    addHitBox(r, std::move(cb));
}

static uint32_t hsvToUiCol(float h, float s, float v) {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    float r = v, g = v, b = v;
    if (s > 1e-4f) {
        float hh = h * 6.0f;
        int i = (int)std::floor(hh) % 6;
        float f = hh - std::floor(hh);
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * f);
        float t = v * (1.0f - s * (1.0f - f));
        if (i == 0)      { r = v; g = t; b = p; }
        else if (i == 1) { r = q; g = v; b = p; }
        else if (i == 2) { r = p; g = v; b = t; }
        else if (i == 3) { r = p; g = q; b = v; }
        else if (i == 4) { r = t; g = p; b = v; }
        else             { r = v; g = p; b = q; }
    }
    return StudioFont::rgba((uint8_t)std::lround(r * 255.0f),
                            (uint8_t)std::lround(g * 255.0f),
                            (uint8_t)std::lround(b * 255.0f), 255);
}

static std::string fmtHexColor(uint32_t argb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06X", (unsigned)(argb & 0xFFFFFFu));
    return buf;
}

static bool parseHexColor(const std::string &in, uint32_t &outArgb) {
    std::string s;
    for (char ch : in) {
        if (ch == '#' || ch == ' ') continue;
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) s.push_back(ch);
        else return false;
    }
    if (s.size() != 6) return false;
    uint32_t rgb = (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
    outArgb = 0xFF000000u | rgb;
    return true;
}

void StudioApp::syncWheelFromArgb(uint32_t argb) {
    float r = ((argb >> 16) & 0xFF) / 255.0f;
    float g = ((argb >> 8) & 0xFF) / 255.0f;
    float b = (argb & 0xFF) / 255.0f;
    float mx = std::max({r, g, b});
    float mn = std::min({r, g, b});
    float d = mx - mn;
    wheelV = mx;
    wheelS = (mx > 1e-4f) ? (d / mx) : 0.0f;
    if (d < 1e-4f) {
        return; // Keep previous hue when picking pure white/gray/black
    }
    float h = 0.0f;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    wheelH = h / 6.0f;
}

uint32_t StudioApp::wheelArgb() const {
    uint32_t uiCol = hsvToUiCol(wheelH, wheelS, wheelV);
    uint8_t r = (uint8_t)(uiCol & 0xFF);
    uint8_t g = (uint8_t)((uiCol >> 8) & 0xFF);
    uint8_t b = (uint8_t)((uiCol >> 16) & 0xFF);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void StudioApp::applyWheelColor(bool saveScene) {
    uint32_t argb = wheelArgb();
    if (modal == ModalKind::SKY || promptTargetKey == "SkyTop" || promptTargetKey == "SkyBot") {
        if (auto *env = scene.findEnvironment()) {
            env->skyPreset = "Custom";
            bool editBot = (modal == ModalKind::SKY) ? skyEditBot : (promptTargetKey == "SkyBot");
            if (editBot) env->skyBotColor = argb;
            else env->skyTopColor = argb;
            if (saveScene) scene.saveToFile(currentProjectDir + "/scene.json");
        }
        return;
    }
    if (auto *n = selectedNode()) {
        n->argbColor = argb;
        if (saveScene) scene.saveToFile(currentProjectDir + "/scene.json");
    }
}

static uint32_t lerpArgb(uint32_t a, uint32_t b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [&](int shift) -> uint32_t {
        float ca = (float)((a >> shift) & 0xFF);
        float cb = (float)((b >> shift) & 0xFF);
        return (uint32_t)std::clamp((int)std::lround(ca + (cb - ca) * t), 0, 255) << shift;
    };
    return 0xFF000000u | mix(16) | mix(8) | mix(0);
}

void StudioApp::applyClockTimeToEnv(SceneNode *env, bool updateSkyColors) {
    if (!env) return;
    float ct = std::fmod(std::max(0.0f, env->clockTime), 24.0f);
    env->clockTime = ct;
    if (!updateSkyColors) return;
    if (env->skyPreset == "Space" || env->skyPreset == "Overcast") return;

    const uint32_t nightTop = 0xFF070D1A, nightBot = 0xFF172542;
    const uint32_t dawnTop  = 0xFF28375E, dawnBot  = 0xFFF59E0B;
    const uint32_t dayTop   = 0xFF1E6EC4, dayBot   = 0xFF8CC4F2;
    const uint32_t duskTop  = 0xFF4A2559, duskBot  = 0xFFF97316;

    if (ct < 5.0f || ct >= 20.0f) {
        env->skyTopColor = nightTop; env->skyBotColor = nightBot;
        env->skyPreset = "Night";
    } else if (ct < 6.5f) {
        float u = (ct - 5.0f) / 1.5f;
        env->skyTopColor = lerpArgb(nightTop, dawnTop, u);
        env->skyBotColor = lerpArgb(nightBot, dawnBot, u);
        env->skyPreset = "Dawn";
    } else if (ct < 8.0f) {
        float u = (ct - 6.5f) / 1.5f;
        env->skyTopColor = lerpArgb(dawnTop, dayTop, u);
        env->skyBotColor = lerpArgb(dawnBot, dayBot, u);
        env->skyPreset = "Day";
    } else if (ct < 17.0f) {
        env->skyTopColor = dayTop; env->skyBotColor = dayBot;
        env->skyPreset = "Day";
    } else if (ct < 18.5f) {
        float u = (ct - 17.0f) / 1.5f;
        env->skyTopColor = lerpArgb(dayTop, duskTop, u);
        env->skyBotColor = lerpArgb(dayBot, duskBot, u);
        env->skyPreset = "Sunset";
    } else {
        float u = (ct - 18.5f) / 1.5f;
        env->skyTopColor = lerpArgb(duskTop, nightTop, u);
        env->skyBotColor = lerpArgb(duskBot, nightBot, u);
        env->skyPreset = "Night";
    }
}

std::vector<std::string> StudioApp::listAnimationFiles() const {
    std::vector<std::string> out;
    if (currentProjectDir.empty()) return out;
    std::string dirPath = currentProjectDir + "/animations";
    DIR *d = opendir(dirPath.c_str());
    if (!d) return out;
    while (struct dirent *ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() > 10 && name.substr(name.size() - 10) == ".anim.json") {
            out.push_back("animations/" + name);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> StudioApp::listScriptFiles() const {
    std::vector<std::string> out;
    auto pushUnique = [&](const std::string &p) {
        if (p.empty()) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (!currentProjectDir.empty()) {
        std::ifstream fobs(currentProjectDir + "/main.obs");
        if (fobs.good()) pushUnique("main.obs");
        std::ifstream flua(currentProjectDir + "/main.lua");
        if (flua.good()) pushUnique("main.lua");
    }
    if (out.empty()) pushUnique("main.obs");
    std::function<void(const SceneNode &)> walk = [&](const SceneNode &nd) {
        if (nd.isScript() && !nd.scriptPath.empty()) pushUnique(nd.scriptPath);
        for (const auto &c : nd.children) walk(c);
    };
    walk(scene.root);
    if (!currentProjectDir.empty()) {
        std::string sdir = currentProjectDir + "/scripts";
        if (DIR *d = opendir(sdir.c_str())) {
            std::vector<std::string> disk;
            while (struct dirent *ent = readdir(d)) {
                std::string fn = ent->d_name;
                bool isObs = fn.size() > 4 && fn.substr(fn.size() - 4) == ".obs";
                bool isLua = fn.size() > 4 && fn.substr(fn.size() - 4) == ".lua";
                if ((isObs || isLua) && fn != "obsidian_sdk.obs" && fn != "studio_sdk.lua") {
                    disk.push_back("scripts/" + fn);
                }
            }
            closedir(d);
            std::sort(disk.begin(), disk.end());
            for (const auto &p : disk) pushUnique(p);
        }
    }
    return out;
}

void StudioApp::openAnimator(SceneNode *target) {
    if (target && target->is3DNode()) animTargetNodeId = target->id;
    else if (auto *s = selectedNode(); s && s->is3DNode()) animTargetNodeId = s->id;
    else animTargetNodeId.clear();

    if (!currentProjectDir.empty()) {
        std::string dirPath = currentProjectDir + "/animations";
        mkdir(dirPath.c_str(), 0755);
    }

    auto files = listAnimationFiles();
    SceneNode *tn = animTargetNodeId.empty() ? nullptr : scene.findById(animTargetNodeId);
    if (tn && !tn->animPath.empty()) {
        activeAnimFile = tn->animPath;
    } else if (activeAnimFile.empty() && !files.empty()) {
        activeAnimFile = files[0];
    }

    if (!activeAnimFile.empty()) {
        AnimClip loaded;
        if (AnimClip::loadFromFile(currentProjectDir + "/" + activeAnimFile, loaded)) {
            activeAnim = std::move(loaded);
        } else {
            activeAnim = AnimClip{};
            activeAnim.name = activeAnimFile;
        }
    }
    animPlaying = false;
    animTime = 0.0f;
    showAnimator = true;
    modal = ModalKind::NONE;
}

void StudioApp::emitColorWheel(float cx, float cy, float rad, const Rect &valBar) {
    rWheel = {cx - rad, cy - rad, rad * 2.0f, rad * 2.0f};
    rValBar = valBar;
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;

    // Outer border ring
    emitCircle(cx, cy, rad + 1.5f * density, Col::border());

    // Smooth HSV disc: center = (0, 0, V), rim = (H, 1, V). Linear RGB interp = exact HSV!
    uint32_t cCenter = hsvToUiCol(0.0f, 0.0f, wheelV);
    const int segs = 60;
    for (int i = 0; i < segs; ++i) {
        float h0 = (float)i / (float)segs;
        float h1 = (float)(i + 1) / (float)segs;
        float a0 = h0 * 6.2831853f;
        float a1 = h1 * 6.2831853f;
        uint32_t c0 = hsvToUiCol(h0, 1.0f, wheelV);
        uint32_t c1 = hsvToUiCol(h1, 1.0f, wheelV);
        v.push_back({cx, cy, u, u, cCenter});
        v.push_back({cx + std::cos(a0) * rad, cy + std::sin(a0) * rad, u, u, c0});
        v.push_back({cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, u, u, c1});
    }

    // Current (H, S) marker on wheel
    float ang = wheelH * 6.2831853f;
    float mx = cx + std::cos(ang) * wheelS * rad;
    float my = cy + std::sin(ang) * wheelS * rad;
    emitCircle(mx, my, 5.5f * density, StudioFont::rgba(0, 0, 0, 220));
    emitCircle(mx, my, 3.8f * density, hsvToUiCol(wheelH, wheelS, wheelV));
    emitCircleOutline(mx, my, 5.5f * density, 1.3f * density, StudioFont::rgba(255, 255, 255, 255));

    // Vertical Brightness (V) bar: top = full hue/sat at V=1, bottom = black (V=0)
    uint32_t topCol = hsvToUiCol(wheelH, wheelS, 1.0f);
    uint32_t botCol = StudioFont::rgba(0, 0, 0, 255);
    UiVertex b0{valBar.x, valBar.y, u, u, topCol};
    UiVertex b1{valBar.x + valBar.w, valBar.y, u, u, topCol};
    UiVertex b2{valBar.x + valBar.w, valBar.y + valBar.h, u, u, botCol};
    UiVertex b3{valBar.x, valBar.y + valBar.h, u, u, botCol};
    v.push_back(b0); v.push_back(b1); v.push_back(b2);
    v.push_back(b0); v.push_back(b2); v.push_back(b3);
    emitRectOutline(valBar.x, valBar.y, valBar.w, valBar.h, 1.0f, Col::border());

    // Brightness slider knob
    float vy = valBar.y + (1.0f - std::clamp(wheelV, 0.0f, 1.0f)) * valBar.h;
    emitRect(valBar.x - 2.0f * density, vy - 2.5f * density, valBar.w + 4.0f * density, 5.0f * density,
             StudioFont::rgba(255, 255, 255, 255));
    emitRectOutline(valBar.x - 2.0f * density, vy - 2.5f * density, valBar.w + 4.0f * density, 5.0f * density,
                    1.0f, StudioFont::rgba(20, 20, 20, 255));
}

// =========================================================================
// Frame Build
// =========================================================================
void StudioApp::buildFrame(float w, float h, float d) {
    width = w; height = h; density = std::max(1.5f, d);
    syncOrientation();
    computeLayout();
    uiBatches.clear();
    hitBoxes.clear();
    hasClipRect = false;
    frame3DMesh.clear();
    frame3DShadowCasters.clear();
    frame3DLines.clear();
    frame3DOverlayLines.clear();
    gizmoTips.clear();

    has3DViewportVisible = (!showProjectsTab && activeScript.empty() && currentIs3D);

    if (!has3DViewportVisible) {
        pushBatch(false, {});
        emitRect(0, 0, width, height, Col::bg());
    }

    bool isPlayingFull = (runningLua && !showProjectsTab && activeScript.empty());
    bool isCodeEditorMode = (!showProjectsTab && !activeScript.empty());

    if (showProjectsTab) {
        buildProjectsTab(rViewport);
    } else if (isCodeEditorMode) {
        buildCodeEditor(rViewport);
        if (modal != ModalKind::NONE) buildModal();
        return;
    } else if (currentIs3D) {
        build3DScene();
        pushBatch(true, rViewport);
        std::vector<const SceneNode *> uiNodes;
        scene.collectUi(uiNodes);
        for (const auto *u : uiNodes) {
            if (u->className == "TextLabel") {
                emitText(u->text, rViewport.x + u->posX * density, rViewport.y + u->posY * density,
                         std::max(10.0f, u->textSize * 0.65f * density), Col::argbToUi(u->argbColor));
            }
        }
        if (runningLua) renderLuaDrawOverlay(rViewport, false);
        if (!isPlayingFull) {
            std::string badge = "3D";
            float bw = font.measureWidth(badge, 10.0f * density) + 12.0f * density;
            Rect rb{rViewport.x + rViewport.w - bw - 6 * density, rViewport.y + 6 * density, bw, 16 * density};
            emitRect(rb.x, rb.y, rb.w, rb.h, StudioFont::rgba(24, 24, 24, 200));
            emitTextCentered(badge, rb, 10.0f * density, Col::accent());
        }
    } else {
        build2DScene(rViewport);
    }

    if (isPlayingFull || playerOnly) {
        if (!playerOnly) {
            pushBatch(false, {});
            Rect rClose = closePlayBtnRect();
            emitRect(rClose.x, rClose.y, rClose.w, rClose.h, StudioFont::rgba(24, 24, 24, 225));
            emitRectOutline(rClose.x, rClose.y, rClose.w, rClose.h, 1.5f * density, StudioFont::rgba(220, 220, 220, 235));
            emitTextCentered("X", rClose, 13.5f * density, StudioFont::rgba(255, 255, 255, 255));
            addHitBox(rClose, [this]() { stopSim(); });
        }
        return;
    }

    buildTopRibbon();
    buildDocTabs();
    buildRightDock();
    buildBottomDock();

    if (modal != ModalKind::NONE) buildModal();
}

void StudioApp::buildProjectsTab(const Rect &area) {
    pushBatch(true, area);
    emitRect(area.x, area.y, area.w, area.h, Col::bg());
    float pad = 10.0f * density;
    float fs = 10.0f * density;

    emitText("Projects", area.x + pad, area.y + pad, 13.0f * density, Col::text());
    Rect bNew{area.x + pad, area.y + 32 * density, 128 * density, 26 * density};
    addBtn(bNew, "+ New Project", Col::btnAct(), Col::text(), fs, [this]() {
        modal = ModalKind::NEW_PROJECT;
    });

    Rect listArea{area.x, area.y + 66 * density, area.w, std::max(40.0f, area.h - 66 * density)};
    pushBatch(true, listArea);
    auto list = callbacks.listProjects ? callbacks.listProjects() : std::vector<std::pair<std::string, bool>>{};
    float rowH = 34.0f * density;
    float maxScroll = std::max(0.0f, list.size() * rowH - listArea.h);
    projectsScrollY = std::clamp(projectsScrollY, 0.0f, maxScroll);
    float y = listArea.y - projectsScrollY;

    for (const auto &pr : list) {
        if (y + rowH >= listArea.y && y <= listArea.y + listArea.h) {
            std::string pname = pr.first;
            bool is3d = pr.second;
            bool cur = (pname == currentProjectName);

            float actW = 54.0f * density;
            float openW = std::max(100.0f, listArea.w - pad * 2 - actW * 2 - 8 * density);
            Rect rOpen{listArea.x + pad, y + 3 * density, openW, 28 * density};
            addBtn(rOpen, pname + (is3d ? "  [3D]" : "  [2D]"), cur ? Col::selRow() : Col::panel(), Col::text(), fs, [this, pname, is3d]() {
                openProject(pname, is3d);
            });

            Rect rRen{rOpen.x + rOpen.w + 4 * density, rOpen.y, actW, 28 * density};
            addBtn(rRen, "Rename", Col::btn(), Col::text(), 8.5f * density, [this, pname]() {
                promptTitle = "Rename Project"; promptValue = pname; promptExtra = pname; promptTargetKey = "RenameProj";
                modal = ModalKind::TEXT_PROMPT;
            });

            Rect rDel{rRen.x + rRen.w + 4 * density, rOpen.y, actW, 28 * density};
            addBtn(rDel, "Del", Col::btn(), cur ? Col::muted() : Col::red(), 8.5f * density, [this, pname]() {
                if (callbacks.deleteProject && pname != currentProjectName) {
                    callbacks.deleteProject(pname);
                }
            });
        }
        y += rowH;
    }
}

void StudioApp::build3DScene() {
    const auto *env = scene.environment();
    float ct = env ? env->clockTime : 14.0f;
    std::string sp = env ? env->skyPreset : "Day";
    uint32_t topCol = env ? env->skyTopColor : 0xFF1E6EC4;
    uint32_t botCol = env ? env->skyBotColor : 0xFF8CC4F2;
    auto argbToF = [](uint32_t c, float out[4]) {
        out[0] = ((c >> 16) & 0xFF) / 255.0f;
        out[1] = ((c >> 8)  & 0xFF) / 255.0f;
        out[2] = ( c        & 0xFF) / 255.0f;
        out[3] = 1.0f;
    };
    argbToF(topCol, skyTop);
    argbToF(botCol, skyBot);

    if (sp == "Space") {
        skyTint[0] = 0.56f; skyTint[1] = 0.65f; skyTint[2] = 0.90f; skyTint[3] = 0.24f;
    } else if (sp == "Overcast") {
        skyTint[0] = 0.88f; skyTint[1] = 0.91f; skyTint[2] = 0.96f; skyTint[3] = 0.48f;
    } else if (ct < 5.5f || ct >= 19.5f) {
        skyTint[0] = 0.60f; skyTint[1] = 0.70f; skyTint[2] = 0.96f; skyTint[3] = 0.26f;
    } else if ((ct >= 5.5f && ct < 7.5f) || (ct >= 17.0f && ct < 19.5f)) {
        skyTint[0] = 1.08f; skyTint[1] = 0.86f; skyTint[2] = 0.72f; skyTint[3] = 0.36f;
    } else {
        skyTint[0] = 1.0f; skyTint[1] = 1.0f; skyTint[2] = 1.0f; skyTint[3] = 0.42f;
    }

    // Sun direction follows ClockTime (modulated by DirectionalLight3D azimuth if present)
    std::vector<const SceneNode *> nodes3D;
    scene.collect3DNodes(nodes3D);
    float azX = 0.55f, azZ = 0.40f;
    for (const auto *n : nodes3D) {
        if (n->className == "DirectionalLight3D") {
            azX = n->posX; azZ = n->posZ;
            break;
        }
    }
    float azLen = std::max(0.01f, std::hypot(azX, azZ));
    float sunAngle = (ct - 6.0f) / 12.0f * 3.14159265f;
    float sunHoriz = std::cos(sunAngle);
    float sunElev  = std::sin(sunAngle);
    Vec3f sdir = (sunElev >= 0.08f)
        ? norm3({(azX / azLen) * sunHoriz, sunElev, (azZ / azLen) * sunHoriz})
        : norm3({-0.35f, 0.48f, -0.35f}); // Moon light at night
    sunDir[0] = sdir.x; sunDir[1] = sdir.y; sunDir[2] = sdir.z; sunDir[3] = 0.0f;

    // Camera matrices (uses active Camera3D during Play when enabled, else free orbit camera)
    float fovDeg = 53.0f;
    if (runningLua && useCamera3DInPlay) {
        if (const auto *cam = scene.findActiveCamera()) {
            fovDeg = std::clamp(cam->fov, 25.0f, 110.0f);
            eye = {cam->posX, cam->posY, cam->posZ};
            constexpr float kDeg = 3.14159265f / 180.0f;
            float rx = cam->rotX * kDeg, ry = cam->rotY * kDeg;
            fwd = norm3({-std::sin(ry) * std::cos(rx), std::sin(rx), -std::cos(ry) * std::cos(rx)});
            right = norm3(cross3(fwd, {0, 1, 0}));
            up = cross3(right, fwd);
        }
    } else {
        float cy = std::cos(camYaw), sy = std::sin(camYaw);
        float cp = std::cos(camPitch), sp = std::sin(camPitch);
        Vec3f target{camTargetX, camTargetY, camTargetZ};
        eye = {target.x + camDist * cp * sy, target.y + camDist * sp, target.z + camDist * cp * cy};
        fwd = norm3(target - eye);
        right = norm3(cross3(fwd, {0, 1, 0}));
        up = cross3(right, fwd);
    }

    float aspect = std::max(0.2f, rViewport.w / std::max(1.0f, rViewport.h));
    float fovY = fovDeg * (3.14159265f / 180.0f);
    float f = 1.0f / std::tan(fovY * 0.5f);
    focalLen = (rViewport.h * 0.5f) * f;
    float zn = 0.15f, zf = 2000.0f;

    float proj[16]{};
    proj[0] = f / aspect;
    proj[5] = -f;
    proj[10] = zf / (zn - zf);
    proj[11] = -1.0f;
    proj[14] = (zn * zf) / (zn - zf);

    float view[16]{};
    view[0] = right.x; view[4] = right.y; view[8]  = right.z; view[12] = -dot3(right, eye);
    view[1] = up.x;    view[5] = up.y;    view[9]  = up.z;    view[13] = -dot3(up, eye);
    view[2] = -fwd.x;  view[6] = -fwd.y;  view[10] = -fwd.z;  view[14] = dot3(fwd, eye);
    view[15] = 1.0f;
    mat4Mul(viewProj, proj, view);

    // Light view-projection for shadow map
    Vec3f lDir = norm3({sunDir[0], sunDir[1], sunDir[2]});
    Vec3f lFocus = (runningLua && useCamera3DInPlay)
        ? Vec3f{eye.x + fwd.x * 8.0f, 0.0f, eye.z + fwd.z * 8.0f}
        : Vec3f{camTargetX, 0.0f, camTargetZ};
    Vec3f lEye = lFocus + lDir * 45.0f;
    Vec3f lFwd = norm3(lFocus - lEye);
    Vec3f upHint = (std::fabs(lFwd.y) > 0.98f) ? Vec3f{0, 0, 1} : Vec3f{0, 1, 0};
    Vec3f lRight = norm3(cross3(lFwd, upHint));
    Vec3f lUp = cross3(lRight, lFwd);
    float S = 38.0f, lzn = 1.0f, lzf = 110.0f;
    float lProj[16]{};
    lProj[0] = 1.0f / S;
    lProj[5] = -1.0f / S;
    lProj[10] = 1.0f / (lzn - lzf);
    lProj[14] = lzn / (lzn - lzf);
    lProj[15] = 1.0f;
    float lView[16]{};
    lView[0] = lRight.x; lView[4] = lRight.y; lView[8]  = lRight.z; lView[12] = -dot3(lRight, lEye);
    lView[1] = lUp.x;    lView[5] = lUp.y;    lView[9]  = lUp.z;    lView[13] = -dot3(lUp, lEye);
    lView[2] = -lFwd.x;  lView[6] = -lFwd.y;  lView[10] = -lFwd.z;  lView[14] = dot3(lFwd, lEye);
    lView[15] = 1.0f;
    mat4Mul(lightVP, lProj, lView);

    // 3D solid meshes + shadow casters
    initTexAtlas();
    auto tileUv = [&](const std::string &texPath, float &u0, float &v0, float &u1, float &v1) {
        int tile = ensureTextureTile(texPath);
        int tx = tile % 4, ty = tile / 4;
        float pad = 2.0f / (float)kTexAtlasSize;
        u0 = tx * 0.25f + pad; u1 = (tx + 1) * 0.25f - pad;
        v0 = ty * 0.25f + pad; v1 = (ty + 1) * 0.25f - pad;
    };
    std::vector<const SceneNode *> meshes;
    scene.collectMeshes(meshes);
    for (const auto *p : meshes) {
        if (p->transparency >= 0.99f) continue;
        float u0 = 0, v0 = 0, u1 = 0.25f, v1 = 0.25f;
        tileUv(p->texturePath, u0, v0, u1, v1);
        if (p->className == "GltfModel" && !p->modelPath.empty()) {
            if (GltfModelAsset *gm = getOrLoadGltf(p->modelPath)) {
                int aIdx = -1;
                float aTime = 0.0f;
                if (showAnimator && (animTargetNodeId == p->id || (animTargetNodeId.empty() && selectedId == p->id))) {
                    aIdx = p->modelAnimIndex;
                    aTime = animTime;
                } else if (p->animAutoPlay && p->modelAnimIndex >= 0 && p->modelAnimIndex < (int)gm->animations.size()) {
                    aIdx = p->modelAnimIndex;
                    aTime = gltfLiveAnimTime;
                }
                gm->appendMesh(*p, aIdx, aTime, frame3DMesh, u0, v0, u1, v1, tileUv);
                if (showShadows && p->castShadow) {
                    gm->appendMesh(*p, aIdx, aTime, frame3DShadowCasters, u0, v0, u1, v1, nullptr);
                }
                continue;
            }
        }
        StudioScene::appendPartMesh(*p, frame3DMesh, u0, v0, u1, v1);
        if (showShadows && p->castShadow) {
            StudioScene::appendPartMesh(*p, frame3DShadowCasters);
        }
    }

    auto addLine = [&](std::vector<MeshVertex> &dst, Vec3f a, Vec3f b, uint32_t col) {
        dst.push_back({a.x, a.y, a.z, 0, 1, 0, col, 0, 0});
        dst.push_back({b.x, b.y, b.z, 0, 1, 0, col, 0, 0});
    };
    auto addBoxWire = [&](std::vector<MeshVertex> &dst, const SceneNode &node, float pad, uint32_t col) {
        float hx = node.sizeX * 0.5f + pad, hy = node.sizeY * 0.5f + pad, hz = node.sizeZ * 0.5f + pad;
        Vec3f c[8] = {
            StudioScene::transformVert(node, -hx, -hy, -hz), StudioScene::transformVert(node,  hx, -hy, -hz),
            StudioScene::transformVert(node,  hx,  hy, -hz), StudioScene::transformVert(node, -hx,  hy, -hz),
            StudioScene::transformVert(node, -hx, -hy,  hz), StudioScene::transformVert(node,  hx, -hy,  hz),
            StudioScene::transformVert(node,  hx,  hy,  hz), StudioScene::transformVert(node, -hx,  hy,  hz)
        };
        int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for (auto &e : edges) addLine(dst, c[e[0]], c[e[1]], col);
    };

    if (showGrid) {
        uint32_t gCol = StudioFont::rgba(92, 92, 92, 130);
        for (int i = -40; i <= 40; i += 4) {
            addLine(frame3DLines, {(float)i, 0.02f, -40}, {(float)i, 0.02f, 40}, gCol);
            addLine(frame3DLines, {-40, 0.02f, (float)i}, {40, 0.02f, (float)i}, gCol);
        }
    }

    // Editor wireframes for Godot-style Collision / Area / Camera3D / Lights
    if (!runningLua) {
        for (const auto *n : nodes3D) {
            if (showColliders && (n->className == "CollisionBox" || n->className == "Area3D")) {
                uint32_t cc = (n->className == "Area3D") ? StudioFont::rgba(250, 204, 21, 185) : StudioFont::rgba(34, 211, 238, 185);
                addBoxWire(frame3DLines, *n, 0.03f, cc);
            } else if (showColliders && n->className == "CollisionSphere") {
                float r = std::max({n->sizeX, n->sizeY, n->sizeZ}) * 0.5f;
                uint32_t cc = StudioFont::rgba(34, 211, 238, 185);
                const int seg = 16;
                for (int i = 0; i < seg; ++i) {
                    float a0 = 6.283185f * i / seg, a1 = 6.283185f * (i + 1) / seg;
                    Vec3f p{n->posX, n->posY, n->posZ};
                    addLine(frame3DLines, {p.x + std::cos(a0)*r, p.y, p.z + std::sin(a0)*r}, {p.x + std::cos(a1)*r, p.y, p.z + std::sin(a1)*r}, cc);
                    addLine(frame3DLines, {p.x + std::cos(a0)*r, p.y + std::sin(a0)*r, p.z}, {p.x + std::cos(a1)*r, p.y + std::sin(a1)*r, p.z}, cc);
                }
            } else if (n->className == "Camera3D") {
                uint32_t camCol = StudioFont::rgba(168, 85, 247, 230);
                Vec3f o{n->posX, n->posY, n->posZ};
                Vec3f f0 = StudioScene::transformVert(*n, -1.0f,  0.65f, -2.0f);
                Vec3f f1 = StudioScene::transformVert(*n,  1.0f,  0.65f, -2.0f);
                Vec3f f2 = StudioScene::transformVert(*n,  1.0f, -0.65f, -2.0f);
                Vec3f f3 = StudioScene::transformVert(*n, -1.0f, -0.65f, -2.0f);
                addLine(frame3DLines, o, f0, camCol); addLine(frame3DLines, o, f1, camCol);
                addLine(frame3DLines, o, f2, camCol); addLine(frame3DLines, o, f3, camCol);
                addLine(frame3DLines, f0, f1, camCol); addLine(frame3DLines, f1, f2, camCol);
                addLine(frame3DLines, f2, f3, camCol); addLine(frame3DLines, f3, f0, camCol);
            } else if (n->className == "DirectionalLight3D" || n->className == "PointLight3D") {
                uint32_t lc = StudioFont::rgba(250, 204, 21, 240);
                Vec3f o{n->posX, n->posY, n->posZ};
                addLine(frame3DLines, {o.x - 0.8f, o.y, o.z}, {o.x + 0.8f, o.y, o.z}, lc);
                addLine(frame3DLines, {o.x, o.y - 0.8f, o.z}, {o.x, o.y + 0.8f, o.z}, lc);
                addLine(frame3DLines, {o.x, o.y, o.z - 0.8f}, {o.x, o.y, o.z + 0.8f}, lc);
            }
        }

        if (const auto *sel = selectedNode(); sel && (sel->is3DNode() || (sel->isGroup() && toolMode == ToolMode::MOVE))) {
            Vec3f orig{sel->posX, sel->posY, sel->posZ};
            float gScale = 2.0f;
            if (sel->is3DNode()) {
                addBoxWire(frame3DOverlayLines, *sel, 0.08f, StudioFont::rgba(56, 189, 248, 255));
                gScale = std::max({sel->sizeX, sel->sizeY, sel->sizeZ});
            } else {
                Vec3f sum{0, 0, 0}; int cnt = 0;
                std::function<void(const SceneNode &)> avg = [&](const SceneNode &nd) {
                    if (nd.is3DNode()) { sum = sum + Vec3f{nd.posX, nd.posY, nd.posZ}; cnt++; }
                    for (const auto &c : nd.children) avg(c);
                };
                avg(*sel);
                if (cnt > 0) orig = sum * (1.0f / (float)cnt);
            }
            if (toolMode != ToolMode::SELECT) {
                float gLen = std::max(3.0f, gScale * 0.65f + 1.6f);
                Vec3f tips[3] = {{orig.x + gLen, orig.y, orig.z}, {orig.x, orig.y + gLen, orig.z}, {orig.x, orig.y, orig.z + gLen}};
                uint32_t cols[3] = {StudioFont::rgba(239,68,68), StudioFont::rgba(34,197,94), StudioFont::rgba(59,130,246)};
                float bx = 0, by = 0;
                projectPoint(orig, bx, by);
                for (int ax = 0; ax < 3; ++ax) {
                    addLine(frame3DOverlayLines, orig, tips[ax], cols[ax]);
                    float sx = 0, sy = 0;
                    if (projectPoint(tips[ax], sx, sy)) gizmoTips.push_back({ax, sx, sy, bx, by});
                }
            }
        }
    }
}

void StudioApp::renderLuaDrawOverlay(const Rect &area, bool applyClearColor) {
    if (last2DFrameJson.empty()) return;
    auto fToU8 = [](double v) -> uint8_t {
        return (uint8_t)std::clamp((int)std::lround(v * 255.0), 0, 255);
    };
    JsonVal j = MiniJson::parse(last2DFrameJson);
    if (applyClearColor) {
        if (const JsonVal *cc = j.get("clear_color"); cc && cc->type == JsonVal::J_ARR && cc->arr.size() >= 3) {
            emitRect(area.x, area.y, area.w, area.h,
                     StudioFont::rgba(fToU8(cc->arr[0].num), fToU8(cc->arr[1].num), fToU8(cc->arr[2].num)));
        }
    }
    if (const JsonVal *cmds = j.get("draw_cmds"); cmds && cmds->type == JsonVal::J_ARR) {
        for (const auto &c : cmds->arr) {
            uint32_t col = StudioFont::rgba(255, 255, 255, 255);
            if (const JsonVal *ca = c.get("color"); ca && ca->type == JsonVal::J_ARR && ca->arr.size() >= 3) {
                uint8_t a = (ca->arr.size() >= 4) ? fToU8(ca->arr[3].num) : 255;
                col = StudioFont::rgba(fToU8(ca->arr[0].num), fToU8(ca->arr[1].num), fToU8(ca->arr[2].num), a);
            }
            const JsonVal *kv = c.get("kind");
            int kind = (kv && kv->type == JsonVal::J_NUM) ? (int)kv->num : -1;
            std::string ks = (kv && kv->type == JsonVal::J_STR) ? kv->str : "";
            float x = area.x + (float)c.d("x");
            float y = area.y + (float)c.d("y");
            float w = (float)c.d("w", 32.0);
            float h = (float)c.d("h", 32.0);
            float extra = (float)c.d("extra", 2.0);
            if (kind == 1 || ks == "rect") {
                emitRect(x, y, w, h, col);
            } else if (kind == 2 || ks == "circle") {
                float rad = (kv && kv->type == JsonVal::J_NUM) ? (w * 0.5f) : (float)c.d("radius", 6.0);
                emitCircle(x, y, rad, col);
            } else if (kind == 3 || ks == "text") {
                std::string txt = c.s("text");
                float sz = (kv && kv->type == JsonVal::J_NUM) ? w : (float)c.d("size", 18.0);
                int align = (int)c.d("align", 0);
                float tx = x;
                if (align == 1) tx -= font.measureWidth(txt, sz) * 0.5f;
                else if (align == 2) tx -= font.measureWidth(txt, sz);
                emitText(txt, tx, y, sz, col);
            } else if (kind == 4) {
                emitLine2D(x, y, area.x + w, area.y + h, extra, col);
            } else if (kind == 5) {
                emitRectOutline(x, y, w, h, extra, col);
            } else if (kind == 6) {
                emitCircleOutline(x, y, w * 0.5f, extra, col);
            } else if (kind == 7) {
                emitRoundRect(x, y, w, h, extra, col);
            }
        }
    }
    if (const JsonVal *ents = j.get("entities"); ents && ents->type == JsonVal::J_ARR) {
        for (const auto &e : ents->arr) {
            uint32_t col = StudioFont::rgba((uint8_t)e.d("r", 100), (uint8_t)e.d("g", 220), (uint8_t)e.d("b", 120));
            emitRect(area.x + (float)e.d("x"), area.y + (float)e.d("y"), (float)e.d("w", 16), (float)e.d("h", 16), col);
        }
    }
    if (const JsonVal *txts = j.get("texts"); txts && txts->type == JsonVal::J_ARR) {
        for (const auto &t : txts->arr) {
            uint32_t col = StudioFont::rgba((uint8_t)t.d("r", 255), (uint8_t)t.d("g", 255), (uint8_t)t.d("b", 255));
            emitText(t.s("text"), area.x + (float)t.d("x"), area.y + (float)t.d("y"), (float)t.d("size", 18), col);
        }
    }
}

void StudioApp::build2DScene(const Rect &area) {
    pushBatch(true, area);
    emitRect(area.x, area.y, area.w, area.h, StudioFont::rgba(18, 18, 18));
    if (last2DFrameJson.empty()) {
        emitTextCentered("2D Canvas - Tap Play to run " + currentProjectName, area, 12.0f * density, Col::muted());
        return;
    }
    renderLuaDrawOverlay(area, true);
}

void StudioApp::buildTopRibbon() {
    float fs = 8.8f * density;
    pushBatch(true, rTopTabs);
    emitRect(rTopTabs.x, rTopTabs.y, rTopTabs.w, rTopTabs.h, Col::bg());
    emitRect(rTopTabs.x, rTopTabs.y + rTopTabs.h - 1, rTopTabs.w, 1, Col::border());

    float cx = 4.0f * density;
    struct TabDef { RibbonTab t; const char *lbl; };
    TabDef tabs[] = {
        {RibbonTab::PROJECTS, "PROJECTS"},
        {RibbonTab::HOME, "HOME"},
        {RibbonTab::CREATE, "CREATE"},
        {RibbonTab::VIEW, "VIEW"},
        {RibbonTab::SCRIPTS, "SCRIPTS"}
    };
    float tw = std::min(74.0f * density, (rTopTabs.w - 8 * density) / 5.0f);
    for (auto &td : tabs) {
        bool act = (td.t == RibbonTab::PROJECTS) ? showProjectsTab : (!showProjectsTab && activeRibbon == td.t);
        Rect rt{cx, 2 * density, tw - 2 * density, rTopTabs.h - 4 * density};
        addBtn(rt, td.lbl, act ? Col::btnAct() : Col::bg(), act ? Col::text() : Col::muted(), 8.3f * density, [this, td]() {
            if (td.t == RibbonTab::PROJECTS) {
                showProjectsTab = true;
            } else {
                showProjectsTab = false;
                activeRibbon = td.t;
            }
        });
        cx += tw;
    }

    pushBatch(true, rRibbon);
    emitRect(rRibbon.x, rRibbon.y, rRibbon.w, rRibbon.h, Col::panel());
    emitRect(rRibbon.x, rRibbon.y + rRibbon.h - 1, rRibbon.w, 1, Col::border());

    float rx = 6.0f * density - ribbonScrollX;
    float ry = rRibbon.y + 3 * density;
    float rh = rRibbon.h - 6 * density;
    auto ribBtn = [&](const std::string &lbl, bool act, uint32_t customBg, std::function<void()> cb) {
        float bw = std::max(38.0f * density, font.measureWidth(lbl, fs) + 12.0f * density);
        Rect r{rx, ry, bw, rh};
        uint32_t bg = customBg ? customBg : (act ? Col::btnAct() : Col::btn());
        addBtn(r, lbl, bg, Col::text(), fs, std::move(cb));
        rx += bw + 4 * density;
    };

    if (showProjectsTab) {
        ribBtn("+ 3D Example", false, Col::btnAct(), [this]() {
            promptTitle = "New 3D Example Name"; promptValue = "Demo3D"; promptTargetKey = "New3DExample";
            modal = ModalKind::TEXT_PROMPT;
        });
        ribBtn("+ 3D Scratch (Player)", false, 0, [this]() {
            promptTitle = "New 3D Scratch Name"; promptValue = "MyGame3D"; promptTargetKey = "New3DScratch";
            modal = ModalKind::TEXT_PROMPT;
        });
        ribBtn("+ 2D Canvas", false, 0, [this]() {
            promptTitle = "New 2D Project Name"; promptValue = "Game2D"; promptTargetKey = "New2D";
            modal = ModalKind::TEXT_PROMPT;
        });
        ribBtn("Back to Scene", false, 0, [this]() { showProjectsTab = false; });
        return;
    }

    if (activeRibbon == RibbonTab::HOME) {
        if (runningLua) {
            ribBtn("Stop", false, Col::red(), [this]() { stopSim(); });
        } else {
            ribBtn("Play", false, Col::green(), [this]() { startSim(); });
        }
        ribBtn("Build APK", false, Col::btnAct(), [this]() {
            saveActiveScript();
            if (currentIs3D) scene.saveToFile(currentProjectDir + "/scene.json");
            promptTitle = "Build APK - App Name";
            promptValue = currentProjectName.empty() ? "MyGame" : currentProjectName;
            promptTargetKey = "BuildApkAppName";
            modal = ModalKind::TEXT_PROMPT;
        });
        ribBtn("Select", toolMode == ToolMode::SELECT, 0, [this]() { toolMode = ToolMode::SELECT; });
        ribBtn("Move", toolMode == ToolMode::MOVE, 0, [this]() { toolMode = ToolMode::MOVE; });
        ribBtn("Scale", toolMode == ToolMode::SCALE, 0, [this]() { toolMode = ToolMode::SCALE; });
        ribBtn("Rotate", toolMode == ToolMode::ROTATE, 0, [this]() { toolMode = ToolMode::ROTATE; });
        ribBtn("+ Node", false, 0, [this]() {
            insertParentId = selectedId;
            modal = ModalKind::INSERT;
        });
        auto groupWith = [this](const std::string &grpCls) {
            std::vector<std::string> ids = multiSelectedIds;
            if (ids.empty() && !selectedId.empty()) ids.push_back(selectedId);
            if (!ids.empty()) {
                std::string gid = scene.groupNodes(ids, grpCls);
                if (!gid.empty()) {
                    selectedId = gid;
                    multiSelectedIds.clear();
                    multiSelectMode = false;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            }
        };
        ribBtn("Group Folder", false, 0, [groupWith]() { groupWith("Folder"); });
        ribBtn("Group Model", false, 0, [groupWith]() { groupWith("Model"); });
        ribBtn("Move To", false, 0, [this]() {
            if (!selectedId.empty() || !multiSelectedIds.empty()) modal = ModalKind::REPARENT;
        });
        if (auto *sn = selectedNode(); sn && sn->isGroup()) {
            ribBtn("Ungroup", false, 0, [this]() {
                if (scene.ungroupNode(selectedId)) {
                    selectedId.clear();
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            });
        }
        ribBtn("Color", false, 0, [this]() {
            if (auto *n = selectedNode()) {
                promptTargetKey.clear();
                syncWheelFromArgb(n->argbColor);
                modal = ModalKind::COLOR;
            }
        });
        ribBtn("Material", false, 0, [this]() { if (selectedNode()) modal = ModalKind::MATERIAL; });
        ribBtn("Texture", false, 0, [this]() {
            if (auto *n = selectedNode(); n && n->isMeshNode() && callbacks.openTexturePicker) {
                callbacks.openTexturePicker(currentProjectName, n->id);
            }
        });
        ribBtn("+ glTF", false, 0, [this]() {
            if (callbacks.openModelPicker) callbacks.openModelPicker(currentProjectName, "");
        });
        ribBtn("Anim", showAnimator || animPlaying, 0, [this]() {
            if (showAnimator) { showAnimator = false; animPlaying = false; }
            else openAnimator(selectedNode());
        });
        ribBtn("Delete", false, 0, [this]() {
            if (!selectedId.empty() && scene.removeById(selectedId)) {
                selectedId.clear();
                scene.saveToFile(currentProjectDir + "/scene.json");
            }
        });
        ribBtn("Save", false, 0, [this]() {
            saveActiveScript();
            if (currentIs3D) scene.saveToFile(currentProjectDir + "/scene.json");
            appendOutput("Saved " + currentProjectName, Col::green());
        });
    } else if (activeRibbon == RibbonTab::CREATE) {
        auto quickAdd = [&](const std::string &cls) {
            SceneNode *parent = selectedNode();
            if (!parent || parent->isScript()) parent = scene.findService("World3D");
            if (auto *p = scene.createNode(parent, cls, cls + std::to_string(scene.nextId))) {
                selectedId = p->id;
                scene.saveToFile(currentProjectDir + "/scene.json");
            }
        };
        ribBtn("+ Box", false, 0, [quickAdd]() { quickAdd("Box"); });
        ribBtn("+ Wedge", false, 0, [quickAdd]() { quickAdd("Wedge"); });
        ribBtn("+ Sphere", false, 0, [quickAdd]() { quickAdd("Sphere"); });
        ribBtn("+ Cylinder", false, 0, [quickAdd]() { quickAdd("Cylinder"); });
        ribBtn("+ Plane", false, 0, [quickAdd]() { quickAdd("Plane"); });
        ribBtn("+ glTF Model", false, 0, [this]() {
            if (callbacks.openModelPicker) callbacks.openModelPicker(currentProjectName, "");
        });
        ribBtn("+ Model", false, 0, [quickAdd]() { quickAdd("Model"); });
        ribBtn("+ Folder", false, 0, [quickAdd]() { quickAdd("Folder"); });
        ribBtn("+ CollisionBox", false, 0, [quickAdd]() { quickAdd("CollisionBox"); });
        ribBtn("+ StaticBody3D", false, 0, [quickAdd]() { quickAdd("StaticBody3D"); });
        ribBtn("+ RigidBody3D", false, 0, [quickAdd]() { quickAdd("RigidBody3D"); });
        ribBtn("+ Camera3D", false, 0, [quickAdd]() { quickAdd("Camera3D"); });
        ribBtn("+ Light3D", false, 0, [quickAdd]() { quickAdd("PointLight3D"); });
    } else if (activeRibbon == RibbonTab::VIEW) {
        ribBtn("Explorer", showExplorer, 0, [this]() { showExplorer = !showExplorer; });
        ribBtn("Properties", showProperties, 0, [this]() { showProperties = !showProperties; });
        ribBtn("Output", showOutput, 0, [this]() { showOutput = !showOutput; });
        ribBtn("Grid", showGrid, 0, [this]() { showGrid = !showGrid; });
        ribBtn("Shadows", showShadows, 0, [this]() { showShadows = !showShadows; });
        ribBtn("Sky", false, 0, [this]() {
            skyEditBot = false;
            if (auto *e = scene.findEnvironment()) syncWheelFromArgb(e->skyTopColor);
            modal = ModalKind::SKY;
        });
        ribBtn("Colliders", showColliders, 0, [this]() { showColliders = !showColliders; });
        ribBtn("Play:Cam3D", useCamera3DInPlay, 0, [this]() { useCamera3DInPlay = !useCamera3DInPlay; });
        ribBtn("Reset Cam", false, 0, [this]() {
            camTargetX = 0; camTargetY = 1.5f; camTargetZ = -3.0f; camYaw = 0.36f; camPitch = 0.34f; camDist = 28.0f;
        });
        ribBtn("Settings", false, 0, [this]() { modal = ModalKind::SETTINGS; });
    } else {
        ribBtn("New Script", false, 0, [this]() {
            if (auto *sss = scene.findService("Scripts")) {
                auto *s = scene.createNode(sss, "Script", "Script" + std::to_string(scene.nextId));
                if (s) {
                    s->scriptPath = "scripts/" + s->name + ".obs";
                    openScript(s->scriptPath);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            } else {
                openScript("main.obs");
            }
        });
        for (const auto &sp : listScriptFiles()) {
            std::string label = sp;
            auto slash = label.find_last_of('/');
            if (slash != std::string::npos) label = label.substr(slash + 1);
            ribBtn(label, activeScript == sp, 0, [this, sp]() { openScript(sp); });
        }
        ribBtn("Settings & Fonts", false, 0, [this]() { modal = ModalKind::SETTINGS; });
    }
}

void StudioApp::buildDocTabs() {
    pushBatch(true, rDocTabs);
    emitRect(rDocTabs.x, rDocTabs.y, rDocTabs.w, rDocTabs.h, Col::bg());
    emitRect(rDocTabs.x, rDocTabs.y + rDocTabs.h - 1, rDocTabs.w, 1, Col::border());
    float fs = 8.3f * density;
    float x = rDocTabs.x + 2 * density;
    float h = rDocTabs.h - 2 * density;

    Rect rProj{x, rDocTabs.y + 1, 56 * density, h};
    addBtn(rProj, "Projects", showProjectsTab ? Col::header() : Col::bg(),
           showProjectsTab ? Col::accent() : Col::muted(), fs, [this]() {
               showProjectsTab = true;
               editorFocused = false;
               if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
               syncOrientation();
           });
    x += rProj.w + 2 * density;

    Rect rv{x, rDocTabs.y + 1, 66 * density, h};
    addBtn(rv, currentIs3D ? "3D Scene" : "2D Canvas", (!showProjectsTab && activeScript.empty()) ? Col::header() : Col::bg(),
           (!showProjectsTab && activeScript.empty()) ? Col::accent() : Col::muted(), fs, [this]() {
               saveActiveScript();
               showProjectsTab = false;
               activeScript.clear();
               editorFocused = false;
               if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
               syncOrientation();
           });
    x += rv.w + 2 * density;

    for (size_t i = 0; i < openScripts.size(); ++i) {
        std::string sp = openScripts[i];
        std::string shortName = sp;
        auto slash = shortName.find_last_of('/');
        if (slash != std::string::npos) shortName = shortName.substr(slash + 1);
        float w = std::max(52 * density, font.measureWidth(shortName, fs) + 20 * density);
        bool act = (!showProjectsTab && activeScript == sp);
        Rect rt{x, rDocTabs.y + 1, w - 15 * density, h};
        addBtn(rt, shortName, act ? Col::header() : Col::bg(), act ? Col::text() : Col::muted(), fs, [this, sp]() {
            openScript(sp);
        });
        Rect rxBtn{rt.x + rt.w, rDocTabs.y + 1, 15 * density, h};
        addBtn(rxBtn, "x", act ? Col::header() : Col::bg(), Col::muted(), fs, [this, sp]() {
            if (activeScript == sp) { saveActiveScript(); activeScript.clear(); syncOrientation(); }
            openScripts.erase(std::remove(openScripts.begin(), openScripts.end(), sp), openScripts.end());
        });
        x += w + 2 * density;
    }
}

void StudioApp::buildRightDock() {
    if (rRightDock.w <= 1.0f) return;
    float fs = 8.3f * density;
    float hdrH = 21.0f * density;
    float rowH = 18.0f * density;

    if (showExplorer && rExplorer.h > 1.0f) {
        pushBatch(true, rExplorer);
        emitRect(rExplorer.x, rExplorer.y, rExplorer.w, rExplorer.h, Col::panel());
        emitRect(rExplorer.x, rExplorer.y, 1, rExplorer.h, Col::border());
        emitRect(rExplorer.x, rExplorer.y, rExplorer.w, hdrH, Col::header());
        emitText("Scene Tree", rExplorer.x + 5 * density, rExplorer.y + 5 * density, fs, Col::text(), rExplorer.w - 60 * density);
        Rect rMulti{rExplorer.x + rExplorer.w - 56 * density, rExplorer.y + 2 * density, 30 * density, hdrH - 4 * density};
        addBtn(rMulti, "Mul", multiSelectMode ? Col::btnAct() : Col::btn(), Col::text(), 7.6f * density, [this]() {
            multiSelectMode = !multiSelectMode;
            if (!multiSelectMode) multiSelectedIds.clear();
        });
        Rect rAdd{rExplorer.x + rExplorer.w - 24 * density, rExplorer.y + 2 * density, 22 * density, hdrH - 4 * density};
        addBtn(rAdd, "+", Col::btn(), Col::accent(), 10.5f * density, [this]() {
            insertParentId = selectedId;
            modal = ModalKind::INSERT;
        });

        Rect listArea{rExplorer.x + 1, rExplorer.y + hdrH, rExplorer.w - 1, std::max(1.0f, rExplorer.h - hdrH)};
        pushBatch(true, listArea);
        struct Row { const SceneNode *n; int depth; };
        std::vector<Row> rows;
        std::function<void(const SceneNode &, int)> flatten = [&](const SceneNode &node, int d) {
            rows.push_back({&node, d});
            if (node.expanded) for (const auto &c : node.children) flatten(c, d + 1);
        };
        for (const auto &svc : scene.root.children) flatten(svc, 0);

        float maxScroll = std::max(0.0f, rows.size() * rowH - listArea.h);
        explorerScrollY = std::clamp(explorerScrollY, 0.0f, maxScroll);
        float y = listArea.y - explorerScrollY;
        for (const auto &r : rows) {
            if (y + rowH >= listArea.y && y <= listArea.y + listArea.h) {
                bool mSel = std::find(multiSelectedIds.begin(), multiSelectedIds.end(), r.n->id) != multiSelectedIds.end();
                bool sel = (r.n->id == selectedId) || mSel;
                Rect rr{listArea.x, y, listArea.w, rowH};
                if (sel) emitRect(rr.x, rr.y, rr.w, rr.h, mSel ? StudioFont::rgba(74, 74, 74) : Col::selRow());
                float indent = (4 + r.depth * 8) * density;
                std::string prefix = r.n->children.empty() ? "  " : (r.n->expanded ? "v " : "> ");
                std::string lbl = prefix + r.n->name + (r.n->className == "Model" ? " [M]" : (r.n->className == "Folder" ? " [F]" : ""));
                uint32_t tc = sel ? Col::text() :
                    (r.n->isScript() ? Col::accent() :
                    (r.n->isGroup() ? StudioFont::rgba(216, 216, 216) :
                    (r.n->isCollisionNode() ? StudioFont::rgba(196, 196, 196) : Col::text())));
                emitText(lbl, rr.x + indent, rr.y + 3 * density, 8.0f * density, tc, rr.w - indent - 2);
                std::string nid = r.n->id;
                addHitBox(rr, [this, nid]() {
                    if (multiSelectMode) {
                        auto it = std::find(multiSelectedIds.begin(), multiSelectedIds.end(), nid);
                        if (it != multiSelectedIds.end()) multiSelectedIds.erase(it);
                        else multiSelectedIds.push_back(nid);
                        selectedId = nid;
                        return;
                    }
                    if (auto *node = scene.findById(nid)) {
                        if (selectedId == nid && !node->children.empty()) node->expanded = !node->expanded;
                        selectedId = nid;
                        if (node->isScript() && !node->scriptPath.empty()) openScript(node->scriptPath);
                        if (node->is3DNode()) {
                            camTargetX = node->posX; camTargetY = node->posY; camTargetZ = node->posZ;
                        }
                    }
                });
            }
            y += rowH;
        }
    }

    if (showProperties && rProperties.h > 1.0f) {
        pushBatch(true, rProperties);
        emitRect(rProperties.x, rProperties.y, rProperties.w, rProperties.h, Col::panel());
        emitRect(rProperties.x, rProperties.y, 1, rProperties.h, Col::border());
        emitRect(rProperties.x, rProperties.y, rProperties.w, hdrH, Col::header());
        emitText("Inspector", rProperties.x + 5 * density, rProperties.y + 5 * density, fs, Col::text());

        Rect pArea{rProperties.x + 1, rProperties.y + hdrH, rProperties.w - 1, std::max(1.0f, rProperties.h - hdrH)};
        pushBatch(true, pArea);
        SceneNode *n = selectedNode();
        if (!n) {
            emitText("No selection", pArea.x + 6 * density, pArea.y + 6 * density, fs, Col::muted());
            return;
        }
        struct Prop { std::string k, v; std::function<void()> onTap; };
        std::vector<Prop> props;
        props.push_back({"Name", n->name, [this, n]() {
            promptTitle = "Rename " + n->name; promptValue = n->name; promptTargetKey = "Rename";
            modal = ModalKind::TEXT_PROMPT;
        }});
        props.push_back({"Type", n->className, nullptr});
        if (n->is3DNode()) {
            props.push_back({"Position", fmtVec3(n->posX, n->posY, n->posZ), [this, n]() {
                promptTitle = "Position (X, Y, Z)"; promptValue = fmtVec3(n->posX, n->posY, n->posZ);
                promptTargetKey = "Position"; modal = ModalKind::TEXT_PROMPT;
            }});
            props.push_back({"Size", fmtVec3(n->sizeX, n->sizeY, n->sizeZ), [this, n]() {
                promptTitle = "Size (X, Y, Z)"; promptValue = fmtVec3(n->sizeX, n->sizeY, n->sizeZ);
                promptTargetKey = "Size"; modal = ModalKind::TEXT_PROMPT;
            }});
            props.push_back({"Rotation", fmtVec3(n->rotX, n->rotY, n->rotZ), [this, n]() {
                promptTitle = "Rotation (X, Y, Z)"; promptValue = fmtVec3(n->rotX, n->rotY, n->rotZ);
                promptTargetKey = "Rotation"; modal = ModalKind::TEXT_PROMPT;
            }});
        }
        if (n->isMeshNode()) {
            if (n->className == "GltfModel") {
                std::string mLbl = n->modelPath.empty() ? "None (Pick .glb/.gltf)" : n->modelPath;
                if (auto sl = mLbl.find_last_of('/'); sl != std::string::npos) mLbl = mLbl.substr(sl + 1);
                props.push_back({"Model File", mLbl, [this, n]() {
                    if (callbacks.openModelPicker) callbacks.openModelPicker(currentProjectName, n->id);
                }});
                if (GltfModelAsset *gm = getOrLoadGltf(n->modelPath)) {
                    std::string clipLbl = "None";
                    if (n->modelAnimIndex >= 0 && n->modelAnimIndex < (int)gm->animations.size()) {
                        clipLbl = gm->animations[(size_t)n->modelAnimIndex].name;
                    }
                    props.push_back({"glTF Clip", clipLbl + " (" + std::to_string(gm->animations.size()) + ")", [this, n]() {
                        openAnimator(n);
                    }});
                }
            } else {
                props.push_back({"Mesh", n->shape(), [this]() { modal = ModalKind::SHAPE; }});
                props.push_back({"Segments", std::to_string(n->segments), [this, n]() {
                    promptTitle = "Polygon Segments (6..64)"; promptValue = std::to_string(n->segments);
                    promptTargetKey = "Segments"; modal = ModalKind::TEXT_PROMPT;
                }});
                if (n->shape() == "Box" || n->shape() == "Plane") {
                    props.push_back({"Roundness", fmtNum(n->cornerRadius), [this, n]() {
                        promptTitle = "Corner Radius (0..0.48)"; promptValue = fmtNum(n->cornerRadius);
                        promptTargetKey = "CornerRadius"; modal = ModalKind::TEXT_PROMPT;
                    }});
                }
            }
            props.push_back({"Color", "#", [this, n]() {
                promptTargetKey.clear();
                syncWheelFromArgb(n->argbColor);
                modal = ModalKind::COLOR;
            }});
            props.push_back({"Material", n->material, [this]() { modal = ModalKind::MATERIAL; }});
            std::string texLbl = n->texturePath.empty() ? "None (Pick)" : n->texturePath;
            if (auto sl = texLbl.find_last_of('/'); sl != std::string::npos) texLbl = texLbl.substr(sl + 1);
            props.push_back({"Texture", texLbl, [this, n]() {
                if (callbacks.openTexturePicker) callbacks.openTexturePicker(currentProjectName, n->id);
            }});
            if (!n->texturePath.empty()) {
                props.push_back({"Clear Tex", "Remove", [this, n]() {
                    n->texturePath.clear();
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }});
            }
            props.push_back({"Visible", n->visible ? "true" : "false", [this, n]() {
                n->visible = !n->visible; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
            props.push_back({"CastShadow", n->castShadow ? "true" : "false", [this, n]() {
                n->castShadow = !n->castShadow; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
            props.push_back({"Transp", fmtNum(n->transparency), [this, n]() {
                promptTitle = "Transparency (0..1)"; promptValue = fmtNum(n->transparency);
                promptTargetKey = "Transparency"; modal = ModalKind::TEXT_PROMPT;
            }});
        }
        if (n->is3DNode()) {
            if (n->className == "GltfModel") {
                props.push_back({"AnimAuto", n->animAutoPlay ? "true" : "false", [this, n]() {
                    n->animAutoPlay = !n->animAutoPlay;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }});
                props.push_back({"AnimLoop", n->animLoop ? "true" : "false", [this, n]() {
                    n->animLoop = !n->animLoop;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }});
            } else {
                std::string animLbl = n->animPath.empty() ? "None (Pick File)" : n->animPath;
                auto sl = animLbl.find_last_of('/');
                if (sl != std::string::npos) animLbl = animLbl.substr(sl + 1);
                props.push_back({"Animation", animLbl, [this, n]() { openAnimator(n); }});
                if (!n->animPath.empty()) {
                    props.push_back({"AnimAuto", n->animAutoPlay ? "true" : "false", [this, n]() {
                        n->animAutoPlay = !n->animAutoPlay;
                        scene.saveToFile(currentProjectDir + "/scene.json");
                    }});
                }
            }
        }
        if (n->isMeshNode() || n->isCollisionNode()) {
            props.push_back({"Collidable", n->collidable ? "true" : "false", [this, n]() {
                n->collidable = !n->collidable; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
            props.push_back({"Static", n->staticBody ? "true" : "false", [this, n]() {
                n->staticBody = !n->staticBody; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
            if (!n->staticBody || n->className == "RigidBody3D") {
                props.push_back({"Mass", fmtNum(n->mass), [this, n]() {
                    promptTitle = "Mass (kg)"; promptValue = fmtNum(n->mass);
                    promptTargetKey = "Mass"; modal = ModalKind::TEXT_PROMPT;
                }});
                props.push_back({"Gravity", fmtNum(n->gravityScale), [this, n]() {
                    promptTitle = "Gravity Scale"; promptValue = fmtNum(n->gravityScale);
                    promptTargetKey = "Gravity"; modal = ModalKind::TEXT_PROMPT;
                }});
            }
        }
        if (n->className == "Camera3D") {
            props.push_back({"FOV", fmtNum(n->fov), [this, n]() {
                promptTitle = "Camera3D FOV"; promptValue = fmtNum(n->fov);
                promptTargetKey = "FOV"; modal = ModalKind::TEXT_PROMPT;
            }});
        } else if (n->className == "DirectionalLight3D" || n->className == "PointLight3D") {
            props.push_back({"Energy", fmtNum(n->energy), [this, n]() {
                promptTitle = "Light Energy"; promptValue = fmtNum(n->energy);
                promptTargetKey = "Energy"; modal = ModalKind::TEXT_PROMPT;
            }});
        } else if (n->isScript()) {
            props.push_back({"Open Code", "Edit Script", [this, n]() { openScript(n->scriptPath); }});
            props.push_back({"Path", n->scriptPath, [this, n]() { openScript(n->scriptPath); }});
            props.push_back({"Disabled", n->disabled ? "true" : "false", [this, n]() {
                n->disabled = !n->disabled; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
        } else if (n->className == "Environment" || n->className == "Lighting") {
            props.push_back({"Sky Preset", n->skyPreset, [this, n]() {
                skyEditBot = false;
                syncWheelFromArgb(n->skyTopColor);
                modal = ModalKind::SKY;
            }});
            props.push_back({"Sky Top", "#", [this, n]() {
                promptTargetKey = "SkyTop";
                syncWheelFromArgb(n->skyTopColor);
                modal = ModalKind::COLOR;
            }});
            props.push_back({"Sky Horizon", "#", [this, n]() {
                promptTargetKey = "SkyBot";
                syncWheelFromArgb(n->skyBotColor);
                modal = ModalKind::COLOR;
            }});
            props.push_back({"ClockTime", fmtNum(n->clockTime), [this, n]() {
                promptTitle = "ClockTime (0..24)"; promptValue = fmtNum(n->clockTime);
                promptExtra.clear(); promptTargetKey = "ClockTime"; modal = ModalKind::TEXT_PROMPT;
            }});
        } else if (n->isUi()) {
            props.push_back({"Text", n->text, [this, n]() {
                promptTitle = "Label Text"; promptValue = n->text;
                promptTargetKey = "Text"; modal = ModalKind::TEXT_PROMPT;
            }});
        }

        float maxScroll = std::max(0.0f, props.size() * rowH - pArea.h);
        propsScrollY = std::clamp(propsScrollY, 0.0f, maxScroll);
        float y = pArea.y - propsScrollY;
        float splitX = pArea.x + pArea.w * 0.45f;
        for (const auto &p : props) {
            if (y + rowH >= pArea.y && y <= pArea.y + pArea.h) {
                Rect pr{pArea.x, y, pArea.w, rowH};
                emitRect(pr.x, pr.y + rowH - 1, pr.w, 1, Col::border());
                emitText(p.k, pr.x + 4 * density, pr.y + 3 * density, 7.8f * density, Col::muted(), splitX - pr.x - 4);
                if (p.k == "Color") {
                    emitRect(splitX + 2, pr.y + 3 * density, 24 * density, rowH - 6 * density, Col::argbToUi(n->argbColor));
                } else if (p.k == "Sky Top") {
                    emitRect(splitX + 2, pr.y + 3 * density, 24 * density, rowH - 6 * density, Col::argbToUi(n->skyTopColor));
                } else if (p.k == "Sky Horizon") {
                    emitRect(splitX + 2, pr.y + 3 * density, 24 * density, rowH - 6 * density, Col::argbToUi(n->skyBotColor));
                } else {
                    emitText(p.v, splitX + 2, pr.y + 3 * density, 7.8f * density,
                             p.onTap ? Col::text() : Col::muted(), pr.x + pr.w - splitX - 4);
                }
                if (p.onTap) addHitBox(pr, p.onTap);
            }
            y += rowH;
        }
    }
}

void StudioApp::buildAnimatorDock(const Rect &area) {
    pushBatch(true, area);
    emitRect(area.x, area.y, area.w, area.h, Col::panel());
    emitRect(area.x, area.y, area.w, 1, Col::border());

    float pad = 4.0f * density;
    float hdrH = 22.0f * density;
    float fs = 7.8f * density;
    emitRect(area.x, area.y, area.w, hdrH, Col::header());
    emitRect(area.x, area.y + hdrH - 1, area.w, 1, Col::border());

    SceneNode *tn = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId);
    GltfModelAsset *gm = (tn && tn->className == "GltfModel" && !tn->modelPath.empty())
        ? getOrLoadGltf(tn->modelPath) : nullptr;
    if (gm && !gm->nodes.empty()) {
        animBoneNodeIdx = std::clamp(animBoneNodeIdx, 0, (int)gm->nodes.size() - 1);
    }

    std::string targetName = (tn && tn->is3DNode()) ? tn->name : "None";
    std::string fileLbl = gm
        ? ((tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size())
            ? gm->animations[(size_t)tn->modelAnimIndex].name : "No glTF clip")
        : (activeAnimFile.empty() ? "No clip" : activeAnimFile);
    if (!gm) {
        if (auto sl = fileLbl.find_last_of('/'); sl != std::string::npos) fileLbl = fileLbl.substr(sl + 1);
    }

    float bx = area.x + pad;
    float by = area.y + 2 * density;
    float bh = hdrH - 4 * density;
    float infoW = std::min(150.0f * density, area.w * 0.28f);
    emitText("Anim: " + targetName + " [" + fileLbl + "]", bx, area.y + 5 * density, fs, Col::text(), infoW);
    bx += infoW + pad;

    auto topBtn = [&](const std::string &lbl, bool act, std::function<void()> cb) {
        float w = std::max(34.0f * density, font.measureWidth(lbl, fs) + 10.0f * density);
        if (bx + w > area.x + area.w - 44 * density) return;
        addBtn({bx, by, w, bh}, lbl, act ? Col::btnAct() : Col::btn(), Col::text(), fs, std::move(cb));
        bx += w + 3 * density;
    };

    if (gm) {
        topBtn("+ Clip", false, [this]() {
            promptTitle = "New glTF Clip Name";
            promptValue = "Clip_" + std::to_string(scene.nextId);
            promptTargetKey = "NewGltfClip";
            modal = ModalKind::TEXT_PROMPT;
        });
        topBtn(animPlaying ? "Pause" : "Play", animPlaying, [this, tn, gm]() {
            if (animPlaying) animPlaying = false;
            else if (tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size()) {
                float dur = gm->animations[(size_t)tn->modelAnimIndex].duration;
                if (dur > 1e-4f) {
                    if (animTime >= dur) animTime = 0.0f;
                    animPlaying = true;
                }
            }
        });
        topBtn(tn->animLoop ? "Loop:ON" : "Loop:OFF", tn->animLoop, [this, tn]() {
            tn->animLoop = !tn->animLoop;
            scene.saveToFile(currentProjectDir + "/scene.json");
        });
        topBtn("+ Key", false, [this, tn, gm]() {
            if (gm->nodes.empty()) return;
            if (tn->modelAnimIndex < 0 || tn->modelAnimIndex >= (int)gm->animations.size()) {
                tn->modelAnimIndex = gm->addAnimationClip("Clip_0");
            }
            float tr[3], rq[4], sc[3];
            gm->sampleNodeTRS(tn->modelAnimIndex, animBoneNodeIdx, animTime, tr, rq, sc);
            gm->setNodeKeyframe(tn->modelAnimIndex, animBoneNodeIdx, animTime, tr, rq, sc);
            gm->saveAnimationsToFile();
            tn->animAutoPlay = true;
            scene.saveToFile(currentProjectDir + "/scene.json");
            animTime += 0.4f;
        });
        topBtn("Del Key", false, [this, tn, gm]() {
            if (tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size()) {
                gm->removeKeyframeNear(tn->modelAnimIndex, animBoneNodeIdx, animTime);
                gm->saveAnimationsToFile();
            }
        });
        topBtn("t-", false, [this]() {
            animPlaying = false;
            animTime = std::max(0.0f, animTime - 0.25f);
        });
        topBtn("t+", false, [this, tn, gm]() {
            animPlaying = false;
            float dur = (tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size())
                ? gm->animations[(size_t)tn->modelAnimIndex].duration : 2.0f;
            animTime = std::min(std::max(4.0f, dur + 1.0f), animTime + 0.25f);
        });
    } else {
        topBtn("+ New", false, [this]() {
            promptTitle = "New Animation Name";
            promptValue = "anim_" + std::to_string(scene.nextId);
            promptTargetKey = "NewAnim";
            modal = ModalKind::TEXT_PROMPT;
        });
        topBtn(animPlaying ? "Pause" : "Play", animPlaying, [this]() {
            if (animPlaying) {
                animPlaying = false;
            } else if (!activeAnim.frames.empty()) {
                if (animTime >= activeAnim.duration()) animTime = 0.0f;
                animPlaying = true;
            }
        });
        topBtn(activeAnim.loop ? "Loop:ON" : "Loop:OFF", activeAnim.loop, [this]() {
            activeAnim.loop = !activeAnim.loop;
            if (!activeAnimFile.empty()) activeAnim.saveToFile(currentProjectDir + "/" + activeAnimFile);
            if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode()) {
                n->animLoop = activeAnim.loop;
                scene.saveToFile(currentProjectDir + "/scene.json");
            }
        });
        topBtn("+ Key", false, [this]() {
            SceneNode *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId);
            if (!n || !n->is3DNode()) {
                appendOutput("Select a 3D object first.", Col::muted());
                return;
            }
            if (activeAnimFile.empty()) {
                activeAnimFile = "animations/" + n->name + "_anim.anim.json";
                activeAnim = AnimClip{};
                activeAnim.name = n->name + "_anim";
            }
            float tPos = animTime;
            bool replaced = false;
            for (auto &kf : activeAnim.frames) {
                if (std::fabs(kf.t - tPos) < 0.05f) {
                    kf = {tPos, n->posX, n->posY, n->posZ, n->rotX, n->rotY, n->rotZ, n->sizeX, n->sizeY, n->sizeZ};
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                activeAnim.frames.push_back({tPos, n->posX, n->posY, n->posZ, n->rotX, n->rotY, n->rotZ, n->sizeX, n->sizeY, n->sizeZ});
            }
            activeAnim.sortFrames();
            std::string dirPath = currentProjectDir + "/animations";
            mkdir(dirPath.c_str(), 0755);
            activeAnim.saveToFile(currentProjectDir + "/" + activeAnimFile);
            n->animPath = activeAnimFile;
            n->animAutoPlay = true;
            n->animLoop = activeAnim.loop;
            scene.saveToFile(currentProjectDir + "/scene.json");
            animTime = tPos + 0.5f;
        });
        topBtn("Del Key", false, [this]() {
            if (activeAnim.frames.empty()) return;
            size_t bestIdx = 0;
            float bestDist = 1e9f;
            for (size_t i = 0; i < activeAnim.frames.size(); ++i) {
                float d = std::fabs(activeAnim.frames[i].t - animTime);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            if (bestDist <= 0.35f || activeAnim.frames.size() == 1) {
                activeAnim.frames.erase(activeAnim.frames.begin() + (ptrdiff_t)bestIdx);
            } else {
                activeAnim.frames.pop_back();
            }
            if (!activeAnimFile.empty()) activeAnim.saveToFile(currentProjectDir + "/" + activeAnimFile);
        });
        topBtn("t-", false, [this]() {
            animPlaying = false;
            animTime = std::max(0.0f, animTime - 0.25f);
            if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode() && !activeAnim.frames.empty()) {
                activeAnim.sample(animTime, *n);
            }
        });
        topBtn("t+", false, [this]() {
            animPlaying = false;
            animTime = std::min(std::max(4.0f, activeAnim.duration() + 1.0f), animTime + 0.25f);
            if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode() && !activeAnim.frames.empty()) {
                activeAnim.sample(animTime, *n);
            }
        });
    }
    addBtn({area.x + area.w - 40 * density, by, 36 * density, bh}, "Close", Col::btn(), Col::muted(), fs, [this]() {
        showAnimator = false;
        animPlaying = false;
    });

    // Body: left = clip list (glTF clips or .anim.json files), right = bone/timeline controls
    Rect body{area.x, area.y + hdrH, area.w, std::max(24.0f, area.h - hdrH)};
    float listW = std::min(130.0f * density, body.w * 0.28f);
    Rect fileRect{body.x, body.y, listW, body.h};
    Rect tlRect{body.x + listW, body.y, body.w - listW, body.h};

    pushBatch(true, fileRect);
    emitRect(fileRect.x + fileRect.w - 1, fileRect.y, 1, fileRect.h, Col::border());
    float fy = fileRect.y + 3 * density;
    if (gm) {
        for (size_t ci = 0; ci < gm->animations.size(); ++ci) {
            if (fy + 18 * density > fileRect.y + fileRect.h) break;
            bool sel = (tn->modelAnimIndex == (int)ci);
            addBtn({fileRect.x + 3 * density, fy, fileRect.w - 6 * density, 16 * density},
                   gm->animations[ci].name, sel ? Col::btnAct() : Col::btn(), Col::text(), 7.3f * density, [this, tn, ci]() {
                       tn->modelAnimIndex = (int)ci;
                       animTime = 0.0f;
                       animPlaying = false;
                       scene.saveToFile(currentProjectDir + "/scene.json");
                   });
            fy += 18 * density;
        }
        if (gm->animations.empty()) {
            emitText("Tap + Clip", fileRect.x + 4 * density, fy + 2 * density, 7.3f * density, Col::muted(), fileRect.w - 8 * density);
        }
    } else {
        auto files = listAnimationFiles();
        for (const auto &fp : files) {
            if (fy + 18 * density > fileRect.y + fileRect.h) break;
            std::string fn = fp;
            if (auto sl = fn.find_last_of('/'); sl != std::string::npos) fn = fn.substr(sl + 1);
            bool sel = (fp == activeAnimFile);
            addBtn({fileRect.x + 3 * density, fy, fileRect.w - 6 * density, 16 * density}, fn,
                   sel ? Col::btnAct() : Col::btn(), Col::text(), 7.3f * density, [this, fp]() {
                       activeAnimFile = fp;
                       AnimClip loaded;
                       if (AnimClip::loadFromFile(currentProjectDir + "/" + fp, loaded)) {
                           activeAnim = std::move(loaded);
                       }
                       animTime = 0.0f;
                       animPlaying = false;
                       if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode()) {
                           n->animPath = fp;
                           n->animAutoPlay = true;
                           n->animLoop = activeAnim.loop;
                           activeAnim.sample(0.0f, *n);
                           scene.saveToFile(currentProjectDir + "/scene.json");
                       }
                   });
            fy += 18 * density;
        }
    }

    pushBatch(true, tlRect);
    if (gm) {
        float curDur = (tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size())
            ? gm->animations[(size_t)tn->modelAnimIndex].duration : 0.0f;
        float maxT = std::max(2.5f, curDur);
        float rx = tlRect.x + pad;
        float row1Y = tlRect.y + 3 * density;
        std::string boneName = gm->nodes.empty() ? "No nodes" : gm->nodes[(size_t)animBoneNodeIdx].name;
        addBtn({rx, row1Y, 18 * density, 16 * density}, "<", Col::btn(), Col::text(), 7.5f * density, [this, gm]() {
            if (!gm->nodes.empty()) animBoneNodeIdx = (animBoneNodeIdx + (int)gm->nodes.size() - 1) % (int)gm->nodes.size();
        });
        float bnW = std::min(100.0f * density, tlRect.w * 0.25f);
        emitText("Bone: " + boneName, rx + 21 * density, row1Y + 2 * density, 7.3f * density, Col::text(), bnW);
        addBtn({rx + 23 * density + bnW, row1Y, 18 * density, 16 * density}, ">", Col::btn(), Col::text(), 7.5f * density, [this, gm]() {
            if (!gm->nodes.empty()) animBoneNodeIdx = (animBoneNodeIdx + 1) % (int)gm->nodes.size();
        });

        // Quick bone rotation buttons (+/- 15 deg) that keyframe immediately into the glTF clip
        float rotBx = rx + 46 * density + bnW;
        struct RotCmd { const char *lbl; int axis; float deg; };
        static const RotCmd rCmds[] = {{"RX-",0,-15},{"RX+",0,15},{"RY-",1,-15},{"RY+",1,15},{"RZ-",2,-15},{"RZ+",2,15}};
        float rbw = 26 * density;
        for (int i = 0; i < 6; ++i) {
            if (rotBx + rbw > tlRect.x + tlRect.w - pad) break;
            addBtn({rotBx, row1Y, rbw, 16 * density}, rCmds[i].lbl, Col::btn(), Col::text(), 7.0f * density, [this, tn, gm, i]() {
                if (gm->nodes.empty()) return;
                if (tn->modelAnimIndex < 0 || tn->modelAnimIndex >= (int)gm->animations.size()) {
                    tn->modelAnimIndex = gm->addAnimationClip("Clip_0");
                }
                float tr[3], rq[4], sc[3];
                gm->sampleNodeTRS(tn->modelAnimIndex, animBoneNodeIdx, animTime, tr, rq, sc);
                float ex = 0, ey = 0, ez = 0;
                GltfModelAsset::eulerDegFromQuat(rq, ex, ey, ez);
                if (rCmds[i].axis == 0) ex += rCmds[i].deg;
                else if (rCmds[i].axis == 1) ey += rCmds[i].deg;
                else ez += rCmds[i].deg;
                GltfModelAsset::quatFromEulerDeg(ex, ey, ez, rq);
                gm->setNodeKeyframe(tn->modelAnimIndex, animBoneNodeIdx, animTime, tr, rq, sc);
                gm->saveAnimationsToFile();
                tn->animAutoPlay = true;
                scene.saveToFile(currentProjectDir + "/scene.json");
            });
            rotBx += rbw + 2 * density;
        }

        float row2Y = tlRect.y + 22 * density;
        emitText("t=" + fmtNum(animTime) + "s / " + fmtNum(maxT) + "s",
                 tlRect.x + pad, row2Y, 7.4f * density, Col::muted(), tlRect.w - pad * 2);

        Rect track{tlRect.x + pad, tlRect.y + 36 * density, std::max(40.0f, tlRect.w - pad * 2), 24 * density};
        emitRect(track.x, track.y, track.w, track.h, Col::bg());
        emitRectOutline(track.x, track.y, track.w, track.h, 1.0f, Col::border());
        float prog = std::clamp(animTime / maxT, 0.0f, 1.0f);
        emitRect(track.x, track.y, track.w * prog, track.h, StudioFont::rgba(52, 52, 52, 180));
        emitRect(track.x + track.w * prog - 1.5f * density, track.y, 3.0f * density, track.h, Col::accent());
        for (int s = 0; s < 8; ++s) {
            float sw = track.w / 8.0f;
            addHitBox({track.x + s * sw, track.y, sw, track.h}, [this, s, maxT]() {
                animPlaying = false;
                animTime = ((float)s + 0.5f) / 8.0f * maxT;
            });
        }
        if (tn->modelAnimIndex >= 0 && tn->modelAnimIndex < (int)gm->animations.size()) {
            for (const auto &ch : gm->animations[(size_t)tn->modelAnimIndex].channels) {
                if (ch.targetNode != animBoneNodeIdx) continue;
                for (const auto &kf : ch.keys) {
                    float u = std::clamp(kf.t / maxT, 0.0f, 1.0f);
                    emitRect(track.x + u * track.w - 2 * density, track.y + 3 * density, 4 * density, track.h - 6 * density, Col::text());
                }
                break;
            }
        }
        return;
    }

    float maxT = std::max(2.5f, activeAnim.duration());
    emitText("t=" + fmtNum(animTime) + "s / " + fmtNum(maxT) + "s   Keys: " + std::to_string(activeAnim.frames.size()),
             tlRect.x + pad, tlRect.y + 3 * density, 7.6f * density, Col::muted(), tlRect.w - pad * 2);

    Rect track{tlRect.x + pad, tlRect.y + 20 * density, std::max(40.0f, tlRect.w - pad * 2), 26 * density};
    emitRect(track.x, track.y, track.w, track.h, Col::bg());
    emitRectOutline(track.x, track.y, track.w, track.h, 1.0f, Col::border());
    float prog = std::clamp(animTime / maxT, 0.0f, 1.0f);
    emitRect(track.x, track.y, track.w * prog, track.h, StudioFont::rgba(52, 52, 52, 180));
    emitRect(track.x + track.w * prog - 1.5f * density, track.y, 3.0f * density, track.h, Col::accent());

    // Step scrub buttons along track (8 segments)
    for (int s = 0; s < 8; ++s) {
        float sw = track.w / 8.0f;
        Rect seg{track.x + s * sw, track.y, sw, track.h};
        addHitBox(seg, [this, s, maxT]() {
            animPlaying = false;
            animTime = ((float)s + 0.5f) / 8.0f * maxT;
            if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode() && !activeAnim.frames.empty()) {
                activeAnim.sample(animTime, *n);
            }
        });
    }

    // Draw keyframe diamonds/bars and quick-select chips below track
    float ky = track.y + track.h + 4 * density;
    float kx = track.x;
    for (size_t i = 0; i < activeAnim.frames.size(); ++i) {
        const auto &kf = activeAnim.frames[i];
        float u = std::clamp(kf.t / maxT, 0.0f, 1.0f);
        float mx = track.x + u * track.w;
        emitRect(mx - 2.0f * density, track.y + 3 * density, 4.0f * density, track.h - 6 * density, Col::text());
        if (ky + 16 * density <= tlRect.y + tlRect.h - 2) {
            std::string kl = fmtNum(kf.t) + "s";
            float kw = std::max(28.0f * density, font.measureWidth(kl, 7.2f * density) + 8 * density);
            if (kx + kw <= tlRect.x + tlRect.w - pad) {
                bool nearT = std::fabs(kf.t - animTime) < 0.08f;
                addBtn({kx, ky, kw, 16 * density}, kl, nearT ? Col::btnAct() : Col::btn(), Col::text(), 7.2f * density, [this, i]() {
                    animPlaying = false;
                    animTime = activeAnim.frames[i].t;
                    if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode()) {
                        activeAnim.sample(animTime, *n);
                    }
                });
                kx += kw + 3 * density;
            }
        }
    }
}

void StudioApp::buildBottomDock() {
    if (showAnimator && !showProjectsTab && activeScript.empty()) {
        buildAnimatorDock(rBottomDock);
        return;
    }
    pushBatch(true, rBottomDock);
    emitRect(rBottomDock.x, rBottomDock.y, rBottomDock.w, rBottomDock.h, Col::panel());
    emitRect(rBottomDock.x, rBottomDock.y, rBottomDock.w, 1, Col::border());

    if (showOutput && rOutLog.h > 1.0f) {
        float hdrH = rOutLog.y - rBottomDock.y;
        emitRect(rBottomDock.x, rBottomDock.y, rBottomDock.w, hdrH, Col::header());
        emitText("Output", rBottomDock.x + 6 * density, rBottomDock.y + 1.5f * density, 7.8f * density, Col::muted());
        Rect rClr{rBottomDock.x + rBottomDock.w - 38 * density, rBottomDock.y + 1, 34 * density, hdrH - 2};
        addBtn(rClr, "Clear", Col::btn(), Col::muted(), 7.3f * density, [this]() { outputLines.clear(); });

        pushBatch(true, rOutLog);
        float lh = 11.0f * density;
        float maxScroll = std::max(0.0f, outputLines.size() * lh - rOutLog.h);
        outputScrollY = std::clamp(outputScrollY, 0.0f, maxScroll);
        float y = rOutLog.y - outputScrollY;
        for (const auto &ol : outputLines) {
            if (y + lh >= rOutLog.y && y <= rOutLog.y + rOutLog.h) {
                emitText(ol.text, rOutLog.x + 6 * density, y + 1, 7.8f * density, ol.color, rOutLog.w - 12 * density);
            }
            y += lh;
        }
    }

    pushBatch(true, rCmdBar);
    emitRect(rCmdBar.x, rCmdBar.y, rCmdBar.w, rCmdBar.h, Col::bg());
    emitRect(rCmdBar.x, rCmdBar.y, rCmdBar.w, 1, Col::border());
    float runW = 38.0f * density;
    Rect rInput{rCmdBar.x + 2 * density, rCmdBar.y + 2 * density, rCmdBar.w - runW - 6 * density, rCmdBar.h - 4 * density};
    emitRect(rInput.x, rInput.y, rInput.w, rInput.h, commandFocused ? Col::header() : Col::panel());
    if (commandFocused) emitRectOutline(rInput.x, rInput.y, rInput.w, rInput.h, 1.0f, Col::accent());
    std::string disp = commandText.empty() ? "> Run a command" : ("> " + commandText + (commandFocused ? "_" : ""));
    emitText(disp, rInput.x + 4 * density, rInput.y + 3 * density, 8.2f * density,
             commandText.empty() ? Col::muted() : Col::text(), rInput.w - 8 * density);
    addHitBox(rInput, [this]() {
        commandFocused = true;
        editorFocused = false;
        if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(true);
    });
    Rect rRun{rCmdBar.x + rCmdBar.w - runW - 2 * density, rCmdBar.y + 2 * density, runW, rCmdBar.h - 4 * density};
    addBtn(rRun, "Run", Col::btnAct(), Col::text(), 8.2f * density, [this]() {
        std::string cmd = commandText;
        commandText.clear();
        execCommand(cmd);
    });
}

void StudioApp::buildCodeEditor(const Rect &area) {
    float hdrH  = 30.0f * density;
    float tabsH = 22.0f * density;
    float barH  = 24.0f * density;
    float botH  = tabsH + barH;
    Rect hdrRect {area.x, area.y, area.w, hdrH};
    Rect codeRect{area.x, area.y + hdrH, area.w, std::max(40.0f, area.h - hdrH - botH)};
    Rect tabsRect{area.x, area.y + hdrH + codeRect.h, area.w, tabsH};
    Rect barRect {area.x, area.y + hdrH + codeRect.h + tabsH, area.w, barH};

    // Minimal top bar: Back | file - Obsidronium | Vert/Horiz
    pushBatch(true, hdrRect);
    emitRect(hdrRect.x, hdrRect.y, hdrRect.w, hdrRect.h, Col::header());
    emitRect(hdrRect.x, hdrRect.y + hdrRect.h - 1, hdrRect.w, 1, Col::border());
    float backW = 58.0f * density;
    float oriW  = 58.0f * density;
    addBtn({hdrRect.x + 4 * density, hdrRect.y + 4 * density, backW, hdrH - 8 * density},
           "< Back", Col::btn(), Col::text(), 8.5f * density, [this]() {
               saveActiveScript();
               activeScript.clear();
               editorFocused = false;
               if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
               syncOrientation();
           });
    std::string shortName = activeScript;
    if (auto sl = shortName.find_last_of('/'); sl != std::string::npos) shortName = shortName.substr(sl + 1);
    Rect titleRect{hdrRect.x + backW + 8 * density, hdrRect.y, std::max(40.0f, hdrRect.w - backW - oriW - 16 * density), hdrH};
    std::string hdrTitle = editorStatus.empty() ? (shortName + " - Obsidronium") : editorStatus;
    uint32_t hdrCol = editorStatus.empty() ? Col::accent() : editorStatusCol;
    emitTextCentered(hdrTitle, titleRect, 8.6f * density, hdrCol);
    addBtn({hdrRect.x + hdrRect.w - oriW - 4 * density, hdrRect.y + 4 * density, oriW, hdrH - 8 * density},
           editorPortrait ? "Vert" : "Horiz", Col::btnAct(), Col::text(), 8.5f * density, [this]() {
               editorPortrait = !editorPortrait;
               syncOrientation();
           });

    pushBatch(true, codeRect);
    uint32_t edBg = prefTheme == 1 ? StudioFont::rgba(248, 248, 248) : Col::bg();
    uint32_t edFg = prefTheme == 1 ? StudioFont::rgba(24, 24, 24) : Col::text();
    emitRect(codeRect.x, codeRect.y, codeRect.w, codeRect.h, edBg);

    float fs = std::max(9.0f, (float)prefFontSize * 0.72f) * density;
    float lh = fs * 1.28f;
    int totalLines = lc_editor_line_count();
    float maxScroll = std::max(0.0f, totalLines * lh - codeRect.h + lh * 2);
    editorScrollY = std::clamp(editorScrollY, 0.0f, maxScroll);

    int curLine = lc_editor_cursor_line();
    int curCol  = lc_editor_cursor_col();
    float gutterW = prefLineNumbers ? 26.0f * density : 4.0f * density;

    int firstLine = std::max(0, (int)(editorScrollY / lh));
    int lastLine  = std::min(totalLines - 1, (int)((editorScrollY + codeRect.h) / lh) + 1);
    char buf[4096];
    int toks[256 * 3];

    for (int ln = firstLine; ln <= lastLine; ++ln) {
        float y = codeRect.y + ln * lh - editorScrollY;
        if (ln == curLine) {
            emitRect(codeRect.x, y, codeRect.w, lh,
                     prefTheme == 1 ? StudioFont::rgba(224, 224, 224) : StudioFont::rgba(32, 32, 32));
        }
        if (prefLineNumbers) {
            emitText(std::to_string(ln + 1), codeRect.x + 3 * density, y + 2, fs * 0.85f, Col::muted(), gutterW - 4);
        }
        int bytes = lc_editor_get_line(ln, buf, sizeof(buf));
        std::string lineStr(buf, bytes);
        int ntok = lc_editor_tokenize_line(ln, toks, 256);

        std::vector<uint32_t> colAt(lineStr.size() + 1, edFg);
        for (int ti = 0; ti < ntok; ++ti) {
            int c0 = toks[ti * 3 + 0], len = toks[ti * 3 + 1], k = toks[ti * 3 + 2];
            uint32_t tc = edFg;
            if (prefTheme == 1) {
                if (k == 1) tc = StudioFont::rgba(10, 10, 10);
                else if (k == 2) tc = StudioFont::rgba(56, 56, 56);
                else if (k == 3) tc = StudioFont::rgba(36, 36, 36);
                else if (k == 4) tc = StudioFont::rgba(72, 72, 72);
                else if (k == 5) tc = StudioFont::rgba(64, 64, 64);
                else if (k == 6) tc = StudioFont::rgba(130, 130, 130);
                else if (k == 7) tc = StudioFont::rgba(48, 48, 48);
            } else {
                if (k == 1) tc = StudioFont::rgba(255, 255, 255);
                else if (k == 2) tc = StudioFont::rgba(220, 220, 220);
                else if (k == 3) tc = StudioFont::rgba(235, 235, 235);
                else if (k == 4) tc = StudioFont::rgba(202, 202, 202);
                else if (k == 5) tc = StudioFont::rgba(212, 212, 212);
                else if (k == 6) tc = StudioFont::rgba(128, 128, 128);
                else if (k == 7) tc = StudioFont::rgba(228, 228, 228);
            }
            for (int c = c0; c < c0 + len && c < (int)colAt.size(); ++c) colAt[(size_t)c] = tc;
        }
        float px = codeRect.x + gutterW + 3 * density;
        const char *p = lineStr.data(), *end = p + lineStr.size();
        int charIdx = 0;
        float s = fs / font.basePixelHeight;
        while (p < end) {
            uint32_t cp = StudioFont::nextUtf8(p, end);
            if (ln == curLine && charIdx == curCol && editorFocused) {
                emitRect(px, y + 1, 2.0f, lh - 2, Col::accent());
            }
            auto it = font.glyphs.find(cp);
            if (it == font.glyphs.end()) it = font.glyphs.find('?');
            if (it != font.glyphs.end()) {
                const FontGlyph &g = it->second;
                uint32_t gc = charIdx < (int)colAt.size() ? colAt[(size_t)charIdx] : edFg;
                if (g.x1 > g.x0 && g.y1 > g.y0 && !uiBatches.empty()) {
                    float gx0 = px + g.x0 * s, gy0 = y + g.y0 * s, gx1 = px + g.x1 * s, gy1 = y + g.y1 * s;
                    auto &v = uiBatches.back().verts;
                    UiVertex a{gx0, gy0, g.u0, g.v0, gc}, b{gx1, gy0, g.u1, g.v0, gc};
                    UiVertex c{gx1, gy1, g.u1, g.v1, gc}, d{gx0, gy1, g.u0, g.v1, gc};
                    v.push_back(a); v.push_back(b); v.push_back(c);
                    v.push_back(a); v.push_back(c); v.push_back(d);
                }
                px += g.advance * s;
            }
            charIdx++;
        }
        if (ln == curLine && curCol >= charIdx && editorFocused) {
            emitRect(px, y + 1, 2.0f, lh - 2, Col::accent());
        }
    }

    // Bottom script tabs bar
    pushBatch(true, tabsRect);
    emitRect(tabsRect.x, tabsRect.y, tabsRect.w, tabsRect.h, Col::panel());
    emitRect(tabsRect.x, tabsRect.y, tabsRect.w, 1, Col::border());
    float tx = tabsRect.x + 3 * density;
    float tfs = 8.0f * density;
    float plusW = 24.0f * density;
    for (const auto &sp : openScripts) {
        std::string tabLbl = sp;
        if (auto sl = tabLbl.find_last_of('/'); sl != std::string::npos) tabLbl = tabLbl.substr(sl + 1);
        float tw = std::max(54 * density, font.measureWidth(tabLbl, tfs) + 22 * density);
        if (tx + tw > tabsRect.x + tabsRect.w - plusW - 6 * density) break;
        bool act = (activeScript == sp);
        addBtn({tx, tabsRect.y + 2, tw - 15 * density, tabsH - 3}, tabLbl,
               act ? Col::btnAct() : Col::btn(), act ? Col::text() : Col::muted(), tfs, [this, sp]() {
                   openScript(sp);
               });
        addBtn({tx + tw - 15 * density, tabsRect.y + 2, 15 * density, tabsH - 3}, "x",
               act ? Col::btnAct() : Col::btn(), Col::muted(), tfs, [this, sp]() {
                   if (activeScript == sp) saveActiveScript();
                   openScripts.erase(std::remove(openScripts.begin(), openScripts.end(), sp), openScripts.end());
                   if (activeScript == sp) {
                       if (!openScripts.empty()) {
                           activeScript.clear();
                           openScript(openScripts.back());
                       } else {
                           activeScript.clear();
                           editorFocused = false;
                           if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
                           syncOrientation();
                       }
                   }
               });
        tx += tw + 3 * density;
    }
    addBtn({tabsRect.x + tabsRect.w - plusW - 3 * density, tabsRect.y + 2, plusW, tabsH - 3}, "+",
           Col::btn(), Col::accent(), 9.5f * density, [this]() {
               if (auto *sss = scene.findService("Scripts")) {
                   auto *s = scene.createNode(sss, "Script", "Script" + std::to_string(scene.nextId));
                   if (s) {
                       s->scriptPath = "scripts/" + s->name + ".obs";
                       openScript(s->scriptPath);
                       scene.saveToFile(currentProjectDir + "/scene.json");
                   }
               } else {
                   openScript("scripts/script_" + std::to_string(openScripts.size() + 1) + ".obs");
               }
           });

    // Bottom compact action bar (4 essential buttons only)
    pushBatch(true, barRect);
    emitRect(barRect.x, barRect.y, barRect.w, barRect.h, Col::header());
    const char *keys[] = {"Tab", "Undo", "Check", "Save"};
    float kx = barRect.x + 3 * density;
    float kw = (barRect.w - 6 * density) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        Rect kr{kx + i * kw, barRect.y + 2, kw - 2, barRect.h - 4};
        addBtn(kr, keys[i], Col::btn(), Col::text(), 8.2f * density, [this, i]() {
            if (i == 0) lc_editor_insert(std::string(prefTabSpaces, ' ').c_str());
            else if (i == 1) lc_editor_undo();
            else if (i == 2) {
                char *err = nullptr;
                int ok = obsidian_check(lc_editor_get_text(), activeScript.c_str(), &err);
                editorStatus = ok ? "Syntax OK" : (err ? err : "Syntax error");
                editorStatusCol = ok ? Col::text() : Col::muted();
                appendOutput(ok ? (activeScript + ": syntax OK") : editorStatus, editorStatusCol);
                if (err) obsidian_free(err);
            } else if (i == 3) {
                saveActiveScript();
                editorStatus = "Saved";
                editorStatusCol = Col::text();
                appendOutput("Saved " + activeScript, Col::text());
            }
        });
    }
}

void StudioApp::buildModal() {
    pushBatch(false, {});
    emitRect(0, 0, width, height, StudioFont::rgba(0, 0, 0, 165));
    addHitBox({0, 0, width, height}, [this]() {
        if (modal != ModalKind::TEXT_PROMPT) modal = ModalKind::NONE;
    });

    bool wideModal = (modal == ModalKind::SKY || modal == ModalKind::COLOR);
    float mw = std::min(width - 20 * density, wideModal ? 440.0f * density : 350.0f * density);
    float mh = std::min(height - 20 * density, wideModal ? 330.0f * density : 350.0f * density);
    if (modal == ModalKind::TEXT_PROMPT) mh = 132.0f * density;
    Rect box{(width - mw) * 0.5f, (height - mh) * 0.5f, mw, mh};
    emitRect(box.x, box.y, box.w, box.h, Col::panel());
    emitRectOutline(box.x, box.y, box.w, box.h, 1.5f, Col::border());
    addHitBox(box, []() {});

    float fs = 9.0f * density;
    float pad = 9.0f * density;
    float y = box.y + pad;

    auto closeBtn = [&](const std::string &title) {
        emitText(title, box.x + pad, y, 10.5f * density, Col::text(), mw - 44 * density);
        Rect rc{box.x + box.w - 30 * density, y - 3 * density, 24 * density, 24 * density};
        addBtn(rc, "X", Col::btn(), Col::muted(), fs, [this]() {
            modal = ModalKind::NONE;
            if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
        });
        y += 26.0f * density;
    };

    if (modal == ModalKind::INSERT) {
        closeBtn("Add Node (3D / Group / Logic)");
        const char *cls[] = {
            "Box", "Wedge", "Sphere", "Cylinder",
            "Plane", "GltfModel", "Model", "Folder",
            "StaticBody3D", "RigidBody3D", "CollisionBox", "CollisionSphere",
            "Area3D", "Camera3D", "DirectionalLight3D", "PointLight3D",
            "Script"
        };
        float cw = (mw - pad * 3) * 0.5f;
        for (int i = 0; i < 17; ++i) {
            float x = box.x + pad + (i % 2) * (cw + pad);
            float ry = y + (i / 2) * 22 * density;
            if (ry + 19 * density > box.y + box.h - 4) break;
            std::string cn = cls[i];
            addBtn({x, ry, cw, 19 * density}, cn, Col::btn(), Col::text(), 8.1f * density, [this, cn]() {
                if (cn == "GltfModel") {
                    modal = ModalKind::NONE;
                    if (callbacks.openModelPicker) callbacks.openModelPicker(currentProjectName, "");
                    return;
                }
                SceneNode *parent = insertParentId.empty() ? nullptr : scene.findById(insertParentId);
                if (!parent) {
                    if (cn == "Script") parent = scene.findService("Scripts");
                    else parent = scene.findService("World3D");
                }
                if (auto *created = scene.createNode(parent, cn, cn + std::to_string(scene.nextId))) {
                    selectedId = created->id;
                    if (cn == "Script") {
                        created->scriptPath = "scripts/" + created->name + ".obs";
                        openScript(created->scriptPath);
                    }
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = ModalKind::NONE;
            });
        }
    } else if (modal == ModalKind::NEW_PROJECT) {
        closeBtn("Create New Project");
        addBtn({box.x + pad, y, mw - pad * 2, 34 * density},
               "3D Example (Demo World)", Col::btnAct(), Col::text(), fs, [this]() {
                   promptTitle = "New 3D Example Name"; promptValue = "Demo3D"; promptTargetKey = "New3DExample";
                   modal = ModalKind::TEXT_PROMPT;
               });
        y += 42 * density;
        addBtn({box.x + pad, y, mw - pad * 2, 34 * density},
               "3D From Scratch (With Player)", Col::green(), Col::text(), fs, [this]() {
                   promptTitle = "New 3D Scratch Name"; promptValue = "MyGame3D"; promptTargetKey = "New3DScratch";
                   modal = ModalKind::TEXT_PROMPT;
               });
        y += 42 * density;
        addBtn({box.x + pad, y, mw - pad * 2, 34 * density},
               "2D Canvas (Obsidronium)", Col::btn(), Col::text(), fs, [this]() {
                   promptTitle = "New 2D Project Name"; promptValue = "Game2D"; promptTargetKey = "New2D";
                   modal = ModalKind::TEXT_PROMPT;
               });
    } else if (modal == ModalKind::REPARENT) {
        closeBtn("Move Into Folder / Model");
        std::vector<const SceneNode *> containers;
        scene.collectContainers(containers);
        for (size_t i = 0; i < containers.size(); ++i) {
            float ry = y + i * 26 * density;
            if (ry + 22 * density > box.y + box.h - 4) break;
            std::string dstId = containers[i]->id;
            std::string dstLabel = containers[i]->name + "  [" + containers[i]->className + "]";
            addBtn({box.x + pad, ry, mw - pad * 2, 22 * density}, dstLabel, Col::btn(), Col::text(), 8.5f * density, [this, dstId]() {
                std::vector<std::string> ids = multiSelectedIds;
                if (ids.empty() && !selectedId.empty()) ids.push_back(selectedId);
                for (const auto &id : ids) scene.reparentNode(id, dstId);
                multiSelectedIds.clear();
                multiSelectMode = false;
                scene.saveToFile(currentProjectDir + "/scene.json");
                modal = ModalKind::NONE;
            });
        }
    } else if (modal == ModalKind::COLOR) {
        std::string title = (promptTargetKey == "SkyTop") ? "Sky Top Color"
                          : ((promptTargetKey == "SkyBot") ? "Sky Horizon Color" : "Object Color");
        closeBtn(title);
        float areaH = (box.y + box.h - pad) - y;
        float barW = 20.0f * density;
        float rightW = std::max(106.0f * density, mw * 0.34f);
        float maxRadW = (mw - pad * 4 - barW - rightW) * 0.5f;
        float rad = std::max(34.0f * density, std::min(areaH * 0.48f, maxRadW));
        float cx = box.x + pad + rad;
        float cy = y + areaH * 0.5f;
        Rect valBar{cx + rad + pad, cy - rad, barW, rad * 2.0f};
        emitColorWheel(cx, cy, rad, valBar);

        float rx = valBar.x + valBar.w + pad;
        float rw = box.x + box.w - pad - rx;
        float ry = y + 2 * density;
        uint32_t curArgb = wheelArgb();
        emitRect(rx, ry, rw, 28 * density, Col::argbToUi(curArgb));
        emitRectOutline(rx, ry, rw, 28 * density, 1.0f, Col::border());
        ry += 34 * density;

        addBtn({rx, ry, rw, 24 * density}, "Hex " + fmtHexColor(curArgb), Col::btn(), Col::text(), 8.3f * density, [this, curArgb]() {
            promptTitle = "Custom Hex (#RRGGBB)";
            promptValue = fmtHexColor(curArgb);
            promptExtra = "COLOR";
            promptTargetKey = "HexColor";
            modal = ModalKind::TEXT_PROMPT;
        });
        ry += 30 * density;

        static const uint32_t quick[6] = {
            0xFFF8FAFC, 0xFFDC4C46, 0xFF3B82F6,
            0xFF22C55E, 0xFFFACC15, 0xFF1E293B
        };
        float sw = (rw - 8 * density) / 3.0f;
        for (int i = 0; i < 6; ++i) {
            Rect sr{rx + (i % 3) * (sw + 4 * density), ry + (i / 3) * 24 * density, sw, 20 * density};
            emitRect(sr.x, sr.y, sr.w, sr.h, Col::argbToUi(quick[i]));
            emitRectOutline(sr.x, sr.y, sr.w, sr.h, 1.0f, Col::border());
            uint32_t cval = quick[i];
            addHitBox(sr, [this, cval]() {
                syncWheelFromArgb(cval);
                applyWheelColor(true);
            });
        }
        ry += 54 * density;
        addBtn({rx, std::min(ry, box.y + box.h - pad - 24 * density), rw, 24 * density},
               "Done", Col::btnAct(), Col::text(), fs, [this]() {
                   applyWheelColor(true);
                   if (promptTargetKey == "SkyTop" || promptTargetKey == "SkyBot") {
                       promptTargetKey.clear();
                       modal = ModalKind::SKY;
                   } else {
                       modal = ModalKind::NONE;
                   }
               });
    } else if (modal == ModalKind::SKY) {
        closeBtn("Sky & Atmosphere");
        SceneNode *env = scene.findEnvironment();
        struct SkyDef { const char *name; uint32_t top, bot; float ct; };
        static const SkyDef presets[] = {
            {"Day",      0xFF1E6EC4, 0xFF8CC4F2, 14.0f},
            {"Sunset",   0xFF4A2559, 0xFFF97316, 18.5f},
            {"Night",    0xFF0A1224, 0xFF1E2B47, 23.0f},
            {"Overcast", 0xFF64748B, 0xFFCBD5E1, 12.0f},
            {"Space",    0xFF03050C, 0xFF111827,  0.0f}
        };
        // Row 1: 5 presets
        float pw = (mw - pad * 2 - 4 * 4 * density) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            float x = box.x + pad + i * (pw + 4 * density);
            bool cur = env && env->skyPreset == presets[i].name;
            addBtn({x, y, pw, 22 * density}, presets[i].name, cur ? Col::btnAct() : Col::btn(), Col::text(), 8.0f * density, [this, i]() {
                if (auto *e = scene.findEnvironment()) {
                    e->skyPreset = presets[i].name;
                    e->skyTopColor = presets[i].top;
                    e->skyBotColor = presets[i].bot;
                    e->clockTime = presets[i].ct;
                    syncWheelFromArgb(skyEditBot ? e->skyBotColor : e->skyTopColor);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            });
        }
        y += 26 * density;

        // Row 2: Target switch (Sky Top / Horizon) + ClockTime
        uint32_t topC = env ? env->skyTopColor : 0xFF1E6EC4;
        uint32_t botC = env ? env->skyBotColor : 0xFF8CC4F2;
        float ct = env ? env->clockTime : 14.0f;
        float tw = (mw - pad * 2 - 8 * density) / 3.0f;
        addBtn({box.x + pad, y, tw, 22 * density}, "Sky Top",
               !skyEditBot ? Col::btnAct() : Col::btn(), Col::text(), 8.2f * density, [this, topC]() {
                   skyEditBot = false;
                   syncWheelFromArgb(topC);
               });
        emitRect(box.x + pad + tw - 14 * density, y + 4 * density, 10 * density, 14 * density, Col::argbToUi(topC));

        addBtn({box.x + pad + tw + 4 * density, y, tw, 22 * density}, "Horizon",
               skyEditBot ? Col::btnAct() : Col::btn(), Col::text(), 8.2f * density, [this, botC]() {
                   skyEditBot = true;
                   syncWheelFromArgb(botC);
               });
        emitRect(box.x + pad + tw * 2 + 4 * density - 14 * density, y + 4 * density, 10 * density, 14 * density, Col::argbToUi(botC));

        addBtn({box.x + pad + (tw + 4 * density) * 2, y, tw - 44 * density, 22 * density},
               "Time: " + fmtNum(ct) + "h", Col::btn(), Col::text(), 7.8f * density, [this, ct]() {
                   promptTitle = "ClockTime (0..24)"; promptValue = fmtNum(ct);
                   promptExtra = "SKY"; promptTargetKey = "ClockTime"; modal = ModalKind::TEXT_PROMPT;
               });
        addBtn({box.x + pad + (tw + 4 * density) * 2 + tw - 42 * density, y, 20 * density, 22 * density},
               "-", Col::btn(), Col::text(), 8.5f * density, [this]() {
                   if (auto *e = scene.findEnvironment()) {
                       e->clockTime = std::fmod(e->clockTime + 23.0f, 24.0f);
                       applyClockTimeToEnv(e, true);
                       syncWheelFromArgb(skyEditBot ? e->skyBotColor : e->skyTopColor);
                       scene.saveToFile(currentProjectDir + "/scene.json");
                   }
               });
        addBtn({box.x + pad + (tw + 4 * density) * 2 + tw - 20 * density, y, 20 * density, 22 * density},
               "+", Col::btn(), Col::text(), 8.5f * density, [this]() {
                   if (auto *e = scene.findEnvironment()) {
                       e->clockTime = std::fmod(e->clockTime + 1.0f, 24.0f);
                       applyClockTimeToEnv(e, true);
                       syncWheelFromArgb(skyEditBot ? e->skyBotColor : e->skyTopColor);
                       scene.saveToFile(currentProjectDir + "/scene.json");
                   }
               });
        y += 28 * density;

        // Row 3: Live HSV Color Wheel + Brightness bar + Gradient Preview & Hex input
        float areaH = (box.y + box.h - pad) - y;
        float barW = 20.0f * density;
        float rightW = std::max(106.0f * density, mw * 0.34f);
        float maxRadW = (mw - pad * 4 - barW - rightW) * 0.5f;
        float rad = std::max(34.0f * density, std::min(areaH * 0.48f, maxRadW));
        float cx = box.x + pad + rad;
        float cy = y + areaH * 0.5f;
        Rect valBar{cx + rad + pad, cy - rad, barW, rad * 2.0f};
        emitColorWheel(cx, cy, rad, valBar);

        float rx = valBar.x + valBar.w + pad;
        float rw = box.x + box.w - pad - rx;
        float ry = y;
        // Sky gradient preview box (top -> horizon)
        float prevH = std::max(38.0f * density, areaH * 0.34f);
        {
            if (uiBatches.empty()) pushBatch(false, {});
            auto &v = uiBatches.back().verts;
            float u = 1.5f / StudioFont::kAtlasSize;
            uint32_t uiT = Col::argbToUi(topC), uiB = Col::argbToUi(botC);
            v.push_back({rx, ry, u, u, uiT});
            v.push_back({rx + rw, ry, u, u, uiT});
            v.push_back({rx + rw, ry + prevH, u, u, uiB});
            v.push_back({rx, ry, u, u, uiT});
            v.push_back({rx + rw, ry + prevH, u, u, uiB});
            v.push_back({rx, ry + prevH, u, u, uiB});
            emitRectOutline(rx, ry, rw, prevH, 1.0f, Col::border());
        }
        ry += prevH + 6 * density;

        uint32_t activeC = skyEditBot ? botC : topC;
        addBtn({rx, ry, rw, 24 * density}, "Hex " + fmtHexColor(activeC), Col::btn(), Col::text(), 8.2f * density, [this, activeC]() {
            promptTitle = skyEditBot ? "Horizon Hex (#RRGGBB)" : "Sky Top Hex (#RRGGBB)";
            promptValue = fmtHexColor(activeC);
            promptExtra = "SKY";
            promptTargetKey = "HexColor";
            modal = ModalKind::TEXT_PROMPT;
        });
        ry += 29 * density;
        addBtn({rx, std::min(ry, box.y + box.h - pad - 24 * density), rw, 24 * density},
               "Done", Col::btnAct(), Col::text(), fs, [this]() {
                   applyWheelColor(true);
                   modal = ModalKind::NONE;
               });
    } else if (modal == ModalKind::MATERIAL) {
        closeBtn("Material");
        const char *mats[] = {"Plastic", "SmoothPlastic", "Slate", "Metal", "Wood", "Neon", "Glass", "Brick"};
        float cw = (mw - pad * 3) * 0.5f;
        for (int i = 0; i < 8; ++i) {
            float x = box.x + pad + (i % 2) * (cw + pad);
            float ry = y + (i / 2) * 28 * density;
            std::string mname = mats[i];
            addBtn({x, ry, cw, 24 * density}, mname, Col::btn(), Col::text(), fs, [this, mname]() {
                if (auto *n = selectedNode()) {
                    n->material = mname;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = ModalKind::NONE;
            });
        }
    } else if (modal == ModalKind::SHAPE) {
        closeBtn("Mesh Shape & Geometry");
        const char *sh[] = {"Box", "Wedge", "Sphere", "Cylinder", "Plane"};
        float sw = (mw - pad * 4) / 3.0f;
        for (int i = 0; i < 5; ++i) {
            std::string sname = sh[i];
            float rx = box.x + pad + (i % 3) * (sw + pad);
            float ry = y + (i / 3) * 26 * density;
            bool cur = selectedNode() && selectedNode()->shape() == sname;
            addBtn({rx, ry, sw, 22 * density}, sname, cur ? Col::btnAct() : Col::btn(), Col::text(), fs, [this, sname]() {
                if (auto *n = selectedNode()) {
                    n->setShape(sname);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            });
        }
        y += 58 * density;
        emitText("Smoothness / Polygons (Circle / Sphere / Cylinder)", box.x + pad, y, 7.8f * density, Col::muted(), mw - pad * 2);
        y += 14 * density;
        static const int segOpts[] = {8, 12, 16, 24, 32, 48};
        float pw = (mw - pad * 2 - 5 * 3 * density) / 6.0f;
        for (int i = 0; i < 6; ++i) {
            int sv = segOpts[i];
            bool cur = selectedNode() && selectedNode()->segments == sv;
            addBtn({box.x + pad + i * (pw + 3 * density), y, pw, 22 * density}, std::to_string(sv),
                   cur ? Col::btnAct() : Col::btn(), Col::text(), 8.0f * density, [this, sv]() {
                       if (auto *n = selectedNode()) {
                           n->segments = sv;
                           scene.saveToFile(currentProjectDir + "/scene.json");
                       }
                   });
        }
        y += 30 * density;
        emitText("Corner Roundness (Box / Square)", box.x + pad, y, 7.8f * density, Col::muted(), mw - pad * 2);
        y += 14 * density;
        static const float radOpts[] = {0.0f, 0.1f, 0.2f, 0.35f, 0.48f};
        float rw = (mw - pad * 2 - 4 * 3 * density) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            float rv = radOpts[i];
            bool cur = selectedNode() && std::fabs(selectedNode()->cornerRadius - rv) < 0.03f;
            addBtn({box.x + pad + i * (rw + 3 * density), y, rw, 22 * density}, fmtNum(rv),
                   cur ? Col::btnAct() : Col::btn(), Col::text(), 8.0f * density, [this, rv]() {
                       if (auto *n = selectedNode()) {
                           n->cornerRadius = rv;
                           scene.saveToFile(currentProjectDir + "/scene.json");
                       }
                   });
        }
    } else if (modal == ModalKind::SETTINGS) {
        closeBtn("Studio & Font Settings");
        auto save = [this]() {
            if (callbacks.savePrefs) callbacks.savePrefs(prefFontFamily, prefFontSize, prefTheme, prefTabSpaces, prefLineNumbers, prefCustomFontPath);
        };
        const char *famNames[] = {"Mono", "Sans", "Serif", "Bold"};
        addBtn({box.x + pad, y, mw - pad * 2, 24 * density},
               std::string("Font Family: ") + famNames[prefFontFamily & 3], Col::btn(), Col::text(), fs, [this, save]() {
                   prefFontFamily = (prefFontFamily + 1) % 4;
                   applyFont(prefCustomFontPath, prefFontFamily);
                   save();
               });
        y += 28 * density;
        addBtn({box.x + pad, y, mw - pad * 2, 24 * density},
               "Import Custom .TTF / .OTF Font", Col::btnAct(), Col::text(), fs, [this]() {
                   if (callbacks.openFontPicker) callbacks.openFontPicker();
               });
        y += 28 * density;
        if (!prefCustomFontPath.empty()) {
            addBtn({box.x + pad, y, mw - pad * 2, 22 * density},
                   "Reset Custom Font", Col::btn(), Col::muted(), fs, [this, save]() {
                       prefCustomFontPath.clear();
                       applyFont("", prefFontFamily);
                       save();
                   });
            y += 26 * density;
        }
        addBtn({box.x + pad, y, (mw - pad * 3) * 0.5f, 24 * density},
               "Size: " + std::to_string(prefFontSize) + "sp", Col::btn(), Col::text(), fs, [this, save]() {
                   prefFontSize = (prefFontSize >= 20) ? 11 : prefFontSize + 2;
                   save();
               });
        addBtn({box.x + pad + (mw - pad * 3) * 0.5f + pad, y, (mw - pad * 3) * 0.5f, 24 * density},
               prefTheme == 1 ? "Theme: Light" : "Theme: Dark", Col::btn(), Col::text(), fs, [this, save]() {
                   prefTheme = (prefTheme + 1) % 2;
                   save();
               });
        y += 28 * density;
        addBtn({box.x + pad, y, (mw - pad * 3) * 0.5f, 24 * density},
               prefLineNumbers ? "Line Nums: ON" : "Line Nums: OFF", Col::btn(), Col::text(), fs, [this, save]() {
                   prefLineNumbers = !prefLineNumbers;
                   save();
               });
        addBtn({box.x + pad + (mw - pad * 3) * 0.5f + pad, y, (mw - pad * 3) * 0.5f, 24 * density},
               "Tab: " + std::to_string(prefTabSpaces) + " sp", Col::btn(), Col::text(), fs, [this, save]() {
                   prefTabSpaces = (prefTabSpaces == 2) ? 4 : 2;
                   save();
               });
        y += 28 * density;
        addBtn({box.x + pad, y, mw - pad * 2, 24 * density},
               "2D Circle Polys: " + std::to_string(circleSegments), Col::btn(), Col::text(), fs, [this]() {
                   static const int cOpts[] = {8, 12, 16, 24, 32, 48, 64};
                   int next = 32;
                   for (int i = 0; i < 7; ++i) {
                       if (cOpts[i] == circleSegments) { next = cOpts[(i + 1) % 7]; break; }
                   }
                   circleSegments = next;
               });
    } else if (modal == ModalKind::TEXT_PROMPT) {
        closeBtn(promptTitle);
        Rect rIn{box.x + pad, y, mw - pad * 2, 25 * density};
        emitRect(rIn.x, rIn.y, rIn.w, rIn.h, Col::bg());
        emitRectOutline(rIn.x, rIn.y, rIn.w, rIn.h, 1.0f, Col::accent());
        emitText(promptValue + "_", rIn.x + 6 * density, rIn.y + 5 * density, fs, Col::text(), rIn.w - 12 * density);
        addHitBox(rIn, [this]() {
            if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(true);
        });
        y += 32 * density;
        Rect rOk{box.x + pad, y, mw - pad * 2, 25 * density};
        addBtn(rOk, "Apply", Col::btnAct(), Col::text(), fs, [this]() {
            if ((promptTargetKey == "New3DExample" || promptTargetKey == "New3D") && callbacks.createProject && !promptValue.empty()) {
                callbacks.createProject("3d_example", promptValue);
                modal = ModalKind::NONE;
                openProject(promptValue, true);
            } else if (promptTargetKey == "New3DScratch" && callbacks.createProject && !promptValue.empty()) {
                callbacks.createProject("3d_scratch", promptValue);
                modal = ModalKind::NONE;
                openProject(promptValue, true);
            } else if (promptTargetKey == "New2D" && callbacks.createProject && !promptValue.empty()) {
                callbacks.createProject("2d", promptValue);
                modal = ModalKind::NONE;
                openProject(promptValue, false);
            } else if (promptTargetKey == "RenameProj" && callbacks.renameProject && !promptValue.empty()) {
                callbacks.renameProject(promptExtra, promptValue);
                if (currentProjectName == promptExtra) {
                    currentProjectName = promptValue;
                    currentProjectDir = projectsRoot + "/" + promptValue;
                }
                modal = ModalKind::NONE;
            } else if (promptTargetKey == "ClockTime") {
                if (auto *env = scene.findEnvironment()) {
                    env->clockTime = std::clamp((float)std::atof(promptValue.c_str()), 0.0f, 24.0f);
                    applyClockTimeToEnv(env, true);
                    syncWheelFromArgb(skyEditBot ? env->skyBotColor : env->skyTopColor);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = (promptExtra == "SKY") ? ModalKind::SKY : ModalKind::NONE;
                promptExtra.clear();
            } else if (promptTargetKey == "NewAnim") {
                std::string clean;
                for (char ch : promptValue) {
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') clean.push_back(ch);
                }
                if (clean.empty()) clean = "anim";
                std::string dirPath = currentProjectDir + "/animations";
                mkdir(dirPath.c_str(), 0755);
                activeAnimFile = "animations/" + clean + ".anim.json";
                activeAnim = AnimClip{};
                activeAnim.name = clean;
                activeAnim.loop = true;
                if (auto *n = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId); n && n->is3DNode()) {
                    activeAnim.frames.push_back({0.0f, n->posX, n->posY, n->posZ, n->rotX, n->rotY, n->rotZ, n->sizeX, n->sizeY, n->sizeZ});
                    n->animPath = activeAnimFile;
                    n->animAutoPlay = true;
                    n->animLoop = true;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                activeAnim.saveToFile(currentProjectDir + "/" + activeAnimFile);
                showAnimator = true;
                modal = ModalKind::NONE;
            } else if (promptTargetKey == "NewGltfClip") {
                SceneNode *tn = animTargetNodeId.empty() ? selectedNode() : scene.findById(animTargetNodeId);
                if (tn && tn->className == "GltfModel") {
                    if (GltfModelAsset *gm = getOrLoadGltf(tn->modelPath)) {
                        int idx = gm->addAnimationClip(promptValue);
                        tn->modelAnimIndex = idx;
                        tn->animAutoPlay = true;
                        gm->saveAnimationsToFile();
                        scene.saveToFile(currentProjectDir + "/scene.json");
                        animTime = 0.0f;
                    }
                }
                showAnimator = true;
                modal = ModalKind::NONE;
            } else if (promptTargetKey == "BuildApkAppName") {
                std::string appName = promptValue.empty() ? currentProjectName : promptValue;
                modal = ModalKind::NONE;
                appendOutput("Compiling & building APK for '" + appName + "'...", Col::muted());
                if (callbacks.buildApk) {
                    callbacks.buildApk(currentProjectName, appName, currentIs3D);
                }
            } else if (promptTargetKey == "HexColor") {
                uint32_t parsed = 0;
                if (parseHexColor(promptValue, parsed)) {
                    syncWheelFromArgb(parsed);
                    if (promptExtra == "SKY") {
                        modal = ModalKind::SKY;
                        applyWheelColor(true);
                    } else {
                        modal = ModalKind::COLOR;
                        applyWheelColor(true);
                    }
                } else {
                    modal = (promptExtra == "SKY") ? ModalKind::SKY : ModalKind::COLOR;
                }
            } else if (auto *n = selectedNode()) {
                float a, b, c;
                if (promptTargetKey == "Rename" && !promptValue.empty()) n->name = promptValue;
                else if (promptTargetKey == "Position" && parseVec3(promptValue, a, b, c)) { n->posX = a; n->posY = b; n->posZ = c; }
                else if (promptTargetKey == "Size" && parseVec3(promptValue, a, b, c)) {
                    n->sizeX = std::max(0.2f, a); n->sizeY = std::max(0.2f, b); n->sizeZ = std::max(0.2f, c);
                } else if (promptTargetKey == "Rotation" && parseVec3(promptValue, a, b, c)) { n->rotX = a; n->rotY = b; n->rotZ = c; }
                else if (promptTargetKey == "Segments") n->segments = std::clamp(std::atoi(promptValue.c_str()), 6, 64);
                else if (promptTargetKey == "CornerRadius") n->cornerRadius = std::clamp((float)std::atof(promptValue.c_str()), 0.0f, 0.48f);
                else if (promptTargetKey == "Transparency") n->transparency = std::clamp((float)std::atof(promptValue.c_str()), 0.0f, 0.95f);
                else if (promptTargetKey == "Mass") n->mass = std::max(0.01f, (float)std::atof(promptValue.c_str()));
                else if (promptTargetKey == "Gravity") n->gravityScale = (float)std::atof(promptValue.c_str());
                else if (promptTargetKey == "FOV") n->fov = std::clamp((float)std::atof(promptValue.c_str()), 20.0f, 120.0f);
                else if (promptTargetKey == "Energy") n->energy = std::max(0.0f, (float)std::atof(promptValue.c_str()));
                else if (promptTargetKey == "ClockTime") n->clockTime = std::clamp((float)std::atof(promptValue.c_str()), 0.0f, 24.0f);
                else if (promptTargetKey == "Text") n->text = promptValue;
                scene.saveToFile(currentProjectDir + "/scene.json");
                modal = ModalKind::NONE;
            } else {
                modal = ModalKind::NONE;
            }
            if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
        });
    }
}

// =========================================================================
// Projection, Raycast, Touch & Keyboard Input
// =========================================================================
bool StudioApp::projectPoint(Vec3f w, float &sx, float &sy) const {
    float cx = viewProj[0] * w.x + viewProj[4] * w.y + viewProj[8]  * w.z + viewProj[12];
    float cy = viewProj[1] * w.x + viewProj[5] * w.y + viewProj[9]  * w.z + viewProj[13];
    float cw = viewProj[3] * w.x + viewProj[7] * w.y + viewProj[11] * w.z + viewProj[15];
    if (cw <= 0.1f) return false;
    float nx = cx / cw, ny = cy / cw;
    sx = rViewport.x + (nx * 0.5f + 0.5f) * rViewport.w;
    sy = rViewport.y + (ny * 0.5f + 0.5f) * rViewport.h;
    return true;
}

std::string StudioApp::raycastPart(float sx, float sy) {
    float nx = (sx - (rViewport.x + rViewport.w * 0.5f)) / focalLen;
    float ny = ((rViewport.y + rViewport.h * 0.5f) - sy) / focalLen;
    Vec3f dir = norm3(fwd + right * nx + up * ny);
    std::vector<const SceneNode *> nodes;
    scene.collect3DNodes(nodes);
    float bestT = 1e9f;
    std::string bestId;
    for (const auto *p : nodes) {
        if (p->locked) continue;
        float hx = p->sizeX * 0.5f, hy = p->sizeY * 0.5f, hz = p->sizeZ * 0.5f;
        float tmin = -1e9f, tmax = 1e9f;
        float ro[3] = {eye.x - p->posX, eye.y - p->posY, eye.z - p->posZ};
        float rd[3] = {dir.x, dir.y, dir.z};
        float he[3] = {hx, hy, hz};
        bool hit = true;
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(rd[a]) < 1e-6f) {
                if (ro[a] < -he[a] || ro[a] > he[a]) { hit = false; break; }
            } else {
                float t1 = (-he[a] - ro[a]) / rd[a];
                float t2 = ( he[a] - ro[a]) / rd[a];
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) { hit = false; break; }
            }
        }
        if (hit && tmax > 0.2f && tmin < bestT) {
            bestT = std::max(0.2f, tmin);
            bestId = p->id;
        }
    }
    return bestId;
}

void StudioApp::applyGizmoDrag(float dx, float dy) {
    auto *n = selectedNode();
    if (!n || (!n->is3DNode() && !n->isGroup()) || dragAxis < 0) return;
    float tipX = 0, tipY = 0, baseX = 0, baseY = 0;
    for (const auto &gt : gizmoTips) {
        if (gt.axis == dragAxis) { tipX = gt.sx; tipY = gt.sy; baseX = gt.bx; baseY = gt.by; break; }
    }
    float vx = tipX - baseX, vy = tipY - baseY;
    float vlen = std::hypot(vx, vy);
    float delta = (vlen > 4.0f) ? ((dx * vx + dy * vy) / vlen) * 0.05f : (dx - dy) * 0.04f;
    if (toolMode == ToolMode::MOVE) {
        float mx = (dragAxis == 0) ? delta : 0.0f;
        float my = (dragAxis == 1) ? delta : 0.0f;
        float mz = (dragAxis == 2) ? delta : 0.0f;
        if (n->isGroup()) StudioScene::moveNodeDelta(*n, mx, my, mz);
        else { n->posX += mx; n->posY += my; n->posZ += mz; }
    } else if (toolMode == ToolMode::SCALE && n->is3DNode()) {
        if (dragAxis == 0) n->sizeX = std::max(0.4f, n->sizeX + delta);
        else if (dragAxis == 1) n->sizeY = std::max(0.4f, n->sizeY + delta);
        else n->sizeZ = std::max(0.4f, n->sizeZ + delta);
    } else if (toolMode == ToolMode::ROTATE && n->is3DNode()) {
        float deg = delta * 18.0f;
        if (dragAxis == 0) n->rotX += deg;
        else if (dragAxis == 1) n->rotY += deg;
        else n->rotZ += deg;
    }
}

void StudioApp::onTouch(int action, int pointerIndex, int pointerCount, const int *ids, const float *xs, const float *ys) {
    if (pointerCount <= 0) return;
    float x0 = xs[0], y0 = ys[0];
    int baseAct = action & 0xFF;

    // Intercept top-right Close ('X') button (single-touch or multi-touch) before forwarding to game
    if (runningLua && !showProjectsTab && modal == ModalKind::NONE && activeScript.empty()) {
        int pi = (baseAct == 5 || baseAct == 6) ? std::max(0, std::min(pointerIndex, pointerCount - 1)) : 0;
        if (!playerOnly) {
            Rect rcHit = closePlayBtnRect();
            float pad = 10.0f * density;
            rcHit.x -= pad; rcHit.y -= pad; rcHit.w += pad * 2.0f; rcHit.h += pad * 2.0f;

            if ((baseAct == 0 || baseAct == 5) && rcHit.contains(xs[pi], ys[pi])) {
                closePointerId = ids[pi];
                if (baseAct == 0) {
                    downX = lastX = x0; downY = lastY = y0;
                    moved = false;
                    touchMode = 11;
                    dragAxis = -1;
                    lastPinch = -1;
                }
                return;
            }
            if ((baseAct == 1 || baseAct == 6) && closePointerId != -1 && ids[pi] == closePointerId) {
                bool inside = rcHit.contains(xs[pi], ys[pi]);
                closePointerId = -1;
                if (baseAct == 1) touchMode = 0;
                if (inside) { stopSim(); return; }
            }
            if (baseAct == 3) closePointerId = -1;
        } else {
            closePointerId = -1;
        }

        if (g_obsidianProj) {
            if (baseAct == 2) {
                for (int i = 0; i < pointerCount; ++i) {
                    if (ids[i] == closePointerId) continue;
                    if (rViewport.contains(xs[i], ys[i])) {
                        obsidian_project_touch(g_obsidianProj, ids[i], xs[i] - rViewport.x, ys[i] - rViewport.y, 1);
                    }
                }
            } else if (ids[pi] != closePointerId) {
                if (rViewport.contains(xs[pi], ys[pi])) {
                    int down = (baseAct == 0 || baseAct == 5) ? 1 : 0;
                    obsidian_project_touch(g_obsidianProj, ids[pi], xs[pi] - rViewport.x, ys[pi] - rViewport.y, down);
                }
            }
        }
        if (playerOnly) return;
    } else {
        closePointerId = -1;
    }

    auto updateWheelHS = [&](float px, float py, bool saveScene) {
        float wcx = rWheel.x + rWheel.w * 0.5f;
        float wcy = rWheel.y + rWheel.h * 0.5f;
        float wr = std::max(1.0f, rWheel.w * 0.5f);
        float dx = px - wcx, dy = py - wcy;
        float dist = std::hypot(dx, dy);
        float ang = std::atan2(dy, dx);
        if (ang < 0.0f) ang += 6.2831853f;
        wheelH = ang / 6.2831853f;
        wheelS = std::clamp(dist / wr, 0.0f, 1.0f);
        applyWheelColor(saveScene);
    };
    auto updateWheelVal = [&](float py, bool saveScene) {
        wheelV = std::clamp(1.0f - (py - rValBar.y) / std::max(1.0f, rValBar.h), 0.0f, 1.0f);
        applyWheelColor(saveScene);
    };

    if (baseAct == 0) {
        downX = lastX = x0; downY = lastY = y0;
        moved = false;
        touchMode = 0;
        dragAxis = -1;
        lastPinch = -1;

        if (modal == ModalKind::SKY || modal == ModalKind::COLOR) {
            float wcx = rWheel.x + rWheel.w * 0.5f;
            float wcy = rWheel.y + rWheel.h * 0.5f;
            float wr = rWheel.w * 0.5f;
            if (wr > 1.0f && std::hypot(x0 - wcx, y0 - wcy) <= wr + 8.0f * density) {
                touchMode = 12;
                updateWheelHS(x0, y0, false);
                return;
            }
            Rect vbHit{rValBar.x - 10.0f * density, rValBar.y - 8.0f * density,
                       rValBar.w + 20.0f * density, rValBar.h + 16.0f * density};
            if (rValBar.w > 1.0f && vbHit.contains(x0, y0)) {
                touchMode = 13;
                updateWheelVal(y0, false);
                return;
            }
        }

        if (modal == ModalKind::NONE) {
            if (rRibbon.contains(x0, y0)) touchMode = 5;
            else if (showProjectsTab && rViewport.contains(x0, y0)) touchMode = 10;
            else if (showExplorer && rExplorer.contains(x0, y0)) touchMode = 6;
            else if (showProperties && rProperties.contains(x0, y0)) touchMode = 7;
            else if (showOutput && rOutLog.contains(x0, y0)) touchMode = 9;
            else if (!activeScript.empty() && rViewport.contains(x0, y0)) touchMode = 8;
            else if (currentIs3D && activeScript.empty() && rViewport.contains(x0, y0) && !(runningLua && useCamera3DInPlay)) {
                for (const auto &gt : gizmoTips) {
                    if (std::hypot(x0 - gt.sx, y0 - gt.sy) <= 22.0f * density) {
                        touchMode = 3; dragAxis = gt.axis; break;
                    }
                }
                if (touchMode == 0) touchMode = 1;
            }
        }
        return;
    }

    if (baseAct == 5 && pointerCount >= 2 && rViewport.contains(x0, y0) && currentIs3D && activeScript.empty() && !showProjectsTab && !(runningLua && useCamera3DInPlay)) {
        touchMode = 2;
        lastPinch = std::hypot(xs[1] - xs[0], ys[1] - ys[0]);
        lastX = (xs[0] + xs[1]) * 0.5f;
        lastY = (ys[0] + ys[1]) * 0.5f;
        moved = true;
        return;
    }

    if (baseAct == 2) {
        if (touchMode == 12) { updateWheelHS(x0, y0, false); lastX = x0; lastY = y0; return; }
        if (touchMode == 13) { updateWheelVal(y0, false); lastX = x0; lastY = y0; return; }
        if (std::hypot(x0 - downX, y0 - downY) > 6.0f * density) moved = true;
        float dx = x0 - lastX, dy = y0 - lastY;
        if (touchMode == 5) ribbonScrollX = std::max(0.0f, ribbonScrollX - dx);
        else if (touchMode == 10) projectsScrollY = std::max(0.0f, projectsScrollY - dy);
        else if (touchMode == 6) explorerScrollY = std::max(0.0f, explorerScrollY - dy);
        else if (touchMode == 7) propsScrollY = std::max(0.0f, propsScrollY - dy);
        else if (touchMode == 8) editorScrollY = std::max(0.0f, editorScrollY - dy);
        else if (touchMode == 9) outputScrollY = std::max(0.0f, outputScrollY - dy);
        else if (touchMode == 3) applyGizmoDrag(dx, dy);
        else if (touchMode == 2 && pointerCount >= 2) {
            float pinch = std::hypot(xs[1] - xs[0], ys[1] - ys[0]);
            if (lastPinch > 0) {
                float scale = lastPinch / std::max(20.0f, pinch);
                float nextDist = camDist * scale;
                if (nextDist < 1.5f) {
                    float push = 1.5f - nextDist;
                    camTargetX += fwd.x * push;
                    camTargetY += fwd.y * push;
                    camTargetZ += fwd.z * push;
                    camDist = 1.5f;
                } else {
                    camDist = std::min(nextDist, 800.0f);
                }
            }
            lastPinch = pinch;
            float cx = (xs[0] + xs[1]) * 0.5f, cy = (ys[0] + ys[1]) * 0.5f;
            float pdx = cx - lastX, pdy = cy - lastY;
            camTargetX -= (right.x * pdx - up.x * pdy) * 0.04f;
            camTargetY -= (right.y * pdx - up.y * pdy) * 0.04f;
            camTargetZ -= (right.z * pdx - up.z * pdy) * 0.04f;
            lastX = cx; lastY = cy;
            return;
        } else if (touchMode == 1) {
            camYaw -= dx * 0.0075f;
            camPitch = std::clamp(camPitch + dy * 0.0075f, -1.2f, 1.35f);
        }
        lastX = x0; lastY = y0;
        return;
    }

    if (baseAct == 1 || baseAct == 3) {
        if (touchMode == 12) { updateWheelHS(x0, y0, true); touchMode = 0; return; }
        if (touchMode == 13) { updateWheelVal(y0, true); touchMode = 0; return; }
        if (touchMode == 3 && moved) scene.saveToFile(currentProjectDir + "/scene.json");
        if (!moved) {
            bool hit = false;
            for (int i = (int)hitBoxes.size() - 1; i >= 0; --i) {
                if (hitBoxes[(size_t)i].r.contains(x0, y0)) {
                    hitBoxes[(size_t)i].onTap();
                    hit = true;
                    break;
                }
            }
            if (!hit && modal == ModalKind::NONE) {
                if (!rCmdBar.contains(x0, y0)) commandFocused = false;
                if (!showProjectsTab && !activeScript.empty() && rViewport.contains(x0, y0)) {
                    editorFocused = true;
                    float fs = std::max(9.0f, (float)prefFontSize * 0.72f) * density;
                    float lh = fs * 1.28f;
                    float gutterW = prefLineNumbers ? 26.0f * density : 4.0f * density;
                    float codeTop = rViewport.y + 30.0f * density;
                    int tapLine = std::max(0, (int)((y0 - codeTop + editorScrollY) / lh));
                    float adv = std::max(4.0f, font.measureWidth("M", fs));
                    int tapCol = std::max(0, (int)((x0 - rViewport.x - gutterW) / adv));
                    lc_editor_set_cursor_line_col(tapLine, tapCol);
                    if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(true);
                } else if (!showProjectsTab && currentIs3D && activeScript.empty() && rViewport.contains(x0, y0) && !runningLua) {
                    editorFocused = false;
                    if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
                    std::string hitId = raycastPart(x0, y0);
                    if (!hitId.empty()) selectedId = hitId;
                }
            }
        }
        touchMode = 0; dragAxis = -1; lastPinch = -1;
    }
}

void StudioApp::onTextInput(const std::string &utf8) {
    if (utf8.empty()) return;
    if (modal == ModalKind::TEXT_PROMPT) promptValue += utf8;
    else if (commandFocused) commandText += utf8;
    else if (!activeScript.empty() && !showProjectsTab) {
        editorStatus.clear();
        lc_editor_insert(utf8.c_str());
    }
}

void StudioApp::onKeyCode(int keyCode) {
    auto popUtf8 = [](std::string &s) {
        while (!s.empty() && ((uint8_t)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty()) s.pop_back();
    };
    if (modal == ModalKind::TEXT_PROMPT) {
        if (keyCode == 67) popUtf8(promptValue);
        else if (keyCode == 66 && callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
        return;
    }
    if (commandFocused) {
        if (keyCode == 67) popUtf8(commandText);
        else if (keyCode == 66) {
            std::string cmd = commandText;
            commandText.clear();
            execCommand(cmd);
        }
        return;
    }
    if (!activeScript.empty() && !showProjectsTab) {
        if (keyCode == 67) lc_editor_backspace();
        else if (keyCode == 66) lc_editor_newline();
        else if (keyCode == 61) lc_editor_insert(std::string(prefTabSpaces, ' ').c_str());
        else if (keyCode == 19) lc_editor_move_cursor(0, -1);
        else if (keyCode == 20) lc_editor_move_cursor(0, 1);
        else if (keyCode == 21) lc_editor_move_cursor(-1, 0);
        else if (keyCode == 22) lc_editor_move_cursor(1, 0);
    }
}
