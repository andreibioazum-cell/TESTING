package com.luac.studio;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.textfield.TextInputEditText;
import java.io.File;
import java.util.ArrayList;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

final class StudioController {
    interface Host {
        void onShowProjects();
        void onOpenFiles(File project);
        void onOpenSettings();
        void onPickAssetForProject(File dir);
        String checkSyntax(String source);
        String openNativeProject(String root);
        String stepNativeProject(double dt);
        String execNativeCommand(String code);
        void closeNativeProject();
        void applyEditorPrefs(CodeEditorView editor);
        Typeface resolvedTypeface();
    }

    private enum RibbonTab { HOME, MODEL, AVATAR, TEST, VIEW, PLUGINS }

    private final Activity activity;
    private final ProjectStore store;
    private final Host host;
    private final File projectDir;
    private SceneModel scene;
    private SceneModel.Node selected;

    private RibbonTab activeRibbon = RibbonTab.HOME;
    private boolean showExplorer = true;
    private boolean showProperties = true;
    private boolean showOutput = true;
    private boolean showGrid = true;
    private boolean collisions = false;
    private float snapGrid = 0.5f;

    private Viewport3DView viewport;
    private LinearLayout scriptPane;
    private CodeEditorView codeEditor;
    private LinearLayout ribbonStrip;
    private LinearLayout docTabs;
    private LinearLayout rightDock, explorerPanel, propertiesPanel, outputPanel;
    private TextView propHeader, outputView;
    private ScrollView outputScroll;
    private EditText commandInput;
    private TextView tabHome, tabModel, tabAvatar, tabTest, tabView, tabPlugins;

    private ExplorerAdapter explorerAdapter;
    private PropertyAdapter propertyAdapter;

    private final ArrayList<File> openScripts = new ArrayList<>();
    private File activeScript = null;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean runningLua = false;
    private long lastTickNs = 0L;
    private JSONObject prePlaySnapshot = null;
    private final SpannableStringBuilder outputLog = new SpannableStringBuilder();

    private final Runnable simLoop = new Runnable() {
        @Override public void run() {
            if (!runningLua) return;
            long now = System.nanoTime();
            double dt = lastTickNs == 0L ? 1.0 / 60.0 : Math.min(0.05, (now - lastTickNs) / 1e9);
            lastTickNs = now;
            try {
                JSONObject frame = new JSONObject(host.stepNativeProject(dt));
                String out = frame.optString("output", "");
                if (!out.isEmpty()) log(out, 0xFFE5E7EB);
                if (!frame.optBoolean("ok", true)) {
                    log(frame.optString("error", "Runtime error") + "\n", 0xFFF87171);
                    stopSimulation();
                    return;
                }
                syncFromLua();
            } catch (Throwable err) {
                log("Simulation error: " + err + "\n", 0xFFF87171);
                stopSimulation();
                return;
            }
            viewport.stepPlayPhysics((float) dt);
            if (runningLua) handler.postDelayed(this, 16);
        }
    };

    StudioController(Activity activity, ProjectStore store, File projectDir, Host host) {
        this.activity = activity;
        this.store = store;
        this.projectDir = projectDir;
        this.host = host;
    }

    void bindViews() {
        scene = SceneModel.loadOrCreate(store, projectDir);
        selected = scene.findService("Workspace");

        TextView title = activity.findViewById(R.id.studio_title);
        title.setText(projectDir.getName());
        activity.findViewById(R.id.btn_projects).setOnClickListener(v -> {
            saveActiveScript();
            stopSimulation();
            host.onShowProjects();
        });

        tabHome = activity.findViewById(R.id.tab_home);
        tabModel = activity.findViewById(R.id.tab_model);
        tabAvatar = activity.findViewById(R.id.tab_avatar);
        tabTest = activity.findViewById(R.id.tab_test);
        tabView = activity.findViewById(R.id.tab_view);
        tabPlugins = activity.findViewById(R.id.tab_plugins);

        tabHome.setOnClickListener(v -> selectRibbon(RibbonTab.HOME));
        tabModel.setOnClickListener(v -> selectRibbon(RibbonTab.MODEL));
        tabAvatar.setOnClickListener(v -> selectRibbon(RibbonTab.AVATAR));
        tabTest.setOnClickListener(v -> selectRibbon(RibbonTab.TEST));
        tabView.setOnClickListener(v -> selectRibbon(RibbonTab.VIEW));
        tabPlugins.setOnClickListener(v -> selectRibbon(RibbonTab.PLUGINS));

        ribbonStrip = activity.findViewById(R.id.ribbon_strip);
        docTabs = activity.findViewById(R.id.doc_tabs);
        viewport = activity.findViewById(R.id.viewport3d);
        scriptPane = activity.findViewById(R.id.script_editor_pane);
        codeEditor = activity.findViewById(R.id.studio_code_editor);
        rightDock = activity.findViewById(R.id.right_dock);
        explorerPanel = activity.findViewById(R.id.explorer_panel);
        propertiesPanel = activity.findViewById(R.id.properties_panel);
        outputPanel = activity.findViewById(R.id.output_panel);
        propHeader = activity.findViewById(R.id.properties_header);
        outputView = activity.findViewById(R.id.studio_output);
        outputScroll = activity.findViewById(R.id.output_scroll);
        commandInput = activity.findViewById(R.id.command_input);

        host.applyEditorPrefs(codeEditor);
        viewport.setDefaultTypeface(host.resolvedTypeface());
        viewport.bind(scene, projectDir, new Viewport3DView.Listener() {
            @Override public void onNodeSelected(SceneModel.Node node) {
                setSelected(node != null ? node : scene.findService("Workspace"));
            }
            @Override public void onSceneChanged() {
                propertyAdapter.setNode(selected);
                saveSceneQuiet();
            }
        });

        RecyclerView expList = activity.findViewById(R.id.explorer_list);
        expList.setLayoutManager(new LinearLayoutManager(activity));
        explorerAdapter = new ExplorerAdapter(new ExplorerAdapter.Listener() {
            @Override public void onSelect(SceneModel.Node node) {
                setSelected(node);
            }
            @Override public void onOpenScript(SceneModel.Node node) {
                openScriptNode(node);
            }
            @Override public void onInsertChild(SceneModel.Node parent) {
                showInsertDialog(parent);
            }
        });
        expList.setAdapter(explorerAdapter);
        explorerAdapter.update(scene, selected);

        RecyclerView propList = activity.findViewById(R.id.properties_list);
        propList.setLayoutManager(new LinearLayoutManager(activity));
        propertyAdapter = new PropertyAdapter(this::editProperty);
        propList.setAdapter(propertyAdapter);
        propertyAdapter.setNode(selected);

        activity.findViewById(R.id.btn_explorer_insert).setOnClickListener(v ->
            showInsertDialog(selected != null ? selected : scene.findService("Workspace")));
        activity.findViewById(R.id.btn_clear_output).setOnClickListener(v -> {
            outputLog.clear();
            outputView.setText("");
        });

        activity.findViewById(R.id.skey_tab).setOnClickListener(v -> codeEditor.insertSnippet(codeEditor.getTabSnippet()));
        activity.findViewById(R.id.skey_colon).setOnClickListener(v -> codeEditor.insertSnippet(":"));
        activity.findViewById(R.id.skey_eq).setOnClickListener(v -> codeEditor.insertSnippet(" = "));
        activity.findViewById(R.id.skey_parens).setOnClickListener(v -> codeEditor.insertSnippet("()"));
        activity.findViewById(R.id.skey_undo).setOnClickListener(v -> codeEditor.undo());
        activity.findViewById(R.id.skey_check).setOnClickListener(v -> {
            saveActiveScript();
            String res = host.checkSyntax(codeEditor.getTextString());
            log(res + "\n", res.startsWith("Syntax OK") ? 0xFF4ADE80 : 0xFFF87171);
        });
        activity.findViewById(R.id.skey_save).setOnClickListener(v -> {
            if (saveActiveScript()) toast("Saved " + (activeScript != null ? activeScript.getName() : ""));
        });

        activity.findViewById(R.id.btn_run_cmd).setOnClickListener(v -> executeCommandBar());
        commandInput.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_SEND || actionId == EditorInfo.IME_ACTION_DONE
                || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER && event.getAction() == KeyEvent.ACTION_DOWN)) {
                executeCommandBar();
                return true;
            }
            return false;
        });

        log("LuaC Studio ready — " + projectDir.getName() + " loaded.\n", 0xFF93C5FD);
        selectRibbon(RibbonTab.HOME);
        rebuildDocTabs();
    }

    void refreshSettings() {
        if (codeEditor != null) host.applyEditorPrefs(codeEditor);
        if (viewport != null) viewport.setDefaultTypeface(host.resolvedTypeface());
    }

    void dispose() {
        saveActiveScript();
        stopSimulation();
    }

    private void setSelected(SceneModel.Node node) {
        selected = node;
        viewport.setSelected(node);
        explorerAdapter.setSelected(node);
        propertyAdapter.setNode(node);
        propHeader.setText(node != null ? "Properties - " + node.name : "Properties");
    }

    private void selectRibbon(RibbonTab tab) {
        activeRibbon = tab;
        TextView[] tabs = {tabHome, tabModel, tabAvatar, tabTest, tabView, tabPlugins};
        RibbonTab[] values = RibbonTab.values();
        for (int i = 0; i < tabs.length; ++i) {
            boolean active = values[i] == tab;
            tabs[i].setTextColor(active ? 0xFFFFFFFF : 0xFF9CA3AF);
            tabs[i].setBackgroundColor(active ? 0xFF2D313C : 0x00000000);
        }
        rebuildRibbon();
    }

    private void rebuildRibbon() {
        ribbonStrip.removeAllViews();
        if (activeRibbon == RibbonTab.HOME) {
            addToolBtn("↖", "Select", viewport.getToolMode() == Viewport3DView.ToolMode.SELECT, 0xFF3B82F6, () -> {
                viewport.setToolMode(Viewport3DView.ToolMode.SELECT);
                rebuildRibbon();
            });
            addToolBtn("✥", "Move", viewport.getToolMode() == Viewport3DView.ToolMode.MOVE, 0xFF3B82F6, () -> {
                viewport.setToolMode(Viewport3DView.ToolMode.MOVE);
                rebuildRibbon();
            });
            addToolBtn("⤢", "Scale", viewport.getToolMode() == Viewport3DView.ToolMode.SCALE, 0xFF3B82F6, () -> {
                viewport.setToolMode(Viewport3DView.ToolMode.SCALE);
                rebuildRibbon();
            });
            addToolBtn("↻", "Rotate", viewport.getToolMode() == Viewport3DView.ToolMode.ROTATE, 0xFF3B82F6, () -> {
                viewport.setToolMode(Viewport3DView.ToolMode.ROTATE);
                rebuildRibbon();
            });
            addSep();
            addToolBtn("⊞", "Collisions", collisions, 0xFF0EA5E9, () -> {
                collisions = !collisions;
                viewport.setCollisionsEnabled(collisions);
                rebuildRibbon();
            });
            addToolBtn("⚓", "Anchor", selected != null && selected.anchored, 0xFF6366F1, () -> {
                if (selected != null && selected.is3DPart()) {
                    selected.anchored = !selected.anchored;
                    propertyAdapter.setNode(selected);
                    saveSceneQuiet();
                    rebuildRibbon();
                }
            });
            addToolBtn("🔒", "Lock", selected != null && selected.locked, 0xFFF59E0B, () -> {
                if (selected != null && selected.is3DPart()) {
                    selected.locked = !selected.locked;
                    propertyAdapter.setNode(selected);
                    saveSceneQuiet();
                    rebuildRibbon();
                }
            });
            addSep();
            addToolBtn("■", "Part", false, 0xFF60A5FA, () -> showInsertPartMenu());
            addToolBtn("T", "UI", false, 0xFFA78BFA, () -> insertUiLabel());
            addToolBtn("📜", "Script", false, 0xFF4ADE80, () -> insertScript());
            addToolBtn("🎨", "Color", false, 0xFFF472B6, () -> {
                if (selected != null && (selected.is3DPart() || selected.isUi())) showColorPicker(selected);
                else toast("Select a Part or UI label first");
            });
            addToolBtn("◈", "Material", false, 0xFF38BDF8, () -> {
                if (selected != null && selected.is3DPart()) showMaterialPicker(selected);
                else toast("Select a Part first");
            });
            addSep();
            addToolBtn("▶", "Play", runningLua && viewport.isPlayMode(), 0xFF2563EB, () -> startSimulation(true));
            addToolBtn("▷", "Run", runningLua && !viewport.isPlayMode(), 0xFF16A34A, () -> startSimulation(false));
            addToolBtn("■", "Stop", false, 0xFFDC2626, this::stopSimulation);
            addSep();
            addToolBtn("⚙", "Fonts", false, 0xFF94A3B8, host::onOpenSettings);
        } else if (activeRibbon == RibbonTab.MODEL) {
            addToolBtn("↖", "Select", viewport.getToolMode() == Viewport3DView.ToolMode.SELECT, 0xFF3B82F6, () -> { viewport.setToolMode(Viewport3DView.ToolMode.SELECT); rebuildRibbon(); });
            addToolBtn("✥", "Move", viewport.getToolMode() == Viewport3DView.ToolMode.MOVE, 0xFF3B82F6, () -> { viewport.setToolMode(Viewport3DView.ToolMode.MOVE); rebuildRibbon(); });
            addToolBtn("⤢", "Scale", viewport.getToolMode() == Viewport3DView.ToolMode.SCALE, 0xFF3B82F6, () -> { viewport.setToolMode(Viewport3DView.ToolMode.SCALE); rebuildRibbon(); });
            addToolBtn("↻", "Rotate", viewport.getToolMode() == Viewport3DView.ToolMode.ROTATE, 0xFF3B82F6, () -> { viewport.setToolMode(Viewport3DView.ToolMode.ROTATE); rebuildRibbon(); });
            addSep();
            addToolBtn("❏", "Duplicate", false, 0xFF38BDF8, () -> {
                SceneModel.Node copy = scene.duplicate(selected);
                if (copy != null) { explorerAdapter.update(scene, copy); setSelected(copy); saveSceneQuiet(); }
            });
            addToolBtn("🗑", "Delete", false, 0xFFEF4444, () -> {
                if (scene.remove(selected)) {
                    setSelected(scene.findService("Workspace"));
                    explorerAdapter.update(scene, selected);
                    viewport.invalidate();
                    saveSceneQuiet();
                }
            });
            addSep();
            addToolBtn("■", "Block", false, 0xFF9CA3AF, () -> insertPart("Part", "Part"));
            addToolBtn("◢", "Wedge", false, 0xFF94A3B8, () -> insertPart("WedgePart", "Wedge"));
            addToolBtn("●", "Sphere", false, 0xFF4ADE80, () -> insertPart("Sphere", "Sphere"));
            addToolBtn("▮", "Cylinder", false, 0xFF38BDF8, () -> insertPart("Cylinder", "Cylinder"));
            addToolBtn("☀", "Spawn", false, 0xFFE5E7EB, () -> insertPart("SpawnLocation", "SpawnLocation"));
        } else if (activeRibbon == RibbonTab.AVATAR) {
            addToolBtn("▶", "Play Rig", runningLua && viewport.isPlayMode(), 0xFF2563EB, () -> startSimulation(true));
            addToolBtn("↺", "Respawn", false, 0xFF38BDF8, () -> { if (viewport.isPlayMode()) viewport.startPlayMode(); else startSimulation(true); });
            addToolBtn("■", "Stop", false, 0xFFDC2626, this::stopSimulation);
        } else if (activeRibbon == RibbonTab.TEST) {
            addToolBtn("▶", "Play", runningLua && viewport.isPlayMode(), 0xFF2563EB, () -> startSimulation(true));
            addToolBtn("▷", "Run", runningLua && !viewport.isPlayMode(), 0xFF16A34A, () -> startSimulation(false));
            addToolBtn("■", "Stop", false, 0xFFDC2626, this::stopSimulation);
            addSep();
            addToolBtn("📷", "Reset Cam", false, 0xFF94A3B8, () -> viewport.resetCamera());
        } else if (activeRibbon == RibbonTab.VIEW) {
            addToolBtn("🌲", "Explorer", showExplorer, 0xFF3B82F6, () -> { showExplorer = !showExplorer; updateDockVisibility(); rebuildRibbon(); });
            addToolBtn("☰", "Properties", showProperties, 0xFF3B82F6, () -> { showProperties = !showProperties; updateDockVisibility(); rebuildRibbon(); });
            addToolBtn("▤", "Output", showOutput, 0xFF3B82F6, () -> { showOutput = !showOutput; updateDockVisibility(); rebuildRibbon(); });
            addSep();
            addToolBtn("⊞", "Grid", showGrid, 0xFF0EA5E9, () -> { showGrid = !showGrid; viewport.setShowGrid(showGrid); rebuildRibbon(); });
            addToolBtn("📷", "Reset Cam", false, 0xFF94A3B8, () -> viewport.resetCamera());
            addToolBtn("⚙", "Settings", false, 0xFFFBBF24, host::onOpenSettings);
        } else { // PLUGINS
            addToolBtn("📁", "Files", false, 0xFF60A5FA, () -> { saveActiveScript(); stopSimulation(); host.onOpenFiles(projectDir); });
            addToolBtn("⬆", "Import", false, 0xFF34D399, () -> host.onPickAssetForProject(new File(projectDir, "assets")));
            addToolBtn("✓", "Check All", false, 0xFFFBBF24, () -> {
                try {
                    String res = host.checkSyntax(store.read(new File(projectDir, "main.lua")));
                    log("main.lua: " + res + "\n", res.startsWith("Syntax OK") ? 0xFF4ADE80 : 0xFFF87171);
                } catch (Exception e) { log(e + "\n", 0xFFF87171); }
            });
            addToolBtn("⚙", "Fonts", false, 0xFF94A3B8, host::onOpenSettings);
        }
    }

    private void addToolBtn(String icon, String label, boolean active, int accent, Runnable onClick) {
        float d = activity.getResources().getDisplayMetrics().density;
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(android.view.Gravity.CENTER);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, (int) (44 * d));
        lp.setMarginEnd((int) (4 * d));
        box.setLayoutParams(lp);
        box.setPadding((int) (9 * d), (int) (2 * d), (int) (9 * d), (int) (2 * d));
        box.setBackgroundColor(active ? 0xFF334155 : 0x00000000);

        TextView ic = new TextView(activity);
        ic.setText(icon);
        ic.setTextColor(active ? 0xFFFFFFFF : accent);
        ic.setTextSize(15f);
        ic.setGravity(android.view.Gravity.CENTER);

        TextView tx = new TextView(activity);
        tx.setText(label);
        tx.setTextColor(active ? 0xFFFFFFFF : 0xFFD1D5DB);
        tx.setTextSize(9.5f);
        tx.setGravity(android.view.Gravity.CENTER);

        box.addView(ic);
        box.addView(tx);
        box.setOnClickListener(v -> onClick.run());
        ribbonStrip.addView(box);
    }

    private void addSep() {
        float d = activity.getResources().getDisplayMetrics().density;
        View v = new View(activity);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams((int) (1 * d), (int) (32 * d));
        lp.setMargins((int) (4 * d), 0, (int) (6 * d), 0);
        v.setLayoutParams(lp);
        v.setBackgroundColor(0xFF3E424D);
        ribbonStrip.addView(v);
    }

    private void updateDockVisibility() {
        explorerPanel.setVisibility(showExplorer ? View.VISIBLE : View.GONE);
        propertiesPanel.setVisibility(showProperties ? View.VISIBLE : View.GONE);
        rightDock.setVisibility((showExplorer || showProperties) ? View.VISIBLE : View.GONE);
        outputPanel.setVisibility(showOutput ? View.VISIBLE : View.GONE);
    }

    private void insertPart(String cls, String baseName) {
        SceneModel.Node parent = (selected != null && ("Workspace".equals(selected.className) || "Folder".equals(selected.className)))
            ? selected : scene.findService("Workspace");
        SceneModel.Node n = scene.createPart(parent, cls, baseName);
        explorerAdapter.update(scene, n);
        setSelected(n);
        saveSceneQuiet();
    }

    private void showInsertPartMenu() {
        String[] shapes = {"Block (Part)", "Wedge (Ramp)", "Sphere", "Cylinder", "SpawnLocation"};
        String[] classes = {"Part", "WedgePart", "Sphere", "Cylinder", "SpawnLocation"};
        String[] names = {"Part", "Wedge", "Sphere", "Cylinder", "SpawnLocation"};
        new MaterialAlertDialogBuilder(activity)
            .setTitle("Insert 3D Part")
            .setItems(shapes, (d, which) -> insertPart(classes[which], names[which]))
            .show();
    }

    private void insertUiLabel() {
        SceneModel.Node sg = scene.findService("StarterGui");
        if (sg == null) return;
        SceneModel.Node gui = sg.children.isEmpty() ? null : sg.children.get(0);
        if (gui == null) {
            gui = new SceneModel.Node("gui_" + System.currentTimeMillis(), "ScreenGui", "ScreenGui");
            sg.add(gui);
        }
        SceneModel.Node lbl = new SceneModel.Node("lbl_" + System.currentTimeMillis(), "TextLabel", "TextLabel");
        lbl.text = "New TextLabel";
        lbl.fontName = "bold";
        lbl.textSize = 18;
        lbl.color = Color.WHITE;
        lbl.posX = 20f;
        lbl.posY = 50f + gui.children.size() * 26f;
        gui.add(lbl);
        sg.expanded = true; gui.expanded = true;
        explorerAdapter.update(scene, lbl);
        setSelected(lbl);
        saveSceneQuiet();
    }

    private void insertScript() {
        SceneModel.Node parent = (selected != null && ("Workspace".equals(selected.className) || "ServerScriptService".equals(selected.className) || "Folder".equals(selected.className)))
            ? selected : scene.findService("ServerScriptService");
        if (parent == null) parent = scene.findService("Workspace");
        String fileName = "scripts/script_" + (System.currentTimeMillis() % 10000) + ".lua";
        try {
            File f = new File(projectDir, fileName);
            store.write(f, "-- LuaC Script\nprint('Hello from " + f.getName() + "!')\n");
            SceneModel.Node sn = new SceneModel.Node("scr_" + System.currentTimeMillis(), f.getName().replace(".lua", ""), "Script");
            sn.scriptPath = fileName;
            parent.add(sn);
            parent.expanded = true;
            explorerAdapter.update(scene, sn);
            setSelected(sn);
            saveSceneQuiet();
            openScriptNode(sn);
        } catch (Exception e) { toast(e.getMessage()); }
    }

    private void showInsertDialog(SceneModel.Node parent) {
        String[] items = {"Part", "WedgePart", "Sphere", "Cylinder", "SpawnLocation", "Script", "TextLabel", "Folder"};
        new MaterialAlertDialogBuilder(activity)
            .setTitle("Insert Object into " + parent.name)
            .setItems(items, (d, which) -> {
                String cls = items[which];
                if ("Script".equals(cls)) { selected = parent; insertScript(); }
                else if ("TextLabel".equals(cls)) { insertUiLabel(); }
                else if ("Folder".equals(cls)) {
                    SceneModel.Node folder = new SceneModel.Node("fld_" + System.currentTimeMillis(), "Folder", "Folder");
                    parent.add(folder); parent.expanded = true;
                    explorerAdapter.update(scene, folder); setSelected(folder); saveSceneQuiet();
                } else {
                    SceneModel.Node n = scene.createPart(
                        ("Workspace".equals(parent.className) || "Folder".equals(parent.className)) ? parent : scene.findService("Workspace"),
                        cls, cls);
                    explorerAdapter.update(scene, n); setSelected(n); saveSceneQuiet();
                }
            }).show();
    }

    private void openScriptNode(SceneModel.Node node) {
        if (node == null || node.scriptPath == null || node.scriptPath.isEmpty()) return;
        File f = new File(projectDir, node.scriptPath);
        if (!f.isFile()) return;
        saveActiveScript();
        if (!openScripts.contains(f)) openScripts.add(f);
        activeScript = f;
        showCodeTab(f);
    }

    private void showViewportTab() {
        saveActiveScript();
        activeScript = null;
        scriptPane.setVisibility(View.GONE);
        viewport.setVisibility(View.VISIBLE);
        rebuildDocTabs();
    }

    private void showCodeTab(File f) {
        try {
            codeEditor.setText(store.read(f));
            host.applyEditorPrefs(codeEditor);
            viewport.setVisibility(View.GONE);
            scriptPane.setVisibility(View.VISIBLE);
            rebuildDocTabs();
        } catch (Exception e) { toast(e.getMessage()); }
    }

    private boolean saveActiveScript() {
        if (activeScript == null || scriptPane.getVisibility() != View.VISIBLE) return true;
        try {
            store.write(activeScript, codeEditor.getTextString());
            return true;
        } catch (Exception e) {
            toast(e.getMessage());
            return false;
        }
    }

    private void rebuildDocTabs() {
        docTabs.removeAllViews();
        addDocTab("🗺 Viewport", activeScript == null, this::showViewportTab, null);
        for (int i = 0; i < openScripts.size(); ++i) {
            final File f = openScripts.get(i);
            addDocTab("📄 " + f.getName(), f.equals(activeScript), () -> {
                saveActiveScript();
                activeScript = f;
                showCodeTab(f);
            }, () -> {
                if (f.equals(activeScript)) saveActiveScript();
                openScripts.remove(f);
                if (f.equals(activeScript)) showViewportTab();
                else rebuildDocTabs();
            });
        }
    }

    private void addDocTab(String title, boolean active, Runnable onClick, Runnable onClose) {
        float d = activity.getResources().getDisplayMetrics().density;
        LinearLayout tab = new LinearLayout(activity);
        tab.setOrientation(LinearLayout.HORIZONTAL);
        tab.setGravity(android.view.Gravity.CENTER_VERTICAL);
        tab.setBackgroundColor(active ? 0xFF2B2E36 : 0xFF1F2127);
        tab.setPadding((int) (10 * d), 0, (int) (8 * d), 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.MATCH_PARENT);
        lp.setMarginEnd((int) (2 * d));
        tab.setLayoutParams(lp);

        TextView tv = new TextView(activity);
        tv.setText(title);
        tv.setTextColor(active ? 0xFFFFFFFF : 0xFF9CA3AF);
        tv.setTextSize(11f);
        tab.addView(tv);

        if (onClose != null) {
            TextView x = new TextView(activity);
            x.setText(" ×");
            x.setTextColor(0xFF9CA3AF);
            x.setTextSize(13f);
            x.setPadding((int) (4 * d), 0, (int) (2 * d), 0);
            x.setOnClickListener(v -> onClose.run());
            tab.addView(x);
        }
        tab.setOnClickListener(v -> onClick.run());
        docTabs.addView(tab);
    }

    private void editProperty(SceneModel.Node node, String key) {
        if ("Name".equals(key)) {
            promptText("Rename " + node.name, node.name, val -> {
                if (!val.trim().isEmpty()) {
                    node.name = val.trim();
                    explorerAdapter.rebuild();
                    propertyAdapter.setNode(node);
                    saveSceneQuiet();
                }
            });
        } else if ("Color".equals(key)) {
            showColorPicker(node);
        } else if ("Material".equals(key)) {
            showMaterialPicker(node);
        } else if ("Shape".equals(key)) {
            new MaterialAlertDialogBuilder(activity).setTitle("Part Shape")
                .setItems(SceneModel.SHAPES, (d, i) -> {
                    node.setShape(SceneModel.SHAPES[i]);
                    viewport.invalidate(); propertyAdapter.setNode(node); explorerAdapter.rebuild(); saveSceneQuiet();
                }).show();
        } else if ("Transparency".equals(key)) {
            promptText("Transparency (0.0 - 1.0)", String.format(Locale.US, "%.2f", node.transparency), val -> {
                try {
                    node.transparency = Math.max(0f, Math.min(1f, Float.parseFloat(val.trim())));
                    viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
                } catch (Exception ignored) { }
            });
        } else if ("Position".equals(key) || "Size".equals(key) || "Rotation".equals(key)) {
            String cur = "Position".equals(key) ? fmt3(node.posX, node.posY, node.posZ)
                : ("Size".equals(key) ? fmt3(node.sizeX, node.sizeY, node.sizeZ) : fmt3(node.rotX, node.rotY, node.rotZ));
            promptText(key + " (X, Y, Z)", cur, val -> {
                String[] parts = val.split(",");
                if (parts.length >= 2) {
                    try {
                        float x = Float.parseFloat(parts[0].trim());
                        float y = Float.parseFloat(parts[1].trim());
                        float z = parts.length >= 3 ? Float.parseFloat(parts[2].trim()) : 0f;
                        if ("Position".equals(key)) { node.posX = x; node.posY = y; node.posZ = z; }
                        else if ("Size".equals(key)) { node.sizeX = Math.max(0.2f, x); node.sizeY = Math.max(0.2f, y); node.sizeZ = Math.max(0.2f, z); }
                        else { node.rotX = x; node.rotY = y; node.rotZ = z; }
                        viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
                    } catch (Exception ignored) { }
                }
            });
        } else if ("Anchored".equals(key)) {
            node.anchored = !node.anchored; propertyAdapter.setNode(node); saveSceneQuiet();
        } else if ("CanCollide".equals(key)) {
            node.canCollide = !node.canCollide; propertyAdapter.setNode(node); saveSceneQuiet();
        } else if ("CastShadow".equals(key)) {
            node.castShadow = !node.castShadow; propertyAdapter.setNode(node); saveSceneQuiet();
        } else if ("Locked".equals(key)) {
            node.locked = !node.locked; propertyAdapter.setNode(node); saveSceneQuiet();
        } else if ("Disabled".equals(key)) {
            node.disabled = !node.disabled; propertyAdapter.setNode(node); saveSceneQuiet();
        } else if ("ClockTime".equals(key)) {
            promptText("Lighting ClockTime (0 - 24)", String.valueOf(node.clockTime), val -> {
                try {
                    node.clockTime = Math.max(0f, Math.min(24f, Float.parseFloat(val.trim())));
                    viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
                } catch (Exception ignored) { }
            });
        } else if ("Text".equals(key)) {
            promptText("TextLabel Text", node.text, val -> {
                node.text = val; viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
            });
        } else if ("Font".equals(key)) {
            new MaterialAlertDialogBuilder(activity).setTitle("UI Font")
                .setItems(SceneModel.FONTS, (d, i) -> {
                    node.fontName = SceneModel.FONTS[i];
                    viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
                }).show();
        } else if ("TextSize".equals(key)) {
            promptText("TextSize", String.valueOf(node.textSize), val -> {
                try {
                    node.textSize = Math.max(8, Math.min(72, Integer.parseInt(val.trim())));
                    viewport.invalidate(); propertyAdapter.setNode(node); saveSceneQuiet();
                } catch (Exception ignored) { }
            });
        } else if ("Source".equals(key) && node.isScript()) {
            openScriptNode(node);
        }
    }

    private void showColorPicker(SceneModel.Node node) {
        int[] palette = {
            0xFFFFFFFF, 0xFFE5E7EB, 0xFF9CA3AF, 0xFF4B5563, 0xFF1F2937,
            0xFFEF4444, 0xFFB8635A, 0xFFF97316, 0xFFFACC15, 0xFF4ADE80,
            0xFF10B981, 0xFF06B6D4, 0xFF3B82F6, 0xFF6366F1, 0xFFA855F7,
            0xFFEC4899, 0xFF92400E, 0xFF78350F, 0xFF1E3A8A, 0xFF064E3B
        };
        float d = activity.getResources().getDisplayMetrics().density;
        GridLayout grid = new GridLayout(activity);
        grid.setColumnCount(5);
        int pad = (int) (16 * d);
        grid.setPadding(pad, pad, pad, pad);
        androidx.appcompat.app.AlertDialog dialog = new MaterialAlertDialogBuilder(activity)
            .setTitle("Color — " + node.name)
            .setView(grid)
            .setNegativeButton("Cancel", null)
            .create();
        for (int col : palette) {
            View sw = new View(activity);
            GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
            lp.width = (int) (42 * d);
            lp.height = (int) (42 * d);
            int m = (int) (6 * d);
            lp.setMargins(m, m, m, m);
            sw.setLayoutParams(lp);
            sw.setBackgroundColor(col);
            sw.setOnClickListener(v -> {
                node.color = col;
                viewport.invalidate();
                propertyAdapter.setNode(node);
                saveSceneQuiet();
                dialog.dismiss();
            });
            grid.addView(sw);
        }
        dialog.show();
    }

    private void showMaterialPicker(SceneModel.Node node) {
        new MaterialAlertDialogBuilder(activity)
            .setTitle("Material — " + node.name)
            .setItems(SceneModel.MATERIALS, (d, i) -> {
                node.material = SceneModel.MATERIALS[i];
                viewport.invalidate();
                propertyAdapter.setNode(node);
                saveSceneQuiet();
            }).show();
    }

    private void startSimulation(boolean withAvatar) {
        saveActiveScript();
        saveSceneQuiet();
        if (runningLua) stopSimulation();
        try {
            prePlaySnapshot = scene.toJson();
        } catch (Exception ignored) { }

        showViewportTab();
        String opened = host.openNativeProject(projectDir.getAbsolutePath());
        try {
            JSONObject initial = new JSONObject(opened);
            String out = initial.optString("output", "");
            if (!out.isEmpty()) log(out, 0xFFE5E7EB);
            if (!initial.optBoolean("ok", true)) {
                log(initial.optString("error", "Cannot start") + "\n", 0xFFF87171);
                return;
            }
        } catch (Exception e) {
            log("Error starting scripts: " + opened + "\n", 0xFFF87171);
            return;
        }
        runningLua = true;
        lastTickNs = 0L;
        if (withAvatar) viewport.startPlayMode();
        rebuildRibbon();
        handler.post(simLoop);
    }

    private void stopSimulation() {
        handler.removeCallbacks(simLoop);
        if (runningLua) {
            runningLua = false;
            host.closeNativeProject();
        }
        viewport.stopPlayMode();
        if (prePlaySnapshot != null) {
            scene = SceneModel.fromJson(prePlaySnapshot);
            prePlaySnapshot = null;
            selected = scene.findService("Workspace");
            viewport.bind(scene, projectDir, new Viewport3DView.Listener() {
                @Override public void onNodeSelected(SceneModel.Node node) {
                    setSelected(node != null ? node : scene.findService("Workspace"));
                }
                @Override public void onSceneChanged() {
                    propertyAdapter.setNode(selected);
                    saveSceneQuiet();
                }
            });
            explorerAdapter.update(scene, selected);
            propertyAdapter.setNode(selected);
        }
        rebuildRibbon();
    }

    private void executeCommandBar() {
        String cmd = commandInput.getText().toString().trim();
        if (cmd.isEmpty()) return;
        commandInput.setText("");
        log("> " + cmd + "\n", 0xFF60A5FA);
        if (!runningLua) {
            saveSceneQuiet();
            String opened = host.openNativeProject(projectDir.getAbsolutePath());
            try {
                JSONObject j = new JSONObject(opened);
                if (!j.optBoolean("ok", true)) {
                    log(j.optString("error", "Init failed") + "\n", 0xFFF87171);
                    return;
                }
                runningLua = true;
            } catch (Exception e) {
                log(opened + "\n", 0xFFF87171);
                return;
            }
        }
        try {
            JSONObject res = new JSONObject(host.execNativeCommand(cmd));
            String out = res.optString("output", "");
            if (!out.isEmpty()) log(out, 0xFFE5E7EB);
            String ret = res.isNull("result") ? "" : res.optString("result", "");
            if (!ret.isEmpty()) log(ret + "\n", 0xFF86EFAC);
            if (!res.optBoolean("ok", true)) {
                log(res.optString("error", "Error") + "\n", 0xFFF87171);
            } else {
                syncFromLua();
            }
        } catch (Exception e) {
            log("Command error: " + e + "\n", 0xFFF87171);
        }
    }

    private void syncFromLua() {
        try {
            JSONObject res = new JSONObject(host.execNativeCommand("return sdk.sync()"));
            String arrStr = res.optString("result", "");
            if (arrStr.isEmpty() || "null".equals(arrStr)) return;
            JSONArray arr = new JSONArray(arrStr);
            SceneModel.Node ws = scene.findService("Workspace");
            if (ws == null) return;
            boolean addedNew = false;
            for (int i = 0; i < arr.length(); ++i) {
                JSONObject p = arr.optJSONObject(i);
                if (p == null) continue;
                String id = p.optString("id", "");
                SceneModel.Node target = !id.isEmpty() ? scene.findById(id) : null;
                if (target == null) {
                    // Created dynamically via Instance.new(...)
                    String cls = p.optString("className", "Part");
                    String name = p.optString("name", cls);
                    target = scene.createPart(ws, cls, name);
                    if (!id.isEmpty()) target.id = id;
                    addedNew = true;
                } else {
                    target.name = p.optString("name", target.name);
                }
                target.posX = (float) p.optDouble("x", target.posX);
                target.posY = (float) p.optDouble("y", target.posY);
                target.posZ = (float) p.optDouble("z", target.posZ);
                target.sizeX = (float) p.optDouble("sx", target.sizeX);
                target.sizeY = (float) p.optDouble("sy", target.sizeY);
                target.sizeZ = (float) p.optDouble("sz", target.sizeZ);
                target.rotX = (float) p.optDouble("rx", target.rotX);
                target.rotY = (float) p.optDouble("ry", target.rotY);
                target.rotZ = (float) p.optDouble("rz", target.rotZ);
                int r = Math.max(0, Math.min(255, (int) Math.round(p.optDouble("r", 0.6) * 255)));
                int g = Math.max(0, Math.min(255, (int) Math.round(p.optDouble("g", 0.6) * 255)));
                int b = Math.max(0, Math.min(255, (int) Math.round(p.optDouble("b", 0.6) * 255)));
                target.color = Color.rgb(r, g, b);
                target.transparency = (float) p.optDouble("transparency", target.transparency);
                target.material = p.optString("material", target.material);
            }
            if (addedNew) explorerAdapter.update(scene, selected);
            viewport.invalidate();
        } catch (Exception ignored) { }
    }

    private void log(String text, int color) {
        if (text == null || text.isEmpty()) return;
        int start = outputLog.length();
        outputLog.append(text);
        outputLog.setSpan(new ForegroundColorSpan(color), start, outputLog.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        if (outputLog.length() > 24000) outputLog.delete(0, outputLog.length() - 16000);
        outputView.setText(outputLog);
        outputScroll.post(() -> outputScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void saveSceneQuiet() {
        try { scene.save(store, projectDir); } catch (Exception ignored) { }
    }

    private void promptText(String title, String initial, ValueAction action) {
        View view = LayoutInflater.from(activity).inflate(R.layout.dialog_name, null);
        TextInputEditText input = view.findViewById(R.id.input);
        input.setText(initial);
        input.setSelection(initial.length());
        new MaterialAlertDialogBuilder(activity).setTitle(title).setView(view)
            .setPositiveButton("OK", (d, w) -> action.run(input.getText().toString()))
            .setNegativeButton("Cancel", null).show();
    }

    private void toast(String msg) {
        Toast.makeText(activity, msg, Toast.LENGTH_SHORT).show();
    }

    private static String fmt3(float a, float b, float c) {
        return String.format(Locale.US, "%.1f, %.1f, %.1f", a, b, c);
    }

    private interface ValueAction { void run(String value); }
}
