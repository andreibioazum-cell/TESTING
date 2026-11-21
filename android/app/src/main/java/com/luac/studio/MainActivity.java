package com.luac.studio;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.List;

public class MainActivity extends AppCompatActivity implements Choreographer.FrameCallback {
    static { System.loadLibrary("luac_native"); }

    private static final String PREFS = "luac_studio_prefs";
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

    private final ActivityResultLauncher<String[]> fontPicker =
        registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
            if (uri != null) importCustomFont(uri);
        });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
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
            if (store.is3DProject(f) && new File(f, "scripts/player.lua").isFile()) { initial = f; break; }
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
        store.upgradeLegacyStudioSdk();
        store.upgradeLegacySnakeIfUntouched();
        List<File> list = store.projects();
        boolean has3DJoystick = false, hasSnake = false;
        for (File f : list) {
            if (store.is3DProject(f) && new File(f, "scripts/player.lua").isFile()) has3DJoystick = true;
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
        if (!runningLoop) {
            runningLoop = true;
            lastFrameNs = 0L;
            Choreographer.getInstance().postFrameCallback(this);
        }
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

    // Callbacks invoked from C++ StudioApp
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
            }
        });
    }

    @SuppressWarnings("unused")
    public void nativeCbOpenFontPicker() {
        runOnUiThread(() -> fontPicker.launch(new String[]{"*/*"}));
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
    private static native void nativeStudioDestroy();

    private static native boolean nativeVulkanCreate(Surface surface, int width, int height);
    private static native void nativeVulkanResize(int width, int height);
    private static native void nativeVulkanDraw();
    private static native void nativeVulkanDestroy();
}
