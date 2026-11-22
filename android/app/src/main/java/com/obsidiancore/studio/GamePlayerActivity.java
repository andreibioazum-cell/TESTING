package com.obsidiancore.studio;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

public class GamePlayerActivity extends AppCompatActivity implements Choreographer.FrameCallback {
    static { System.loadLibrary("obsidian_native"); }

    private PlayerSurfaceView surfaceView;
    private boolean surfaceReady = false;
    private boolean runningLoop = false;
    private long lastFrameNs = 0L;

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

    private static String readAssetText(AssetManager am, String path) throws IOException {
        try (InputStream in = am.open(path); ByteArrayOutputStream bos = new ByteArrayOutputStream()) {
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) >= 0) bos.write(buf, 0, n);
            return bos.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static void copyAssetTree(AssetManager am, String assetDir, File dstDir) throws IOException {
        String[] entries = am.list(assetDir);
        if (entries == null || entries.length == 0) {
            // Single file
            File parent = dstDir.getParentFile();
            if (parent != null && !parent.exists()) parent.mkdirs();
            try (InputStream in = am.open(assetDir); FileOutputStream out = new FileOutputStream(dstDir)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) >= 0) out.write(buf, 0, n);
            }
            return;
        }
        if (!dstDir.exists()) dstDir.mkdirs();
        for (String name : entries) {
            copyAssetTree(am, assetDir + "/" + name, new File(dstDir, name));
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        String projName = "Game";
        boolean is3D = true;
        String buildId = "1";
        File runtimeRoot = new File(getFilesDir(), "game_runtime");

        try {
            AssetManager am = getAssets();
            JSONObject meta = new JSONObject(readAssetText(am, "game_meta.json"));
            projName = meta.optString("projectName", "Game");
            is3D = meta.optBoolean("is3D", true);
            buildId = meta.optString("buildId", "1");

            File projDir = new File(runtimeRoot, projName);
            File stamp = new File(projDir, ".build_id");
            boolean needExtract = !projDir.isDirectory() || !stamp.isFile();
            if (!needExtract) {
                try (InputStream in = new java.io.FileInputStream(stamp)) {
                    byte[] b = in.readAllBytes();
                    if (!buildId.equals(new String(b, StandardCharsets.UTF_8))) needExtract = true;
                }
            }
            if (needExtract) {
                copyAssetTree(am, "game_project", projDir);
                try (FileOutputStream out = new FileOutputStream(stamp)) {
                    out.write(buildId.getBytes(StandardCharsets.UTF_8));
                }
            }
        } catch (Exception ignored) {
        }

        float density = getResources().getDisplayMetrics().density;
        nativePlayerInit(runtimeRoot.getAbsolutePath(), projName, is3D, density);
        surfaceView = new PlayerSurfaceView(this);
        setContentView(surfaceView);
    }

    @SuppressWarnings("unused")
    public void nativeCbSetOrientation(boolean portrait) {
        runOnUiThread(() -> setRequestedOrientation(
            portrait ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                     : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE));
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
        nativePlayerDestroy();
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
        nativePlayerStep(dt);
        if (surfaceReady) nativeVulkanDraw();
        Choreographer.getInstance().postFrameCallback(this);
    }

    private final class PlayerSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
        PlayerSurfaceView(Context ctx) {
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
            if (surfaceReady) nativeVulkanResize(width, height);
            else surfaceReady = nativeVulkanCreate(holder.getSurface(), width, height);
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
            nativePlayerTouch(ev.getActionMasked(), ev.getActionIndex(), ids, xs, ys);
            return true;
        }
    }

    private native void nativePlayerInit(String projectsRoot, String projName, boolean is3D, float density);
    private static native void nativePlayerStep(float dt);
    private static native void nativePlayerTouch(int action, int pointerIndex, int[] ids, float[] xs, float[] ys);
    private static native void nativePlayerDestroy();
    private static native boolean nativeVulkanCreate(Surface surface, int width, int height);
    private static native void nativeVulkanResize(int width, int height);
    private static native void nativeVulkanDraw();
    private static native void nativeVulkanDestroy();
}
