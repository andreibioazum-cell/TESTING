#define STB_TRUETYPE_IMPLEMENTATION
#include "studio_app.hpp"
#include "luac.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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

static void *g_luacProj = nullptr;

namespace Col {
static inline uint32_t bg()       { return StudioFont::rgba(23, 25, 30); }
static inline uint32_t panel()    { return StudioFont::rgba(31, 34, 41); }
static inline uint32_t header()   { return StudioFont::rgba(38, 42, 51); }
static inline uint32_t btn()      { return StudioFont::rgba(46, 51, 63); }
static inline uint32_t btnAct()   { return StudioFont::rgba(59, 130, 246); }
static inline uint32_t green()    { return StudioFont::rgba(34, 197, 94); }
static inline uint32_t red()      { return StudioFont::rgba(239, 68, 68); }
static inline uint32_t border()   { return StudioFont::rgba(55, 62, 77); }
static inline uint32_t text()     { return StudioFont::rgba(232, 236, 243); }
static inline uint32_t muted()    { return StudioFont::rgba(148, 163, 184); }
static inline uint32_t accent()   { return StudioFont::rgba(96, 165, 250); }
static inline uint32_t selRow()   { return StudioFont::rgba(36, 84, 158); }
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
}

void StudioApp::applyFont(const std::string &customPath, int family) {
    prefCustomFontPath = customPath;
    prefFontFamily = family;
    font.loadBestSystemOrCustom(customPath, family);
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
    outputLines.clear();
    editorScrollY = 0;
    explorerScrollY = 0;
    propsScrollY = 0;

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
        openScript("main.lua");
        appendOutput("Loaded 2D LuaC project: " + projName, Col::muted());
    }
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
    g_luacProj = luac_project_open(currentProjectDir.c_str(), &err);
    if (!g_luacProj) {
        appendOutput(std::string("Runtime error: ") + (err ? err : "failed"), Col::red());
        if (err) luac_free(err);
        return;
    }
    if (err) luac_free(err);
    luac_project_set_viewport(g_luacProj, (int)std::max(64.0f, rViewport.w), (int)std::max(64.0f, rViewport.h));
    runningLua = true;
    showProjectsTab = false;
    activeScript.clear();
    appendOutput(currentIs3D ? "3D Scene running." : "2D Canvas running.", Col::green());
}

void StudioApp::stopSim() {
    if (g_luacProj) {
        luac_project_close(g_luacProj);
        g_luacProj = nullptr;
    }
    bool wasRun = runningLua;
    runningLua = false;
    if (currentIs3D && !prePlaySceneJson.empty()) {
        JsonVal j = MiniJson::parse(prePlaySceneJson);
        if (j.type == JsonVal::J_OBJ && j.get("root")) {
            std::ofstream out(currentProjectDir + "/scene.json", std::ios::binary | std::ios::trunc);
            if (out) out << prePlaySceneJson;
            scene = StudioScene::loadOrCreate(currentProjectDir);
        }
        prePlaySceneJson.clear();
    }
    if (wasRun) appendOutput("Stopped.", Col::muted());
}

void StudioApp::syncFromLua() {
    if (!g_luacProj || !currentIs3D) return;
    char *raw = luac_project_exec(g_luacProj, "return sdk and sdk.sync() or nil");
    if (!raw) return;
    JsonVal res = MiniJson::parse(raw);
    luac_free(raw);
    std::string out = res.s("output");
    if (!out.empty()) appendOutput(out, Col::text());
    std::string err = res.s("error");
    if (!err.empty()) appendOutput(err, Col::red());
    std::string val = res.s("value");
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
    if (!g_luacProj) {
        char *err = nullptr;
        g_luacProj = luac_project_open(currentProjectDir.c_str(), &err);
        if (!g_luacProj) {
            appendOutput(err ? err : "failed to open Lua runtime", Col::red());
            if (err) luac_free(err);
            return;
        }
        if (err) luac_free(err);
    }
    char *raw = luac_project_exec(g_luacProj, cmd.c_str());
    if (!raw) return;
    JsonVal res = MiniJson::parse(raw);
    luac_free(raw);
    std::string out = res.s("output");
    if (!out.empty()) appendOutput(out, Col::text());
    std::string val = res.s("value");
    if (!val.empty() && val != "nil") appendOutput("= " + val, Col::green());
    std::string err = res.s("error");
    if (!err.empty()) appendOutput(err, Col::red());
    syncFromLua();
}

void StudioApp::step(float dt) {
    if (runningLua && g_luacProj) {
        luac_project_set_viewport(g_luacProj, (int)std::max(64.0f, rViewport.w), (int)std::max(64.0f, rViewport.h));
        char *raw = luac_project_step(g_luacProj, dt);
        if (raw) {
            if (!currentIs3D) last2DFrameJson = raw;
            JsonVal res = MiniJson::parse(raw);
            luac_free(raw);
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
    }
}

// =========================================================================
// Layout (responsive in both Portrait & Landscape!)
// =========================================================================
void StudioApp::computeLayout() {
    bool landscape = width > height;
    float topTabsH = std::round(22.0f * density);
    float ribbonH  = std::round(32.0f * density);
    float docTabsH = std::round(20.0f * density);
    float rightW   = (!showProjectsTab && (showExplorer || showProperties))
        ? std::min(std::round(146.0f * density), std::round(width * (landscape ? 0.24f : 0.36f)))
        : 0.0f;
    float botH     = showOutput
        ? std::min(std::round(64.0f * density), std::round(height * (landscape ? 0.22f : 0.16f)))
        : std::round(22.0f * density);

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
    if (showOutput) {
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

void StudioApp::emitCircle(float cx, float cy, float r, uint32_t col) {
    if (uiBatches.empty()) pushBatch(false, {});
    auto &v = uiBatches.back().verts;
    float u = 1.5f / StudioFont::kAtlasSize;
    const int segs = 20;
    for (int i = 0; i < segs; ++i) {
        float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        v.push_back({cx, cy, u, u, col});
        v.push_back({cx + std::cos(a0) * r, cy + std::sin(a0) * r, u, u, col});
        v.push_back({cx + std::cos(a1) * r, cy + std::sin(a1) * r, u, u, col});
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
    hitBoxes.push_back({r, std::move(cb)});
}

// =========================================================================
// Frame Build
// =========================================================================
void StudioApp::buildFrame(float w, float h, float d) {
    width = w; height = h; density = std::max(1.5f, d);
    computeLayout();
    uiBatches.clear();
    hitBoxes.clear();
    frame3DMesh.clear();
    frame3DLines.clear();
    frame3DOverlayLines.clear();
    gizmoTips.clear();

    has3DViewportVisible = (!showProjectsTab && activeScript.empty() && currentIs3D);

    pushBatch(false, {});
    emitRect(0, 0, width, height, Col::bg());

    if (showProjectsTab) {
        buildProjectsTab(rViewport);
    } else if (!activeScript.empty()) {
        buildCodeEditor(rViewport);
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
        std::string badge = runningLua ? "PLAY" : "3D";
        float bw = font.measureWidth(badge, 10.0f * density) + 12.0f * density;
        Rect rb{rViewport.x + rViewport.w - bw - 6 * density, rViewport.y + 6 * density, bw, 16 * density};
        emitRect(rb.x, rb.y, rb.w, rb.h, StudioFont::rgba(15, 23, 42, 180));
        emitTextCentered(badge, rb, 10.0f * density, runningLua ? Col::green() : Col::accent());
    } else {
        build2DScene(rViewport);
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
    Rect b3d{area.x + pad, area.y + 32 * density, 118 * density, 26 * density};
    Rect b2d{b3d.x + b3d.w + 8 * density, b3d.y, 118 * density, 26 * density};
    addBtn(b3d, "+ New 3D World", Col::btnAct(), Col::text(), fs, [this]() {
        promptTitle = "New 3D World Name"; promptValue = "World3D"; promptTargetKey = "New3D";
        modal = ModalKind::TEXT_PROMPT;
    });
    addBtn(b2d, "+ New 2D Canvas", Col::btn(), Col::text(), fs, [this]() {
        promptTitle = "New 2D Project Name"; promptValue = "Game2D"; promptTargetKey = "New2D";
        modal = ModalKind::TEXT_PROMPT;
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
    if (ct >= 6.0f && ct <= 18.0f) {
        skyTop[0] = 0.12f; skyTop[1] = 0.43f; skyTop[2] = 0.77f;
        skyBot[0] = 0.55f; skyBot[1] = 0.77f; skyBot[2] = 0.95f;
    } else {
        skyTop[0] = 0.04f; skyTop[1] = 0.07f; skyTop[2] = 0.14f;
        skyBot[0] = 0.12f; skyBot[1] = 0.17f; skyBot[2] = 0.28f;
    }

    // Check DirectionalLight3D in scene
    sunDir[0] = 0.55f; sunDir[1] = 0.85f; sunDir[2] = 0.40f; sunDir[3] = 0.0f;
    std::vector<const SceneNode *> nodes3D;
    scene.collect3DNodes(nodes3D);
    for (const auto *n : nodes3D) {
        if (n->className == "DirectionalLight3D") {
            Vec3f dir = norm3({n->posX, std::max(1.0f, n->posY), n->posZ});
            sunDir[0] = dir.x; sunDir[1] = dir.y; sunDir[2] = dir.z;
            break;
        }
    }

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
    float zn = 0.4f, zf = 400.0f;

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

    // 3D solid meshes
    std::vector<const SceneNode *> meshes;
    scene.collectMeshes(meshes);
    for (const auto *p : meshes) {
        if (p->transparency >= 0.99f) continue;
        StudioScene::appendPartMesh(*p, frame3DMesh);
    }

    auto addLine = [&](std::vector<MeshVertex> &dst, Vec3f a, Vec3f b, uint32_t col) {
        dst.push_back({a.x, a.y, a.z, 0, 1, 0, col});
        dst.push_back({b.x, b.y, b.z, 0, 1, 0, col});
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
        uint32_t gCol = StudioFont::rgba(75, 85, 102, 130);
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

        if (const auto *sel = selectedNode(); sel && sel->is3DNode()) {
            addBoxWire(frame3DOverlayLines, *sel, 0.08f, StudioFont::rgba(56, 189, 248, 255));
            if (toolMode != ToolMode::SELECT) {
                Vec3f orig{sel->posX, sel->posY, sel->posZ};
                float gLen = std::max(3.0f, std::max({sel->sizeX, sel->sizeY, sel->sizeZ}) * 0.65f + 1.6f);
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

void StudioApp::build2DScene(const Rect &area) {
    pushBatch(true, area);
    emitRect(area.x, area.y, area.w, area.h, StudioFont::rgba(15, 23, 42));
    if (last2DFrameJson.empty()) {
        emitTextCentered("2D Canvas — Tap Play to run " + currentProjectName, area, 12.0f * density, Col::muted());
        return;
    }
    JsonVal j = MiniJson::parse(last2DFrameJson);
    if (const JsonVal *cc = j.get("clear_color"); cc && cc->type == JsonVal::J_ARR && cc->arr.size() >= 3) {
        emitRect(area.x, area.y, area.w, area.h,
                 StudioFont::rgba((uint8_t)cc->arr[0].num, (uint8_t)cc->arr[1].num, (uint8_t)cc->arr[2].num));
    }
    if (const JsonVal *cmds = j.get("draw_cmds"); cmds && cmds->type == JsonVal::J_ARR) {
        for (const auto &c : cmds->arr) {
            std::string k = c.s("kind");
            uint32_t col = StudioFont::rgba((uint8_t)c.d("r",255), (uint8_t)c.d("g",255), (uint8_t)c.d("b",255), (uint8_t)c.d("a",255));
            float x = area.x + (float)c.d("x"), y = area.y + (float)c.d("y");
            if (k == "rect") emitRect(x, y, (float)c.d("w"), (float)c.d("h"), col);
            else if (k == "circle") emitCircle(x, y, (float)c.d("radius", 6), col);
            else if (k == "text") emitText(c.s("text"), x, y, (float)c.d("size", 18), col);
        }
    }
    if (const JsonVal *ents = j.get("entities"); ents && ents->type == JsonVal::J_ARR) {
        for (const auto &e : ents->arr) {
            uint32_t col = StudioFont::rgba((uint8_t)e.d("r",100), (uint8_t)e.d("g",220), (uint8_t)e.d("b",120));
            emitRect(area.x + (float)e.d("x"), area.y + (float)e.d("y"), (float)e.d("w", 16), (float)e.d("h", 16), col);
        }
    }
    if (const JsonVal *txts = j.get("texts"); txts && txts->type == JsonVal::J_ARR) {
        for (const auto &t : txts->arr) {
            uint32_t col = StudioFont::rgba((uint8_t)t.d("r",255), (uint8_t)t.d("g",255), (uint8_t)t.d("b",255));
            emitText(t.s("text"), area.x + (float)t.d("x"), area.y + (float)t.d("y"), (float)t.d("size", 18), col);
        }
    }
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
        ribBtn("+ 3D World", false, Col::btnAct(), [this]() {
            promptTitle = "New 3D World Name"; promptValue = "World3D"; promptTargetKey = "New3D";
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
        ribBtn("Select", toolMode == ToolMode::SELECT, 0, [this]() { toolMode = ToolMode::SELECT; });
        ribBtn("Move", toolMode == ToolMode::MOVE, 0, [this]() { toolMode = ToolMode::MOVE; });
        ribBtn("Scale", toolMode == ToolMode::SCALE, 0, [this]() { toolMode = ToolMode::SCALE; });
        ribBtn("Rotate", toolMode == ToolMode::ROTATE, 0, [this]() { toolMode = ToolMode::ROTATE; });
        ribBtn("+ Node", false, 0, [this]() {
            insertParentId = selectedId;
            modal = ModalKind::INSERT;
        });
        ribBtn("Color", false, 0, [this]() { if (selectedNode()) modal = ModalKind::COLOR; });
        ribBtn("Material", false, 0, [this]() { if (selectedNode()) modal = ModalKind::MATERIAL; });
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
                    s->scriptPath = "scripts/" + s->name + ".lua";
                    openScript(s->scriptPath);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
            } else {
                openScript("main.lua");
            }
        });
        ribBtn("main.lua", activeScript == "main.lua", 0, [this]() { openScript("main.lua"); });
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
            if (activeScript == sp) { saveActiveScript(); activeScript.clear(); }
            openScripts.erase(std::remove(openScripts.begin(), openScripts.end(), sp), openScripts.end());
        });
        x += w + 2 * density;
    }
}

void StudioApp::buildRightDock() {
    if (rRightDock.w <= 1.0f) return;
    float fs = 8.3f * density;
    float hdrH = 15.0f * density;
    float rowH = 17.0f * density;

    if (showExplorer && rExplorer.h > 1.0f) {
        pushBatch(true, rExplorer);
        emitRect(rExplorer.x, rExplorer.y, rExplorer.w, rExplorer.h, Col::panel());
        emitRect(rExplorer.x, rExplorer.y, 1, rExplorer.h, Col::border());
        emitRect(rExplorer.x, rExplorer.y, rExplorer.w, hdrH, Col::header());
        emitText("Scene Tree", rExplorer.x + 5 * density, rExplorer.y + 2 * density, fs, Col::text());
        Rect rAdd{rExplorer.x + rExplorer.w - 18 * density, rExplorer.y + 1, 16 * density, hdrH - 2};
        addBtn(rAdd, "+", Col::btn(), Col::accent(), fs, [this]() {
            insertParentId = selectedId;
            modal = ModalKind::INSERT;
        });

        Rect listArea{rExplorer.x + 1, rExplorer.y + hdrH, rExplorer.w - 1, rExplorer.h - hdrH};
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
                bool sel = (r.n->id == selectedId);
                Rect rr{listArea.x, y, listArea.w, rowH};
                if (sel) emitRect(rr.x, rr.y, rr.w, rr.h, Col::selRow());
                float indent = (4 + r.depth * 8) * density;
                std::string prefix = r.n->children.empty() ? "  " : (r.n->expanded ? "v " : "> ");
                std::string lbl = prefix + r.n->name;
                uint32_t tc = sel ? Col::text() :
                    (r.n->isScript() ? Col::accent() :
                    (r.n->isCollisionNode() ? StudioFont::rgba(34, 211, 238) : Col::text()));
                emitText(lbl, rr.x + indent, rr.y + 3 * density, 8.0f * density, tc, rr.w - indent - 2);
                std::string nid = r.n->id;
                hitBoxes.push_back({rr, [this, nid]() {
                    if (auto *node = scene.findById(nid)) {
                        if (selectedId == nid && !node->children.empty()) node->expanded = !node->expanded;
                        selectedId = nid;
                        if (node->isScript() && !node->scriptPath.empty()) openScript(node->scriptPath);
                        if (node->is3DNode()) {
                            camTargetX = node->posX; camTargetY = node->posY; camTargetZ = node->posZ;
                        }
                    }
                }});
            }
            y += rowH;
        }
    }

    if (showProperties && rProperties.h > 1.0f) {
        pushBatch(true, rProperties);
        emitRect(rProperties.x, rProperties.y, rProperties.w, rProperties.h, Col::panel());
        emitRect(rProperties.x, rProperties.y, 1, rProperties.h, Col::border());
        emitRect(rProperties.x, rProperties.y, rProperties.w, hdrH, Col::header());
        emitText("Inspector", rProperties.x + 5 * density, rProperties.y + 2 * density, fs, Col::text());

        Rect pArea{rProperties.x + 1, rProperties.y + hdrH, rProperties.w - 1, rProperties.h - hdrH};
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
            props.push_back({"Mesh", n->shape(), [this]() { modal = ModalKind::SHAPE; }});
            props.push_back({"Color", "#", [this]() { modal = ModalKind::COLOR; }});
            props.push_back({"Material", n->material, [this]() { modal = ModalKind::MATERIAL; }});
            props.push_back({"Visible", n->visible ? "true" : "false", [this, n]() {
                n->visible = !n->visible; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
            props.push_back({"Transp", fmtNum(n->transparency), [this, n]() {
                promptTitle = "Transparency (0..1)"; promptValue = fmtNum(n->transparency);
                promptTargetKey = "Transparency"; modal = ModalKind::TEXT_PROMPT;
            }});
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
            props.push_back({"Path", n->scriptPath, [this, n]() { openScript(n->scriptPath); }});
            props.push_back({"Disabled", n->disabled ? "true" : "false", [this, n]() {
                n->disabled = !n->disabled; scene.saveToFile(currentProjectDir + "/scene.json");
            }});
        } else if (n->className == "Environment" || n->className == "Lighting") {
            props.push_back({"ClockTime", fmtNum(n->clockTime), [this, n]() {
                promptTitle = "ClockTime (0..24)"; promptValue = fmtNum(n->clockTime);
                promptTargetKey = "ClockTime"; modal = ModalKind::TEXT_PROMPT;
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
                } else {
                    emitText(p.v, splitX + 2, pr.y + 3 * density, 7.8f * density,
                             p.onTap ? Col::text() : Col::muted(), pr.x + pr.w - splitX - 4);
                }
                if (p.onTap) hitBoxes.push_back({pr, p.onTap});
            }
            y += rowH;
        }
    }
}

void StudioApp::buildBottomDock() {
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
    hitBoxes.push_back({rInput, [this]() {
        commandFocused = true;
        editorFocused = false;
        if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(true);
    }});
    Rect rRun{rCmdBar.x + rCmdBar.w - runW - 2 * density, rCmdBar.y + 2 * density, runW, rCmdBar.h - 4 * density};
    addBtn(rRun, "Run", Col::btnAct(), Col::text(), 8.2f * density, [this]() {
        std::string cmd = commandText;
        commandText.clear();
        execCommand(cmd);
    });
}

void StudioApp::buildCodeEditor(const Rect &area) {
    float barH = 24.0f * density;
    Rect codeRect{area.x, area.y, area.w, std::max(40.0f, area.h - barH)};
    Rect barRect{area.x, area.y + codeRect.h, area.w, barH};

    pushBatch(true, codeRect);
    uint32_t edBg = prefTheme == 1 ? StudioFont::rgba(250, 250, 252) : Col::bg();
    uint32_t edFg = prefTheme == 1 ? StudioFont::rgba(24, 24, 27) : Col::text();
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
                     prefTheme == 1 ? StudioFont::rgba(226, 232, 240) : StudioFont::rgba(36, 41, 51));
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
            if (k == 1) tc = StudioFont::rgba(198, 120, 221);
            else if (k == 2) tc = StudioFont::rgba(229, 192, 123);
            else if (k == 3) tc = StudioFont::rgba(97, 175, 239);
            else if (k == 4) tc = StudioFont::rgba(152, 195, 121);
            else if (k == 5) tc = StudioFont::rgba(209, 154, 102);
            else if (k == 6) tc = StudioFont::rgba(108, 118, 134);
            else if (k == 7) tc = StudioFont::rgba(86, 182, 194);
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

    pushBatch(true, barRect);
    emitRect(barRect.x, barRect.y, barRect.w, barRect.h, Col::header());
    const char *keys[] = {"TAB", "( )", "\" \"", "=", "Undo", "Check", "Save"};
    float kx = barRect.x + 3 * density;
    float kw = (barRect.w - 6 * density) / 7.0f;
    for (int i = 0; i < 7; ++i) {
        Rect kr{kx + i * kw, barRect.y + 2, kw - 2, barRect.h - 4};
        addBtn(kr, keys[i], Col::btn(), Col::text(), 7.8f * density, [this, i]() {
            if (i == 0) lc_editor_insert(std::string(prefTabSpaces, ' ').c_str());
            else if (i == 1) lc_editor_insert("()");
            else if (i == 2) lc_editor_insert("\"\"");
            else if (i == 3) lc_editor_insert(" = ");
            else if (i == 4) lc_editor_undo();
            else if (i == 5) {
                char *err = nullptr;
                int ok = luac_check(lc_editor_get_text(), activeScript.c_str(), &err);
                appendOutput(ok ? (activeScript + ": syntax OK") : (err ? err : "syntax error"), ok ? Col::green() : Col::red());
                if (err) luac_free(err);
            } else if (i == 6) {
                saveActiveScript();
                appendOutput("Saved " + activeScript, Col::green());
            }
        });
    }
}

void StudioApp::buildModal() {
    pushBatch(false, {});
    emitRect(0, 0, width, height, StudioFont::rgba(0, 0, 0, 165));

    float mw = std::min(width - 20 * density, 350.0f * density);
    float mh = std::min(height - 30 * density, 350.0f * density);
    if (modal == ModalKind::TEXT_PROMPT) mh = 132.0f * density;
    Rect box{(width - mw) * 0.5f, (height - mh) * 0.5f, mw, mh};
    emitRect(box.x, box.y, box.w, box.h, Col::panel());
    emitRectOutline(box.x, box.y, box.w, box.h, 1.5f, Col::border());
    hitBoxes.push_back({box, []() {}});

    float fs = 9.0f * density;
    float pad = 9.0f * density;
    float y = box.y + pad;

    auto closeBtn = [&](const std::string &title) {
        emitText(title, box.x + pad, y, 10.5f * density, Col::text());
        Rect rc{box.x + box.w - 26 * density, y - 2, 19 * density, 19 * density};
        addBtn(rc, "X", Col::btn(), Col::muted(), fs, [this]() {
            modal = ModalKind::NONE;
            if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(false);
        });
        y += 24.0f * density;
    };

    if (modal == ModalKind::INSERT) {
        closeBtn("Add Node (3D / Physics / Logic)");
        const char *cls[] = {
            "Box", "Wedge", "Sphere", "Cylinder",
            "Plane", "StaticBody3D", "RigidBody3D", "CollisionBox",
            "CollisionSphere", "Area3D", "Camera3D", "DirectionalLight3D",
            "PointLight3D", "Node3D", "Script", "Folder"
        };
        float cw = (mw - pad * 3) * 0.5f;
        for (int i = 0; i < 16; ++i) {
            float x = box.x + pad + (i % 2) * (cw + pad);
            float ry = y + (i / 2) * 26 * density;
            if (ry + 22 * density > box.y + box.h - 4) break;
            std::string cn = cls[i];
            addBtn({x, ry, cw, 22 * density}, cn, Col::btn(), Col::text(), 8.3f * density, [this, cn]() {
                SceneNode *parent = insertParentId.empty() ? nullptr : scene.findById(insertParentId);
                if (!parent) {
                    if (cn == "Script") parent = scene.findService("Scripts");
                    else parent = scene.findService("World3D");
                }
                if (auto *created = scene.createNode(parent, cn, cn + std::to_string(scene.nextId))) {
                    selectedId = created->id;
                    if (cn == "Script") {
                        created->scriptPath = "scripts/" + created->name + ".lua";
                        openScript(created->scriptPath);
                    }
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = ModalKind::NONE;
            });
        }
    } else if (modal == ModalKind::COLOR) {
        closeBtn("Object Color");
        static const uint32_t pal[12] = {
            0xFFA3A2A5, 0xFFDC4C46, 0xFF3B82F6, 0xFF22C55E,
            0xFFFACC15, 0xFFF97316, 0xFFA855F7, 0xFFEC4899,
            0xFF38BDF8, 0xFF8B5CF6, 0xFF1E293B, 0xFFF8FAFC
        };
        float cw = (mw - pad * 5) / 4.0f;
        for (int i = 0; i < 12; ++i) {
            float x = box.x + pad + (i % 4) * (cw + pad);
            float ry = y + (i / 4) * 38 * density;
            Rect cr{x, ry, cw, 32 * density};
            emitRect(cr.x, cr.y, cr.w, cr.h, Col::argbToUi(pal[i]));
            emitRectOutline(cr.x, cr.y, cr.w, cr.h, 1.0f, Col::border());
            uint32_t cval = pal[i];
            hitBoxes.push_back({cr, [this, cval]() {
                if (auto *n = selectedNode()) {
                    n->argbColor = cval;
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = ModalKind::NONE;
            }});
        }
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
        closeBtn("Mesh Shape");
        const char *sh[] = {"Box", "Wedge", "Sphere", "Cylinder", "Plane"};
        for (int i = 0; i < 5; ++i) {
            std::string sname = sh[i];
            addBtn({box.x + pad, y + i * 28 * density, mw - pad * 2, 23 * density}, sname, Col::btn(), Col::text(), fs, [this, sname]() {
                if (auto *n = selectedNode()) {
                    n->setShape(sname);
                    scene.saveToFile(currentProjectDir + "/scene.json");
                }
                modal = ModalKind::NONE;
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
    } else if (modal == ModalKind::TEXT_PROMPT) {
        closeBtn(promptTitle);
        Rect rIn{box.x + pad, y, mw - pad * 2, 25 * density};
        emitRect(rIn.x, rIn.y, rIn.w, rIn.h, Col::bg());
        emitRectOutline(rIn.x, rIn.y, rIn.w, rIn.h, 1.0f, Col::accent());
        emitText(promptValue + "_", rIn.x + 6 * density, rIn.y + 5 * density, fs, Col::text(), rIn.w - 12 * density);
        hitBoxes.push_back({rIn, [this]() {
            if (callbacks.setKeyboardVisible) callbacks.setKeyboardVisible(true);
        }});
        y += 32 * density;
        Rect rOk{box.x + pad, y, mw - pad * 2, 25 * density};
        addBtn(rOk, "Apply", Col::btnAct(), Col::text(), fs, [this]() {
            if (promptTargetKey == "New3D" && callbacks.createProject && !promptValue.empty()) {
                callbacks.createProject("3d", promptValue);
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
            } else if (auto *n = selectedNode()) {
                float a, b, c;
                if (promptTargetKey == "Rename" && !promptValue.empty()) n->name = promptValue;
                else if (promptTargetKey == "Position" && parseVec3(promptValue, a, b, c)) { n->posX = a; n->posY = b; n->posZ = c; }
                else if (promptTargetKey == "Size" && parseVec3(promptValue, a, b, c)) {
                    n->sizeX = std::max(0.2f, a); n->sizeY = std::max(0.2f, b); n->sizeZ = std::max(0.2f, c);
                } else if (promptTargetKey == "Rotation" && parseVec3(promptValue, a, b, c)) { n->rotX = a; n->rotY = b; n->rotZ = c; }
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
    if (!n || !n->is3DNode() || dragAxis < 0) return;
    float tipX = 0, tipY = 0, baseX = 0, baseY = 0;
    for (const auto &gt : gizmoTips) {
        if (gt.axis == dragAxis) { tipX = gt.sx; tipY = gt.sy; baseX = gt.bx; baseY = gt.by; break; }
    }
    float vx = tipX - baseX, vy = tipY - baseY;
    float vlen = std::hypot(vx, vy);
    float delta = (vlen > 4.0f) ? ((dx * vx + dy * vy) / vlen) * 0.05f : (dx - dy) * 0.04f;
    if (toolMode == ToolMode::MOVE) {
        if (dragAxis == 0) n->posX += delta;
        else if (dragAxis == 1) n->posY += delta;
        else n->posZ += delta;
    } else if (toolMode == ToolMode::SCALE) {
        if (dragAxis == 0) n->sizeX = std::max(0.4f, n->sizeX + delta);
        else if (dragAxis == 1) n->sizeY = std::max(0.4f, n->sizeY + delta);
        else n->sizeZ = std::max(0.4f, n->sizeZ + delta);
    } else if (toolMode == ToolMode::ROTATE) {
        float deg = delta * 18.0f;
        if (dragAxis == 0) n->rotX += deg;
        else if (dragAxis == 1) n->rotY += deg;
        else n->rotZ += deg;
    }
}

void StudioApp::onTouch(int action, int pointerIndex, int pointerCount, const int *ids, const float *xs, const float *ys) {
    (void)pointerIndex;
    if (pointerCount <= 0) return;
    float x0 = xs[0], y0 = ys[0];
    int baseAct = action & 0xFF;

    // Forward viewport touches to user Lua scripts during Play (2D or 3D)
    if (runningLua && g_luacProj && !showProjectsTab && modal == ModalKind::NONE && activeScript.empty() && rViewport.contains(x0, y0)) {
        int down = (baseAct == 0 || baseAct == 2 || baseAct == 5) ? 1 : 0;
        luac_project_touch(g_luacProj, ids[0], x0 - rViewport.x, y0 - rViewport.y, down);
        if (!currentIs3D && baseAct == 2) return;
    }

    if (baseAct == 0) {
        downX = lastX = x0; downY = lastY = y0;
        moved = false;
        touchMode = 0;
        dragAxis = -1;
        lastPinch = -1;

        if (modal == ModalKind::NONE) {
            if (rRibbon.contains(x0, y0)) touchMode = 5;
            else if (showProjectsTab && rViewport.contains(x0, y0)) touchMode = 10;
            else if (showExplorer && rExplorer.contains(x0, y0)) touchMode = 6;
            else if (showProperties && rProperties.contains(x0, y0)) touchMode = 7;
            else if (showOutput && rOutLog.contains(x0, y0)) touchMode = 9;
            else if (!activeScript.empty() && rViewport.contains(x0, y0)) touchMode = 8;
            else if (currentIs3D && activeScript.empty() && rViewport.contains(x0, y0)) {
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

    if (baseAct == 5 && pointerCount >= 2 && rViewport.contains(x0, y0) && currentIs3D && activeScript.empty() && !showProjectsTab) {
        touchMode = 2;
        lastPinch = std::hypot(xs[1] - xs[0], ys[1] - ys[0]);
        lastX = (xs[0] + xs[1]) * 0.5f;
        lastY = (ys[0] + ys[1]) * 0.5f;
        moved = true;
        return;
    }

    if (baseAct == 2) {
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
            if (lastPinch > 0) camDist = std::clamp(camDist * (lastPinch / std::max(20.0f, pinch)), 4.0f, 160.0f);
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
                    int tapLine = std::max(0, (int)((y0 - rViewport.y + editorScrollY) / lh));
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
    else if (!activeScript.empty() && !showProjectsTab) lc_editor_insert(utf8.c_str());
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
