#pragma once
#include "studio_font.hpp"
#include "studio_scene.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px >= 0 && px <= x + w && py >= y && py <= y + h;
    }
};

struct DrawBatch {
    std::vector<UiVertex> verts;
    Rect scissor;
    bool hasScissor = false;
};

struct StudioCallbacks {
    std::function<void(bool)> setKeyboardVisible;
    std::function<void()> openFontPicker;
    std::function<void(const std::string &kind, const std::string &name)> createProject;
    std::function<void(const std::string &oldName, const std::string &newName)> renameProject;
    std::function<void(const std::string &name)> deleteProject;
    std::function<std::vector<std::pair<std::string, bool>>()> listProjects;
    std::function<void(int family, int size, int theme, int tabSpaces, bool lineNums, const std::string &customFont)> savePrefs;
};

class StudioApp {
public:
    enum class RibbonTab { PROJECTS, HOME, CREATE, VIEW, SCRIPTS };
    enum class ToolMode { SELECT, MOVE, SCALE, ROTATE };
    enum class ModalKind { NONE, INSERT, COLOR, MATERIAL, SHAPE, SETTINGS, TEXT_PROMPT, NEW_PROJECT, REPARENT, SKY };

    float width = 1080.0f;
    float height = 2200.0f;
    float density = 3.0f;

    std::string projectsRoot;
    std::string currentProjectDir;
    std::string currentProjectName;
    bool currentIs3D = true;

    StudioScene scene;
    std::string selectedId;
    bool multiSelectMode = false;
    std::vector<std::string> multiSelectedIds;
    ToolMode toolMode = ToolMode::SELECT;
    RibbonTab activeRibbon = RibbonTab::HOME;

    bool showProjectsTab = true;
    bool showExplorer = true;
    bool showProperties = true;
    bool showOutput = true;
    bool showGrid = true;
    bool showColliders = true;
    bool showShadows = true;
    bool useCamera3DInPlay = true;

    // Editor / 3D camera state
    float camTargetX = 0, camTargetY = 1.5f, camTargetZ = -3.0f;
    float camYaw = 0.36f, camPitch = 0.34f, camDist = 28.0f;

    // Lua runtime state (zero hardcoded avatar/joystick/jump — pure 3D + user Lua scripts)
    bool runningLua = false;
    std::string prePlaySceneJson;
    std::string last2DFrameJson;

    // Script tabs & C++ code editor
    std::vector<std::string> openScripts;
    std::string activeScript;
    float editorScrollY = 0;
    bool editorFocused = false;

    // Output & Command bar
    struct OutLine { std::string text; uint32_t color; };
    std::vector<OutLine> outputLines;
    std::string commandText;
    bool commandFocused = false;

    // Settings
    int prefFontFamily = 0;
    int prefFontSize = 14;
    int prefTheme = 0;
    int prefTabSpaces = 4;
    bool prefLineNumbers = true;
    std::string prefCustomFontPath;

    // Modals & prompts
    ModalKind modal = ModalKind::NONE;
    std::string insertParentId;
    std::string promptTitle;
    std::string promptValue;
    std::string promptTargetKey;
    std::string promptExtra;

    // Scroll offsets
    float ribbonScrollX = 0;
    float explorerScrollY = 0;
    float propsScrollY = 0;
    float outputScrollY = 0;
    float projectsScrollY = 0;

    StudioFont font;
    StudioCallbacks callbacks;

    Rect rTopTabs, rRibbon, rDocTabs, rViewport, rRightDock, rExplorer, rProperties, rBottomDock, rOutLog, rCmdBar;

    std::vector<MeshVertex> frame3DMesh;
    std::vector<MeshVertex> frame3DShadowCasters;
    std::vector<MeshVertex> frame3DLines;
    std::vector<MeshVertex> frame3DOverlayLines;
    float viewProj[16]{};
    float lightVP[16]{};
    float skyTop[4]{0.12f, 0.43f, 0.77f, 1.0f};
    float skyBot[4]{0.55f, 0.77f, 0.95f, 1.0f};
    float skyTint[4]{1.0f, 1.0f, 1.0f, 0.42f};
    float sunDir[4]{0.55f, 0.85f, 0.40f, 0.0f};
    bool has3DViewportVisible = true;

    std::vector<DrawBatch> uiBatches;

    void init(const std::string &rootDir, const std::string &initialProjName, bool is3D,
              int fontFamily, int fontSize, int theme, int tabSpaces, bool lineNums, const std::string &customFont);
    void openProject(const std::string &projName, bool is3D);
    void applyFont(const std::string &customPath, int family);
    void step(float dt);
    void buildFrame(float w, float h, float d);
    void onTouch(int action, int pointerIndex, int pointerCount, const int *ids, const float *xs, const float *ys);
    void onTextInput(const std::string &utf8);
    void onKeyCode(int keyCode);
    void dispose();
    Rect closePlayBtnRect() const;

private:
    struct HitBox {
        Rect r;
        std::function<void()> onTap;
    };
    std::vector<HitBox> hitBoxes;
    Rect currentClipRect{0, 0, 0, 0};
    bool hasClipRect = false;

    int touchMode = 0; // 0=none, 1=orbit, 2=pan_pinch, 3=gizmo, 5=scroll_ribbon, 6=scroll_exp, 7=scroll_props, 8=scroll_ed, 9=scroll_out, 10=scroll_proj, 11=close_play
    int dragAxis = -1;
    int closePointerId = -1;
    float downX = 0, downY = 0, lastX = 0, lastY = 0, lastPinch = -1;
    bool moved = false;
    struct GizmoTip { int axis; float sx, sy, bx, by; };
    std::vector<GizmoTip> gizmoTips;

    Vec3f eye{}, fwd{}, right{}, up{};
    float focalLen = 600.0f;

    void computeLayout();
    void build3DScene();
    void build2DScene(const Rect &area);
    void renderLuaDrawOverlay(const Rect &area, bool applyClearColor);
    void buildProjectsTab(const Rect &area);
    void buildTopRibbon();
    void buildDocTabs();
    void buildRightDock();
    void buildBottomDock();
    void buildCodeEditor(const Rect &area);
    void buildModal();

    void startSim();
    void stopSim();
    void execCommand(const std::string &cmd);
    void syncFromLua();
    void appendOutput(const std::string &txt, uint32_t col);
    void saveActiveScript();
    void openScript(const std::string &relPath);
    SceneNode *selectedNode();

    bool projectPoint(Vec3f w, float &sx, float &sy) const;
    std::string raycastPart(float sx, float sy);
    void applyGizmoDrag(float dx, float dy);

    void pushBatch(bool scissor, Rect sc);
    void addHitBox(Rect r, std::function<void()> cb);
    void emitRect(float x, float y, float w, float h, uint32_t col);
    void emitRectOutline(float x, float y, float w, float h, float t, uint32_t col);
    void emitRoundRect(float x, float y, float w, float h, float rad, uint32_t col);
    void emitLine2D(float x0, float y0, float x1, float y1, float thick, uint32_t col);
    void emitCircle(float cx, float cy, float r, uint32_t col);
    void emitCircleOutline(float cx, float cy, float r, float thick, uint32_t col);
    void emitText(const std::string &txt, float x, float y, float pxSize, uint32_t col, float maxW = 0.0f);
    void emitTextCentered(const std::string &txt, const Rect &r, float pxSize, uint32_t col);
    void addBtn(const Rect &r, const std::string &label, uint32_t bg, uint32_t fg, float pxSize, std::function<void()> cb);
};
