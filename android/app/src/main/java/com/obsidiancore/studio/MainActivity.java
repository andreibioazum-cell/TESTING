package com.obsidiancore.studio;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

public class MainActivity extends AppCompatActivity implements Choreographer.FrameCallback {
    static { System.loadLibrary("obsidian_native"); }

    private static final String PREFS = "obsidian_core_prefs";
    private static final String PREF_FONT_FAMILY = "font_family";
    private static final String PREF_FONT_SIZE = "font_size";
    private static final String PREF_THEME = "theme";
    private static final String PREF_TAB_SPACES = "tab_spaces";
    private static final String PREF_LINE_NUMS = "line_numbers";
    private static final String PREF_CUSTOM_FONT = "custom_font_path";

    private ProjectStore store;
    private StudioSurfaceView surfaceView;
    private boolean surfaceReady = false;
    private boolean runningLoop = false;
    private long lastFrameNs = 0L;
    private String pendingTexProj = "";
    private String pendingTexNode = "";
    private String pendingModelProj = "";
    private String pendingModelNode = "";

    private final ActivityResultLauncher<String[]> fontPicker =
        registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
            if (uri != null) importCustomFont(uri);
        });

    private final ActivityResultLauncher<String[]> texturePicker =
        registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
            if (uri != null) importNodeTexture(uri);
        });

    private final ActivityResultLauncher<String[]> modelPicker =
        registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
            if (uri != null) importNodeModel(uri);
        });

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        View decor = getWindow().getDecorView();
        WindowInsetsControllerCompat controller = WindowCompat.getInsetsController(getWindow(), decor);
        if (controller != null) {
            controller.hide(WindowInsetsCompat.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        decor.setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        );
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        store = new ProjectStore(this);
        ensureDefaultProjects();

        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        int fam = p.getInt(PREF_FONT_FAMILY, 0);
        int sz = p.getInt(PREF_FONT_SIZE, 14);
        int theme = p.getInt(PREF_THEME, 0);
        int tabs = p.getInt(PREF_TAB_SPACES, 4);
        boolean lineNums = p.getBoolean(PREF_LINE_NUMS, true);
        String customFont = p.getString(PREF_CUSTOM_FONT, "");

        List<File> list = store.projects();
        File initial = null;
        for (File f : list) {
            if (store.is3DProject(f) && (new File(f, "scripts/player.obs").isFile() || new File(f, "scripts/player.lua").isFile())) {
                initial = f;
                break;
            }
        }
        if (initial == null) {
            for (File f : list) {
                if (store.is3DProject(f)) { initial = f; break; }
            }
        }
        if (initial == null && !list.isEmpty()) initial = list.get(0);
        String initName = initial != null ? initial.getName() : "3D Игра";
        boolean is3D = initial != null && store.is3DProject(initial);

        float density = getResources().getDisplayMetrics().density;
        nativeStudioInit(store.root.getAbsolutePath(), initName, is3D, density, fam, sz, theme, tabs, lineNums, customFont);

        surfaceView = new StudioSurfaceView(this);
        setContentView(surfaceView);
    }

    private void ensureDefaultProjects() {
        store.upgradeLegacySdk();
        store.upgradeLegacySnakeIfUntouched();
        List<File> list = store.projects();
        boolean has3DJoystick = false, hasSnake = false;
        for (File f : list) {
            if (store.is3DProject(f) && (new File(f, "scripts/player.obs").isFile() || new File(f, "scripts/player.lua").isFile())) {
                has3DJoystick = true;
            }
            if (f.getName().contains("Змейка")) hasSnake = true;
        }
        try {
            if (!has3DJoystick) store.create3DScratchProject("3D Игра");
            if (!hasSnake) store.createSnakeProject("Змейка");
        } catch (Exception ignored) {
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (!runningLoop) {
            runningLoop = true;
            lastFrameNs = 0L;
            Choreographer.getInstance().postFrameCallback(this);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onPause() {
        runningLoop = false;
        Choreographer.getInstance().removeFrameCallback(this);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        runningLoop = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeVulkanDestroy();
        nativeStudioDestroy();
        super.onDestroy();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!runningLoop) return;
        float dt = 0.016f;
        if (lastFrameNs != 0L) {
            dt = Math.max(0.001f, Math.min(0.05f, (frameTimeNanos - lastFrameNs) / 1_000_000_000.0f));
        }
        lastFrameNs = frameTimeNanos;
        nativeStudioStep(dt);
        if (surfaceReady) nativeVulkanDraw();
        Choreographer.getInstance().postFrameCallback(this);
    }

    private void importCustomFont(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) return;
            File dir = new File(getFilesDir(), "fonts");
            if (!dir.exists()) dir.mkdirs();
            File dst = new File(dir, "custom_font.ttf");
            try (FileOutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) >= 0) out.write(buf, 0, n);
            }
            nativeStudioSetCustomFont(dst.getAbsolutePath());
        } catch (Exception ignored) {
        }
    }

    @SuppressWarnings("unused")
    public void nativeCbSetKeyboard(boolean show) {
        runOnUiThread(() -> {
            if (surfaceView == null) return;
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null) return;
            if (show) {
                surfaceView.requestFocus();
                imm.showSoftInput(surfaceView, InputMethodManager.SHOW_IMPLICIT);
            } else {
                imm.hideSoftInputFromWindow(surfaceView.getWindowToken(), 0);
                hideSystemBars();
            }
        });
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null) return n;
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    private void importNodeModel(Uri uri) {
        if (pendingModelProj.isEmpty()) return;
        try {
            File projDir = new File(store.root, pendingModelProj);
            File dir = new File(projDir, "assets");
            if (!dir.exists()) dir.mkdirs();
            byte[] data;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) return;
                data = in.readAllBytes();
            }
            boolean isGlb = data.length >= 4 && data[0] == 'g' && data[1] == 'l' && data[2] == 'T' && data[3] == 'F';
            String dname = queryDisplayName(uri).toLowerCase();
            if (!isGlb && dname.endsWith(".glb")) isGlb = true;
            String ext = isGlb ? ".glb" : ".gltf";
            String base = "model_" + System.currentTimeMillis();
            if (!dname.isEmpty()) {
                int dot = dname.lastIndexOf('.');
                String stem = (dot > 0 ? dname.substring(0, dot) : dname).replaceAll("[^a-zA-Z0-9_-]", "_");
                if (!stem.isEmpty()) base = stem + "_" + (System.currentTimeMillis() % 10000);
            }
            String relPath = "assets/" + base + ext;
            File dst = new File(projDir, relPath);
            try (OutputStream out = new FileOutputStream(dst)) {
                out.write(data);
            }
            nativeStudioApplyModel(pendingModelNode, relPath);
        } catch (Exception ignored) {
        }
    }

    private void importNodeTexture(Uri uri) {
        if (pendingTexProj.isEmpty() || pendingTexNode.isEmpty()) return;
        try {
            File projDir = new File(store.root, pendingTexProj);
            File dir = new File(projDir, "assets");
            if (!dir.exists()) dir.mkdirs();
            String mime = getContentResolver().getType(uri);
            String ext = (mime != null && (mime.contains("jpeg") || mime.contains("jpg"))) ? ".jpg" : ".png";
            String relPath = "assets/tex_" + System.currentTimeMillis() + ext;
            File dst = new File(projDir, relPath);
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dst)) {
                if (in == null) return;
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            nativeStudioApplyTexture(pendingTexNode, relPath);
        } catch (Exception ignored) {
        }
    }

    @SuppressWarnings("unused")
    public void nativeCbOpenFontPicker() {
        runOnUiThread(() -> fontPicker.launch(new String[]{"*/*"}));
    }

    @SuppressWarnings("unused")
    public void nativeCbOpenTexturePicker(String projectName, String nodeId) {
        pendingTexProj = projectName != null ? projectName : "";
        pendingTexNode = nodeId != null ? nodeId : "";
        runOnUiThread(() -> texturePicker.launch(new String[]{"image/*"}));
    }

    @SuppressWarnings("unused")
    public void nativeCbOpenModelPicker(String projectName, String nodeId) {
        pendingModelProj = projectName != null ? projectName : "";
        pendingModelNode = nodeId != null ? nodeId : "";
        runOnUiThread(() -> modelPicker.launch(new String[]{"*/*"}));
    }

    @SuppressWarnings("unused")
    public void nativeCbSetOrientation(boolean portrait) {
        runOnUiThread(() -> setRequestedOrientation(
            portrait ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                     : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE));
    }

    @SuppressWarnings("unused")
    public void nativeCbCreateProject(String kind, String name) {
        try {
            if ("3d_scratch".equals(kind)) store.create3DScratchProject(name);
            else if ("3d".equals(kind) || "3d_example".equals(kind)) store.create3DProject(name);
            else store.createProject(name);
        } catch (Exception ignored) {
        }
    }

    @SuppressWarnings("unused")
    public void nativeCbRenameProject(String oldName, String newName) {
        try {
            File target = new File(store.root, oldName);
            if (target.isDirectory()) store.rename(target, newName);
        } catch (Exception ignored) {
        }
    }

    @SuppressWarnings("unused")
    public void nativeCbDeleteProject(String name) {
        try {
            File target = new File(store.root, name);
            if (target.isDirectory()) store.delete(target);
        } catch (Exception ignored) {
        }
    }

    @SuppressWarnings("unused")
    public String[] nativeCbListProjects() {
        List<File> list = store.projects();
        String[] out = new String[list.size()];
        for (int i = 0; i < list.size(); i++) {
            File f = list.get(i);
            out[i] = f.getName() + "|" + (store.is3DProject(f) ? "3d" : "2d");
        }
        return out;
    }

    @SuppressWarnings("unused")
    public void nativeCbSavePrefs(int fam, int sz, int theme, int tabs, boolean lineNums, String customFont) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putInt(PREF_FONT_FAMILY, fam)
            .putInt(PREF_FONT_SIZE, sz)
            .putInt(PREF_THEME, theme)
            .putInt(PREF_TAB_SPACES, tabs)
            .putBoolean(PREF_LINE_NUMS, lineNums)
            .putString(PREF_CUSTOM_FONT, customFont == null ? "" : customFont)
            .apply();
    }

    private final class StudioSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
        StudioSurfaceView(Context ctx) {
            super(ctx);
            setFocusable(true);
            setFocusableInTouchMode(true);
            getHolder().addCallback(this);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            int w = getWidth(), h = getHeight();
            if (w > 0 && h > 0) {
                surfaceReady = nativeVulkanCreate(holder.getSurface(), w, h);
            }
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            if (width <= 0 || height <= 0) return;
            if (surfaceReady) {
                nativeVulkanResize(width, height);
            } else {
                surfaceReady = nativeVulkanCreate(holder.getSurface(), width, height);
            }
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            surfaceReady = false;
            nativeVulkanDestroy();
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            int n = ev.getPointerCount();
            int[] ids = new int[n];
            float[] xs = new float[n];
            float[] ys = new float[n];
            for (int i = 0; i < n; i++) {
                ids[i] = ev.getPointerId(i);
                xs[i] = ev.getX(i);
                ys[i] = ev.getY(i);
            }
            nativeStudioTouch(ev.getActionMasked(), ev.getActionIndex(), ids, xs, ys);
            return true;
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE;
            return new BaseInputConnection(this, false) {
                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    if (text != null && text.length() > 0) nativeStudioTextInput(text.toString());
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                    for (int i = 0; i < beforeLength; i++) nativeStudioKeyCode(KeyEvent.KEYCODE_DEL);
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    if (event.getAction() == KeyEvent.ACTION_DOWN) {
                        int code = event.getKeyCode();
                        if (code == KeyEvent.KEYCODE_DEL || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_TAB ||
                            (code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_RIGHT)) {
                            nativeStudioKeyCode(code);
                            return true;
                        }
                        int ch = event.getUnicodeChar();
                        if (ch > 0) {
                            nativeStudioTextInput(new String(Character.toChars(ch)));
                            return true;
                        }
                    }
                    return super.sendKeyEvent(event);
                }
            };
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent event) {
            if (keyCode == KeyEvent.KEYCODE_DEL || keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_TAB ||
                (keyCode >= KeyEvent.KEYCODE_DPAD_UP && keyCode <= KeyEvent.KEYCODE_DPAD_RIGHT)) {
                nativeStudioKeyCode(keyCode);
                return true;
            }
            int ch = event.getUnicodeChar();
            if (ch > 0) {
                nativeStudioTextInput(new String(Character.toChars(ch)));
                return true;
            }
            return super.onKeyDown(keyCode, event);
        }

        @Override
        public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
            if (keyCode == KeyEvent.KEYCODE_UNKNOWN && event.getCharacters() != null) {
                nativeStudioTextInput(event.getCharacters());
                return true;
            }
            return super.onKeyMultiple(keyCode, repeatCount, event);
        }
    }

    private native void nativeStudioInit(String projectsRoot, String initialProj, boolean is3D,
                                         float density, int fam, int sz, int theme, int tabs,
                                         boolean lineNums, String customFont);
    private static native void nativeStudioStep(float dt);
    private static native void nativeStudioTouch(int action, int pointerIndex, int[] ids, float[] xs, float[] ys);
    private static native void nativeStudioTextInput(String utf8);
    private static native void nativeStudioKeyCode(int keyCode);
    private static native void nativeStudioSetCustomFont(String path);
    private static native void nativeStudioApplyTexture(String nodeId, String relPath);
    private static native void nativeStudioApplyModel(String nodeId, String relPath);
    private static native void nativeStudioDestroy();

    private static native boolean nativeVulkanCreate(Surface surface, int width, int height);
    private static native void nativeVulkanResize(int width, int height);
    private static native void nativeVulkanDraw();
    private static native void nativeVulkanDestroy();
}
