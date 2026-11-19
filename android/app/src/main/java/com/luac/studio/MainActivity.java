package com.luac.studio;

import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton;
import com.google.android.material.switchmaterial.SwitchMaterial;
import com.google.android.material.textfield.TextInputEditText;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import org.json.JSONObject;

public final class MainActivity extends AppCompatActivity {
    private static final int PICK_ASSET = 41;
    private static final int PICK_FONT = 42;
    private static final String PREFS = "luac_studio_prefs";

    private static boolean nativeAvailable;
    private static String nativeLoadError = "";
    static {
        try { System.loadLibrary("luac_native"); nativeAvailable = true; }
        catch (Throwable error) { nativeLoadError = error.getClass().getSimpleName(); }
    }

    private static native String nativeCheck(String source);
    private static native String nativeGenerate(String source);
    private static native String nativeVersion();
    private static native String nativeOpenProject(String root);
    private static native String nativeStepProject(double dt);
    private static native String nativeExecCommand(String code);
    private static native void nativeSetViewport(int width, int height);
    private static native void nativeTouchProject(int id, double x, double y, boolean down);
    private static native void nativeCloseProject();
    private static native boolean nativeVulkanCreate(android.view.Surface surface);
    private static native void nativeVulkanDraw();
    private static native void nativeVulkanDestroy();

    private final Handler handler = new Handler(Looper.getMainLooper());
    private ProjectStore store;
    private File project, directory, editorFile, imageFile, importDirectory;
    private CodeEditorView editor;
    private TextView output;
    private RuntimeView runtimeView;
    private StudioController studio;
    private boolean running, opening, runtimeError, destroyed;
    private long previousFrame;
    private final StringBuilder console = new StringBuilder();

    private final Runnable frame = new Runnable() {
        @Override public void run() {
            if (!running || !nativeAvailable) return;
            long now = System.nanoTime();
            double dt = previousFrame == 0 ? 1.0 / 60 : Math.min(.05, (now - previousFrame) / 1e9);
            previousFrame = now;
            try { applyFrame(new JSONObject(nativeStepProject(dt))); }
            catch (Throwable error) { appendConsole("Runtime bridge error: " + error + "\n"); running = false; }
            if (running) handler.postDelayed(this, 16);
        }
    };

    @Override public void onCreate(Bundle state) {
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES);
        super.onCreate(state);
        store = new ProjectStore(this);
        File launch3D = null;
        try {
            if (store.projects().isEmpty()) {
                launch3D = store.createFpsArenaProject("FPS Arena");
                store.createSnakeProject("Змейка");
            } else {
                store.upgradeLegacySnakeIfUntouched();
                for (File p : store.projects()) {
                    if (store.is3DProject(p)) { launch3D = p; break; }
                }
                if (launch3D == null) launch3D = store.createFpsArenaProject("FPS Arena");
            }
        } catch (Exception error) { toast(error); }
        if (launch3D != null) showStudio(launch3D);
        else showProjects();
    }

    private void toast(Throwable error) { Toast.makeText(this, String.valueOf(error.getMessage() == null ? error : error.getMessage()), Toast.LENGTH_LONG).show(); }
    private void toast(String message) { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); }

    private MaterialToolbar toolbar(int layout) {
        setContentView(layout);
        MaterialToolbar bar = findViewById(R.id.toolbar);
        bar.setTitleTextColor(Color.WHITE);
        bar.setSubtitleTextColor(Color.WHITE);
        return bar;
    }
    private void back(MaterialToolbar bar, Runnable action) {
        bar.setNavigationIcon(R.drawable.ic_back);
        bar.setNavigationOnClickListener(v -> action.run());
    }
    private String relative(File file) {
        return project.toURI().relativize(file.toURI()).getPath();
    }

    private void showProjects() {
        stopRuntime();
        if (studio != null) { studio.dispose(); studio = null; }
        project = directory = editorFile = imageFile = null;
        MaterialToolbar bar = toolbar(R.layout.screen_projects);
        String version = "native unavailable";
        if (nativeAvailable) try { version = nativeVersion(); } catch (Throwable ignored) { }
        bar.setTitle("LuaC Studio");
        bar.setSubtitle(version + " • 3D & 2D IDE");
        bar.inflateMenu(R.menu.menu_projects);
        bar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_settings) showSettingsDialog();
            return true;
        });
        RecyclerView list = findViewById(R.id.list);
        list.setLayoutManager(new LinearLayoutManager(this));
        ProjectAdapter adapter = new ProjectAdapter(store, store.projects(), new ProjectAdapter.Listener() {
            @Override public void onOpen(File item) { showProject(item); }
            @Override public void onRun(File item) {
                if (store.is3DProject(item)) showStudio(item);
                else startProject(item);
            }
            @Override public void onRename(File item) {
                renameDialog(item, renamed -> { showProjects(); toast("Renamed to " + renamed.getName()); });
            }
            @Override public void onDelete(File item) { confirmDelete(item, MainActivity.this::showProjects); }
        });
        list.setAdapter(adapter);
        ExtendedFloatingActionButton fab = findViewById(R.id.fab);
        fab.setOnClickListener(v -> newProjectDialog());
    }

    private void newProjectDialog() {
        String[] kinds = {"3D Studio World (Roblox-style)", "2D Canvas Project"};
        new MaterialAlertDialogBuilder(this)
            .setTitle("New Project Type")
            .setItems(kinds, (d, which) -> {
                if (which == 0) {
                    nameDialog("New 3D World", "", "My Arena", "Create", name -> {
                        try { showStudio(store.createFpsArenaProject(name)); }
                        catch (Exception error) { toast(error); }
                    });
                } else {
                    nameDialog("New 2D Project", "", "Project name", "Create", name -> {
                        try { showProject(store.createProject(name)); }
                        catch (Exception error) { toast(error); }
                    });
                }
            })
            .show();
    }

    private void showProject(File selected) {
        if (store.is3DProject(selected)) {
            showStudio(selected);
        } else {
            project = selected;
            showDirectory(selected);
        }
    }

    private void showStudio(File selected) {
        stopRuntime();
        if (studio != null) { studio.dispose(); studio = null; }
        project = selected;
        directory = selected;
        editorFile = imageFile = null;
        editor = null;
        runtimeView = null;
        setContentView(R.layout.screen_studio);
        studio = new StudioController(this, store, selected, new StudioController.Host() {
            @Override public void onShowProjects() { showProjects(); }
            @Override public void onOpenFiles(File p) {
                studio = null;
                project = p;
                showDirectory(p);
            }
            @Override public void onOpenSettings() { showSettingsDialog(); }
            @Override public void onPickAssetForProject(File dir) {
                importDirectory = dir;
                pickAsset();
            }
            @Override public String checkSyntax(String source) {
                if (!nativeAvailable) return "Native compiler unavailable";
                return nativeCheck(source);
            }
            @Override public String openNativeProject(String root) {
                if (!nativeAvailable) return "{\"ok\":false,\"error\":\"Native compiler unavailable\"}";
                return nativeOpenProject(root);
            }
            @Override public String stepNativeProject(double dt) {
                if (!nativeAvailable) return "{\"ok\":false,\"error\":\"Native compiler unavailable\"}";
                return nativeStepProject(dt);
            }
            @Override public String execNativeCommand(String code) {
                if (!nativeAvailable) return "{\"ok\":false,\"error\":\"Native compiler unavailable\"}";
                return nativeExecCommand(code);
            }
            @Override public void closeNativeProject() {
                if (nativeAvailable) try { nativeCloseProject(); } catch (Throwable ignored) { }
            }
            @Override public void applyEditorPrefs(CodeEditorView ed) {
                applyEditorSettings(ed);
            }
            @Override public Typeface resolvedTypeface() {
                return resolvedRuntimeTypeface();
            }
        });
        studio.bindViews();
    }

    private void showDirectory(File selected) {
        stopRuntime();
        directory = selected;
        editorFile = imageFile = null;
        MaterialToolbar bar = toolbar(R.layout.screen_files);
        bar.setTitle(project.getName());
        String path = relative(selected);
        bar.setSubtitle(path.isEmpty() ? "Project files" : path);
        back(bar, this::onBackPressed);
        bar.inflateMenu(R.menu.menu_files);
        bar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_run) startProject(project);
            else if (item.getItemId() == R.id.action_add_file) nameDialog("New file", "", "script.lua", "Create", name -> {
                try { showEditor(store.createFile(directory, name)); } catch (Exception error) { toast(error); }
            });
            else if (item.getItemId() == R.id.action_add_folder) nameDialog("New folder", "", "scripts", "Create", name -> {
                try { store.createFolder(directory, name); showDirectory(directory); } catch (Exception error) { toast(error); }
            });
            else if (item.getItemId() == R.id.action_import) pickAsset();
            else if (item.getItemId() == R.id.action_settings) showSettingsDialog();
            return true;
        });
        RecyclerView list = findViewById(R.id.list);
        list.setLayoutManager(new LinearLayoutManager(this));
        FileAdapter adapter = new FileAdapter(store.children(directory), new FileAdapter.Listener() {
            @Override public void onOpen(File file) { openFile(file); }
            @Override public void onRename(File file) {
                renameDialog(file, renamed -> { showDirectory(directory); toast("Renamed to " + renamed.getName()); });
            }
            @Override public void onDelete(File file) { confirmDelete(file, () -> showDirectory(directory)); }
        });
        list.setAdapter(adapter);
    }

    private void openFile(File file) {
        if (file.isDirectory()) showDirectory(file);
        else if (isImage(file)) showImage(file);
        else showEditor(file);
    }
    private static boolean isImage(File file) {
        String name = file.getName().toLowerCase();
        return name.endsWith(".png") || name.endsWith(".jpg") || name.endsWith(".jpeg") || name.endsWith(".webp");
    }

    private void showImage(File file) {
        imageFile = file;
        directory = file.getParentFile();
        MaterialToolbar bar = toolbar(R.layout.screen_image);
        bar.setTitle(file.getName());
        bar.setSubtitle(relative(file));
        back(bar, () -> showDirectory(file.getParentFile()));
        bar.inflateMenu(R.menu.menu_image);
        bar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_rename) renameDialog(file, renamed -> showImage(renamed));
            else if (item.getItemId() == R.id.action_delete) confirmDelete(file, () -> showDirectory(file.getParentFile()));
            return true;
        });
        ImageView image = findViewById(R.id.image);
        image.setImageURI(Uri.fromFile(file));
        TextView path = findViewById(R.id.path);
        path.setText("Use it with engine.load_texture('" + relative(file) + "').");
    }

    private void showEditor(File file) {
        stopRuntime();
        editorFile = file;
        imageFile = null;
        directory = file.getParentFile();
        MaterialToolbar bar = toolbar(R.layout.screen_editor);
        bar.setTitle(file.getName());
        bar.setSubtitle(relative(file));
        back(bar, () -> { if (saveEditor()) showDirectory(directory); });
        bar.inflateMenu(R.menu.menu_editor);
        bar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_rename) renameEditorFile();
            else if (item.getItemId() == R.id.action_settings) showSettingsDialog();
            else if (item.getItemId() == R.id.action_check) { if (saveEditor()) output.setText(compilerCall(editor.getTextString(), false)); }
            else if (item.getItemId() == R.id.action_generate) { if (saveEditor()) output.setText(compilerCall(editor.getTextString(), true)); }
            else if (item.getItemId() == R.id.action_delete) confirmDelete(editorFile, () -> showDirectory(directory));
            return true;
        });
        editor = findViewById(R.id.editor);
        applyEditorSettings();
        try { editor.setText(store.read(file)); } catch (Exception error) { toast(error); showDirectory(directory); return; }
        findViewById(R.id.key_tab).setOnClickListener(v -> editor.insertSnippet(editor.getTabSnippet()));
        findViewById(R.id.key_colon).setOnClickListener(v -> editor.insertSnippet(":"));
        findViewById(R.id.key_eq).setOnClickListener(v -> editor.insertSnippet(" = "));
        findViewById(R.id.key_parens).setOnClickListener(v -> editor.insertSnippet("()"));
        findViewById(R.id.key_undo).setOnClickListener(v -> editor.undo());
        MaterialButton save = findViewById(R.id.save);
        save.setOnClickListener(v -> { if (saveEditor()) toast("Saved"); });
        MaterialButton run = findViewById(R.id.run);
        run.setOnClickListener(v -> { if (saveEditor()) startProject(project); });
        output = findViewById(R.id.output);
    }

    private void renameEditorFile() {
        if (!saveEditor()) return;
        File current = editorFile;
        renameDialog(current, renamed -> {
            editorFile = renamed;
            directory = renamed.getParentFile();
            MaterialToolbar bar = findViewById(R.id.toolbar);
            bar.setTitle(renamed.getName());
            bar.setSubtitle(relative(renamed));
            toast("Renamed to " + renamed.getName());
        });
    }

    private boolean saveEditor() {
        if (editor == null || editorFile == null) return true;
        try { store.write(editorFile, editor.getTextString()); return true; }
        catch (Exception error) { toast(error); return false; }
    }
    private String compilerCall(String source, boolean generate) {
        if (!nativeAvailable) return "Native library unavailable: " + nativeLoadError;
        try { return generate ? nativeGenerate(source) : nativeCheck(source); }
        catch (Throwable error) { return "Compiler error: " + error; }
    }

    private void startProject(File selected) {
        if (opening) return;
        stopRuntime();
        project = selected;
        if (!new File(selected, "main.lua").isFile()) { toast("main.lua is missing"); return; }
        if (!nativeAvailable) { toast("Native library unavailable: " + nativeLoadError); return; }
        opening = true;
        MaterialToolbar bar = toolbar(R.layout.screen_runtime);
        bar.setTitle("Starting " + selected.getName() + "…");
        bar.setSubtitle("Loading main.lua, modules and textures");
        new Thread(() -> {
            String result;
            try { result = nativeOpenProject(selected.getAbsolutePath()); }
            catch (Throwable error) { result = String.valueOf(error); }
            final String completed = result;
            handler.post(() -> {
                opening = false;
                if (destroyed) { try { nativeCloseProject(); } catch (Throwable ignored) { } return; }
                try { showRuntime(selected, new JSONObject(completed)); }
                catch (Exception error) {
                    new MaterialAlertDialogBuilder(this).setTitle("Project could not start").setMessage(completed)
                        .setPositiveButton("OK", (dialog, which) -> showProject(selected)).show();
                }
            });
        }, "LuaC-project-loader").start();
    }

    private void showRuntime(File selected, JSONObject initial) {
        project = selected;
        console.setLength(0);
        runtimeError = false;
        MaterialToolbar bar = toolbar(R.layout.screen_runtime);
        bar.setTitle(selected.getName());
        bar.setSubtitle("Running • touch the scene");
        bar.inflateMenu(R.menu.menu_runtime);
        bar.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.action_stop) showProject(selected);
            return true;
        });
        runtimeView = new RuntimeView(this, selected, (id, x, y, down) -> {
            if (running) nativeTouchProject(id, x, y, down);
        }, (w, h) -> {
            if (running) nativeSetViewport(w, h);
        });
        runtimeView.setDefaultTypeface(resolvedRuntimeTypeface());
        FrameLayout game = findViewById(R.id.game);
        game.addView(runtimeView, new FrameLayout.LayoutParams(-1, -1));
        output = findViewById(R.id.output);
        running = true;
        previousFrame = 0;
        applyFrame(initial);
        if (running) handler.post(frame);
    }

    private void applyFrame(JSONObject value) {
        if (runtimeView == null) return;
        runtimeView.setFrame(value);
        appendConsole(value.optString("output"));
        if (!value.optBoolean("ok", false) && !runtimeError) {
            runtimeError = true;
            running = false;
            appendConsole("\n" + value.optString("error", "Runtime stopped") + "\n");
        }
    }
    private void appendConsole(String message) {
        if (message == null || message.isEmpty() || output == null) return;
        console.append(message);
        if (console.length() > 100000) console.delete(0, console.length() - 80000);
        output.setText(console.toString());
    }

    private void stopRuntime() {
        handler.removeCallbacks(frame);
        boolean wasRunning = running;
        running = false;
        previousFrame = 0;
        if (nativeAvailable && (wasRunning || runtimeView != null)) {
            try { nativeCloseProject(); } catch (Throwable ignored) { }
        }
        if (runtimeView != null) runtimeView.dispose();
        runtimeView = null;
    }

    private void pickAsset() {
        importDirectory = directory != null ? directory : new File(project, "assets");
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_ASSET);
    }

    private void pickCustomFont() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, PICK_FONT);
    }

    private Typeface resolvedRuntimeTypeface() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        int fontFamily = prefs.getInt("font_family", 0);
        String customPath = prefs.getString("custom_font_path", "");
        if (fontFamily == 1) return Typeface.SANS_SERIF;
        if (fontFamily == 2) return Typeface.SERIF;
        if (fontFamily == 3) return Typeface.create(Typeface.MONOSPACE, Typeface.BOLD);
        if (fontFamily == 4 && !customPath.isEmpty()) {
            try {
                File f = new File(customPath);
                if (f.isFile()) {
                    Typeface custom = Typeface.createFromFile(f);
                    if (custom != null) return custom;
                }
            } catch (Exception ignored) { }
        }
        return Typeface.SANS_SERIF;
    }

    private void applyEditorSettings() {
        applyEditorSettings(editor);
    }

    private void applyEditorSettings(CodeEditorView target) {
        if (target == null) return;
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        target.applySettings(
            prefs.getInt("font_family", 0),
            prefs.getInt("font_size", 14),
            prefs.getBoolean("line_numbers", true),
            prefs.getInt("tab_size", 4),
            prefs.getInt("theme", 0),
            prefs.getString("custom_font_path", "")
        );
    }

    private void showSettingsDialog() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        View view = LayoutInflater.from(this).inflate(R.layout.dialog_settings, null);
        Spinner fontSpinner = view.findViewById(R.id.spinner_font);
        MaterialButton importFontBtn = view.findViewById(R.id.btn_import_font);
        TextView sizeLabel = view.findViewById(R.id.label_font_size);
        SeekBar sizeSeek = view.findViewById(R.id.seek_font_size);
        Spinner themeSpinner = view.findViewById(R.id.spinner_theme);
        Spinner tabSpinner = view.findViewById(R.id.spinner_tab);
        SwitchMaterial lineNumbersSwitch = view.findViewById(R.id.switch_line_numbers);

        String customPath = prefs.getString("custom_font_path", "");
        String customLabel = customPath.isEmpty() ? "Custom font (none imported)" : "Custom: " + new File(customPath).getName();
        String[] fonts = {"Monospace", "Sans Serif", "Serif", "Bold Monospace", customLabel};
        ArrayAdapter<String> fontAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, fonts);
        fontAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        fontSpinner.setAdapter(fontAdapter);
        fontSpinner.setSelection(Math.max(0, Math.min(4, prefs.getInt("font_family", 0))));

        String[] themes = {"Slate Dark", "OLED Black", "Midnight Indigo", "Forest Dark"};
        ArrayAdapter<String> themeAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, themes);
        themeAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        themeSpinner.setAdapter(themeAdapter);
        themeSpinner.setSelection(Math.max(0, Math.min(3, prefs.getInt("theme", 0))));

        String[] tabs = {"4 spaces", "2 spaces"};
        ArrayAdapter<String> tabAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, tabs);
        tabAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        tabSpinner.setAdapter(tabAdapter);
        tabSpinner.setSelection(prefs.getInt("tab_size", 4) == 2 ? 1 : 0);

        int currentSize = Math.max(10, Math.min(26, prefs.getInt("font_size", 14)));
        sizeSeek.setProgress(currentSize - 10);
        sizeLabel.setText("Font size: " + currentSize + "sp");
        sizeSeek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                sizeLabel.setText("Font size: " + (10 + p) + "sp");
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        lineNumbersSwitch.setChecked(prefs.getBoolean("line_numbers", true));

        androidx.appcompat.app.AlertDialog dialog = new MaterialAlertDialogBuilder(this)
            .setTitle("Studio Settings")
            .setView(view)
            .setPositiveButton("Apply", (d, w) -> {
                prefs.edit()
                    .putInt("font_family", fontSpinner.getSelectedItemPosition())
                    .putInt("font_size", 10 + sizeSeek.getProgress())
                    .putInt("theme", themeSpinner.getSelectedItemPosition())
                    .putInt("tab_size", tabSpinner.getSelectedItemPosition() == 1 ? 2 : 4)
                    .putBoolean("line_numbers", lineNumbersSwitch.isChecked())
                    .apply();
                applyEditorSettings();
                if (runtimeView != null) runtimeView.setDefaultTypeface(resolvedRuntimeTypeface());
                if (studio != null) studio.refreshSettings();
                toast("Settings saved");
            })
            .setNegativeButton("Cancel", null)
            .create();

        importFontBtn.setOnClickListener(v -> {
            dialog.dismiss();
            pickCustomFont();
        });
        dialog.show();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == PICK_ASSET) {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                File imported = store.importFile(importDirectory, displayName(uri), input);
                toast("Imported " + imported.getName());
                if (project != null && studio == null) showDirectory(importDirectory);
            } catch (Exception error) { toast(error); }
        } else if (request == PICK_FONT) {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                File fontsDir = new File(getFilesDir(), "fonts");
                if (!fontsDir.exists()) fontsDir.mkdirs();
                String name = displayName(uri);
                if (!name.toLowerCase().endsWith(".ttf") && !name.toLowerCase().endsWith(".otf")) name += ".ttf";
                File dest = new File(fontsDir, name);
                try (FileOutputStream out = new FileOutputStream(dest)) {
                    byte[] buf = new byte[16384];
                    int n;
                    while ((n = input.read(buf)) >= 0) out.write(buf, 0, n);
                }
                Typeface test = Typeface.createFromFile(dest);
                if (test == null) throw new IllegalArgumentException("Invalid font file");
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("font_family", 4)
                    .putString("custom_font_path", dest.getAbsolutePath())
                    .apply();
                applyEditorSettings();
                if (runtimeView != null) runtimeView.setDefaultTypeface(test);
                if (studio != null) studio.refreshSettings();
                toast("Font applied: " + dest.getName());
            } catch (Exception error) { toast(error); }
        }
    }
    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) return cursor.getString(column);
            }
        }
        return "asset.png";
    }

    private void nameDialog(String title, String initial, String hint, String positive, NameAction action) {
        View view = LayoutInflater.from(this).inflate(R.layout.dialog_name, null);
        TextInputEditText input = view.findViewById(R.id.input);
        input.setText(initial);
        input.setHint(hint);
        if (!initial.isEmpty()) { input.setSelection(initial.length()); }
        new MaterialAlertDialogBuilder(this).setTitle(title).setView(view)
            .setPositiveButton(positive, (dialog, which) -> action.run(input.getText().toString()))
            .setNegativeButton("Cancel", null).show();
    }
    private void renameDialog(File file, RenameAction after) {
        String title = file.getName().equals("main.lua") ? "Rename entry point (main.lua is required to run)" : "Rename (references are not updated)";
        nameDialog(title, file.getName(), file.getName(), "Rename", name -> {
            try {
                File renamed = store.rename(file, name);
                if (renamed.equals(file)) { toast("Name unchanged"); return; }
                after.run(renamed);
            } catch (Exception error) { toast(error); }
        });
    }
    private interface NameAction { void run(String name); }
    private interface RenameAction { void run(File renamed); }

    private void confirmDelete(File file, Runnable after) {
        new MaterialAlertDialogBuilder(this).setTitle("Delete " + file.getName() + "?")
            .setMessage(file.isDirectory() ? "The folder and everything inside it will be deleted." : "This cannot be undone.")
            .setPositiveButton("Delete", (dialog, which) -> {
                try { store.delete(file); after.run(); } catch (Exception error) { toast(error); }
            }).setNegativeButton("Cancel", null).show();
    }

    @Override public void onBackPressed() {
        if (opening) return;
        if (studio != null) { studio.dispose(); studio = null; showProjects(); return; }
        if (running || runtimeView != null) { showProject(project); return; }
        if (editorFile != null) { if (saveEditor()) showDirectory(directory); return; }
        if (imageFile != null) { showDirectory(imageFile.getParentFile()); return; }
        if (project != null) {
            if (directory != null && !directory.equals(project)) showDirectory(directory.getParentFile());
            else showProjects();
            return;
        }
        super.onBackPressed();
    }
    @Override protected void onPause() {
        if (editorFile != null) saveEditor();
        if (studio != null) studio.dispose();
        super.onPause();
    }
    @Override protected void onDestroy() {
        destroyed = true;
        if (studio != null) { studio.dispose(); studio = null; }
        stopRuntime();
        if (nativeAvailable) try { nativeVulkanDestroy(); } catch (Throwable ignored) { }
        super.onDestroy();
    }
}
