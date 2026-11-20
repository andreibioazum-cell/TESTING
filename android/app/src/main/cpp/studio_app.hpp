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
        return px >= x && py >= y && px <= x + w && py <= y + h;
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
    std::function<void(const std::string &oldDir, const std::string &newName)> renameProject;
    std::function<void(const std::string &dir)> deleteProject;
    std::function<std::vector<std::pair<std::string, bool>>()> listProjects; // (name, is3D)
    std::function<void(int family, int size, int theme, int tabSpaces, bool lineNums, const std::string &customFont)> savePrefs;
};

class StudioApp {
public:
    enum class RibbonTab { HOME, MODEL, AVATAR, TEST, VIEW, PLUGINS };
    enum class ToolMode { SELECT, MOVE, SCALE, ROTATE };
    enum class ModalKind { NONE, PROJECTS, INSERT, COLOR, MATERIAL, SHAPE, SETTINGS, TEXT_PROMPT };

    float width = 1080.0f;
    float height = 2200.0f;
    float density = 3.0f;

    std::string projectsRoot;
    std::string currentProjectDir;
    std::string currentProjectName;
    bool currentIs3D = true;

    StudioScene scene;
    std::string selectedId;
    ToolMode toolMode = ToolMode::SELECT;
    RibbonTab activeRibbon = RibbonTab::HOME;

    bool showExplorer = true;
    bool showProperties = true;
    bool showOutput = true;
    bool showGrid = true;
    bool collisions = false;

    // Camera state
    float camTargetX = 0, camTargetY = 2.5f, camTargetZ = -3.0f;
    float camYaw = 0.36f, camPitch = 0.34f, camDist = 36.0f;

    // Play / Lua runtime state
    bool runningLua = false;
    bool playMode = false;
    std::string prePlaySceneJson;
    float playerX = 0, playerY = 3.5f, playerZ = 10.0f;
    float playerVy = 0, playerYaw = 0, walkCycle = 0;
    bool playerGrounded = false;
    float joyDx = 0, joyDy = 0;
    int joyPointerId = -1;
    std::string last2DFrameJson;

    // Script tabs & C++ code editor
    std::vector<std::string> openScripts; // relative paths
    std::string activeScript;             // empty => Viewport tab
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
    std::string promptTargetKey; // "Rename", "Position", "Size", "Rotation", "Transparency", "ClockTime", "Text", "New3D", "New2D", "RenameProj"
    std::string promptExtra;
    int modalScrollY = 0;

    // Scroll offsets
    float ribbonScrollX = 0;
    float explorerScrollY = 0;
    float propsScrollY = 0;
    float outputScrollY = 0;

    StudioFont font;
    StudioCallbacks callbacks;

    // Computed layout rects updated every frame
    Rect rTopTabs, rRibbon, rDocTabs, rViewport, rRightDock, rExplorer, rProperties, rBottomDock, rOutLog, rCmdBar;

    // 3D frame outputs for Vulkan 3D pass
    std::vector<MeshVertex> frame3DMesh;
    std::vector<MeshVertex> frame3DLines;
    std::vector<MeshVertex> frame3DOverlayLines; // gizmos drawn without depth test
    float viewProj[16]{};
    float skyTop[4]{0.12f, 0.43f, 0.77f, 1.0f};
    float skyBot[4]{0.55f, 0.77f, 0.95f, 1.0f};
    bool has3DViewportVisible = true;

    // 2D batched draw lists
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

private:
    struct HitBox {
        Rect r;
        std::function<void()> onTap;
    };
    std::vector<HitBox> hitBoxes;
    std::vector<std::string> explorerVisibleIds;

    // Touch tracking
    int touchMode = 0; // 0=none, 1=orbit, 2=pan_pinch, 3=gizmo, 4=joystick, 5=scroll_ribbon, 6=scroll_exp, 7=scroll_props, 8=scroll_ed
    int dragAxis = -1;
    float downX = 0, downY = 0, lastX = 0, lastY = 0, lastPinch = -1;
    bool moved = false;
    float joyCx = 0, joyCy = 0, joyR = 50, jumpCx = 0, jumpCy = 0, jumpR = 32;
    struct GizmoTip { int axis; float sx, sy, bx, by; };
    std::vector<GizmoTip> gizmoTips;

    // Cached camera vectors for raycast/pan
    Vec3f eye{}, fwd{}, right{}, up{};
    float focalLen = 600.0f;

    void computeLayout();
    void build3DScene();
    void build2DScene(const Rect &area);
    void buildTopRibbon();
    void buildDocTabs();
    void buildRightDock();
    void buildBottomDock();
    void buildCodeEditor(const Rect &area);
    void buildModal();

    void startSim(bool withAvatar);
    void stopSim();
    void execCommand(const std::string &cmd);
    void syncFromLua();
    void appendOutput(const std::string &txt, uint32_t col);
    void saveActiveScript();
    void openScript(const std::string &relPath);
    SceneNode *selectedNode();

    void stepPlayPhysics(float dt);
    float findGroundBelow(float px, float py, float pz);
    float surfaceHeightAt(const SceneNode &n, float px, float pz, float r);
    bool collidesHorizontal(float px, float py, float pz);
    bool projectPoint(Vec3f w, float &sx, float &sy) const;
    std::string raycastPart(float sx, float sy);
    void applyGizmoDrag(float dx, float dy);

    // 2D quad/text emission helpers
    void pushBatch(bool scissor, Rect sc);
    void emitRect(float x, float y, float w, float h, uint32_t col);
    void emitRectOutline(float x, float y, float w, float h, float t, uint32_t col);
    void emitCircle(float cx, float cy, float r, uint32_t col);
    void emitLine2D(float x0, float y0, float x1, float y1, float thick, uint32_t col);
    void emitText(const std::string &txt, float x, float y, float pxSize, uint32_t col, float maxW = 0.0f);
    void emitTextCentered(const std::string &txt, const Rect &r, float pxSize, uint32_t col);
    void addBtn(const Rect &r, const std::string &label, uint32_t bg, uint32_t fg, float pxSize, std::function<void()> cb);
};
