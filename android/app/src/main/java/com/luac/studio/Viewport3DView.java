package com.luac.studio;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;
import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;

public final class Viewport3DView extends View {
    enum ToolMode { SELECT, MOVE, SCALE, ROTATE }

    interface Listener {
        void onNodeSelected(SceneModel.Node node);
        void onSceneChanged();
    }

    private static final class Vec3 {
        float x, y, z;
        Vec3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
    }

    private static final class Face {
        float[] sx, sy;
        int count;
        float depth;
        int fillColor;
        int strokeColor;
        float strokeWidth;
        boolean isBaseplateTop;
        boolean isSpawnTop;
        SceneModel.Node owner;
        Vec3[] worldPts;
    }

    private static final class GizmoHandle {
        int axis; // 0=X, 1=Y, 2=Z
        float x1, y1, x2, y2;
        int color;
    }

    private SceneModel scene;
    private SceneModel.Node selected;
    private ToolMode toolMode = ToolMode.SELECT;
    private Listener listener;
    private File projectDir;
    private Typeface defaultTypeface = Typeface.SANS_SERIF;
    private final HashMap<String, Typeface> fontCache = new HashMap<>();

    private boolean showGrid = true;
    private boolean collisionsEnabled = false;
    private float snapGrid = 0.5f;

    // Camera orbit state
    private float camTargetX = 0f, camTargetY = 2.5f, camTargetZ = -3f;
    private float camYaw = 0.36f;   // radians
    private float camPitch = 0.34f; // radians
    private float camDist = 36f;

    // Cached camera basis
    private float eyeX, eyeY, eyeZ;
    private float fwdX, fwdY, fwdZ;
    private float rightX, rightY, rightZ;
    private float upX, upY, upZ;
    private float focalLen = 600f;

    // Touch / gizmo drag state
    private final ArrayList<GizmoHandle> activeHandles = new ArrayList<>();
    private final ArrayList<Face> lastSortedFaces = new ArrayList<>();
    private int dragMode = 0; // 0=none, 1=orbit, 2=pan, 3=gizmo, 4=joystick
    private int dragAxis = -1;
    private float lastTouchX, lastTouchY, downTouchX, downTouchY;
    private float lastPinchDist = -1f;
    private boolean touchMoved = false;

    // Play mode state
    private boolean playMode = false;
    private float playerX = 0f, playerY = 3f, playerZ = 8f;
    private float playerVy = 0f, playerYaw = 0f;
    private boolean playerGrounded = false;
    private float joyDx = 0f, joyDy = 0f;
    private int joyPointerId = -1;
    private float joyCenterX, joyCenterY, joyRadius;
    private float jumpBtnX, jumpBtnY, jumpBtnR;
    private float walkCycle = 0f;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path polyPath = new Path();
    private final ArrayList<Face> frameFaces = new ArrayList<>();

    public Viewport3DView(Context context) {
        this(context, null);
    }

    public Viewport3DView(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public Viewport3DView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        fillPaint.setStyle(Paint.Style.FILL);
        strokePaint.setStyle(Paint.Style.STROKE);
        setFocusable(true);
    }

    void bind(SceneModel scene, File projectDir, Listener listener) {
        this.scene = scene;
        this.projectDir = projectDir;
        this.listener = listener;
        invalidate();
    }

    void setSelected(SceneModel.Node node) {
        this.selected = node;
        invalidate();
    }

    void setToolMode(ToolMode mode) {
        this.toolMode = mode;
        invalidate();
    }

    ToolMode getToolMode() { return toolMode; }

    void setShowGrid(boolean show) { this.showGrid = show; invalidate(); }
    void setCollisionsEnabled(boolean enabled) { this.collisionsEnabled = enabled; }
    void setSnapGrid(float snap) { this.snapGrid = snap; }

    void setDefaultTypeface(Typeface tf) {
        this.defaultTypeface = tf != null ? tf : Typeface.SANS_SERIF;
        invalidate();
    }

    void resetCamera() {
        camTargetX = 0f; camTargetY = 2.5f; camTargetZ = -3f;
        camYaw = 0.36f; camPitch = 0.34f; camDist = 36f;
        invalidate();
    }

    void startPlayMode() {
        playMode = true;
        SceneModel.Node spawn = scene != null ? scene.spawnLocation() : null;
        if (spawn != null) {
            playerX = spawn.posX;
            playerY = spawn.posY + spawn.sizeY * 0.5f + 2.6f;
            playerZ = spawn.posZ;
        } else {
            playerX = 0f; playerY = 4f; playerZ = 8f;
        }
        playerVy = 0f;
        playerYaw = 0f;
        joyDx = 0f; joyDy = 0f;
        camDist = 18f;
        camPitch = 0.28f;
        invalidate();
    }

    void stopPlayMode() {
        playMode = false;
        joyDx = 0f; joyDy = 0f;
        joyPointerId = -1;
        camDist = 36f;
        camPitch = 0.34f;
        camTargetX = 0f; camTargetY = 2.5f; camTargetZ = -3f;
        invalidate();
    }

    boolean isPlayMode() { return playMode; }

    void stepPlayPhysics(float dt) {
        if (!playMode || scene == null) return;
        float speed = 14f;
        float moveRight = joyDx;
        float moveForward = -joyDy;
        if (Math.abs(moveRight) > 0.05f || Math.abs(moveForward) > 0.05f) {
            // Move relative to camera yaw
            float sinY = (float) Math.sin(camYaw);
            float cosY = (float) Math.cos(camYaw);
            float wx = moveRight * cosY - moveForward * sinY;
            float wz = -moveRight * sinY - moveForward * cosY;
            float len = (float) Math.hypot(wx, wz);
            if (len > 0.001f) {
                float stepX = (wx / len) * speed * Math.min(1f, (float) Math.hypot(moveRight, moveForward)) * dt;
                float stepZ = (wz / len) * speed * Math.min(1f, (float) Math.hypot(moveRight, moveForward)) * dt;
                if (!collidesHorizontal(playerX + stepX, playerY, playerZ)) playerX += stepX;
                if (!collidesHorizontal(playerX, playerY, playerZ + stepZ)) playerZ += stepZ;
                playerYaw = (float) Math.atan2(wx, wz);
                walkCycle += dt * 10f;
            }
        } else {
            walkCycle = 0f;
        }

        playerVy -= 38f * dt;
        playerY += playerVy * dt;

        float groundY = findGroundBelow(playerX, playerY, playerZ);
        float halfCharH = 2.5f;
        if (playerY - halfCharH <= groundY) {
            playerY = groundY + halfCharH;
            playerVy = 0f;
            playerGrounded = true;
        } else {
            playerGrounded = false;
        }
        if (playerY < -25f) {
            startPlayMode();
        }
        camTargetX = playerX;
        camTargetY = playerY + 1.2f;
        camTargetZ = playerZ;
        invalidate();
    }

    private float findGroundBelow(float px, float py, float pz) {
        float bestY = -40f;
        if (scene == null) return bestY;
        float r = 0.8f;
        for (SceneModel.Node n : scene.all3DParts()) {
            if (!n.canCollide) continue;
            float top = surfaceHeightAt(n, px, pz, r);
            if (top > bestY && top <= py - 0.5f) {
                bestY = top;
            }
        }
        return bestY;
    }

    private float surfaceHeightAt(SceneModel.Node n, float px, float pz, float r) {
        float hx = n.sizeX * 0.5f + r;
        float hz = n.sizeZ * 0.5f + r;
        float dx = px - n.posX;
        float dz = pz - n.posZ;
        // Transform into local Y-rotated space
        float rad = (float) Math.toRadians(-n.rotY);
        float c = (float) Math.cos(rad);
        float s = (float) Math.sin(rad);
        float lx = dx * c - dz * s;
        float lz = dx * s + dz * c;
        if (Math.abs(lx) > hx || Math.abs(lz) > hz) return -999f;
        if ("WedgePart".equals(n.className)) {
            float t = Math.max(0f, Math.min(1f, (lz + n.sizeZ * 0.5f) / Math.max(0.01f, n.sizeZ)));
            return n.posY - n.sizeY * 0.5f + n.sizeY * t;
        }
        return n.posY + n.sizeY * 0.5f;
    }

    private boolean collidesHorizontal(float px, float py, float pz) {
        if (scene == null) return false;
        float r = 0.8f;
        float charBot = py - 1.7f; // allow stepping onto ramps/low curbs
        float charTop = py + 2.2f;
        for (SceneModel.Node n : scene.all3DParts()) {
            if (!n.canCollide || "WedgePart".equals(n.className)) continue;
            float partBot = n.posY - n.sizeY * 0.5f;
            float partTop = n.posY + n.sizeY * 0.5f;
            if (partTop <= charBot || partBot >= charTop) continue;
            if (Math.abs(px - n.posX) < n.sizeX * 0.5f + r && Math.abs(pz - n.posZ) < n.sizeZ * 0.5f + r) {
                return true;
            }
        }
        return false;
    }

    private void updateCameraBasis(int w, int h) {
        float cp = (float) Math.cos(camPitch);
        float sp = (float) Math.sin(camPitch);
        float cy = (float) Math.cos(camYaw);
        float sy = (float) Math.sin(camYaw);

        eyeX = camTargetX + camDist * cp * sy;
        eyeY = camTargetY + camDist * sp;
        eyeZ = camTargetZ + camDist * cp * cy;

        fwdX = camTargetX - eyeX;
        fwdY = camTargetY - eyeY;
        fwdZ = camTargetZ - eyeZ;
        float fl = (float) Math.sqrt(fwdX * fwdX + fwdY * fwdY + fwdZ * fwdZ);
        if (fl < 0.001f) fl = 1f;
        fwdX /= fl; fwdY /= fl; fwdZ /= fl;

        // right = normalize(cross(fwd, (0, 1, 0)))
        rightX = -fwdZ;
        rightY = 0f;
        rightZ = fwdX;
        float rl = (float) Math.hypot(rightX, rightZ);
        if (rl < 0.001f) { rightX = 1f; rightZ = 0f; }
        else { rightX /= rl; rightZ /= rl; }

        // up = cross(right, fwd)
        upX = rightY * fwdZ - rightZ * fwdY;
        upY = rightZ * fwdX - rightX * fwdZ;
        upZ = rightX * fwdY - rightY * fwdX;

        focalLen = Math.min(w, h) * 0.92f;
    }

    private Vec3 toCamera(Vec3 p) {
        float dx = p.x - eyeX;
        float dy = p.y - eyeY;
        float dz = p.z - eyeZ;
        return new Vec3(
            dx * rightX + dy * rightY + dz * rightZ,
            dx * upX + dy * upY + dz * upZ,
            dx * fwdX + dy * fwdY + dz * fwdZ
        );
    }

    private boolean projectCam(Vec3 c, float w, float h, float[] outXY) {
        if (c.z <= 0.25f) return false;
        outXY[0] = w * 0.5f + (c.x / c.z) * focalLen;
        outXY[1] = h * 0.5f - (c.y / c.z) * focalLen;
        return true;
    }

    private boolean projectWorld(Vec3 world, float w, float h, float[] outXY) {
        return projectCam(toCamera(world), w, h, outXY);
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        updateCameraBasis(w, h);
        drawSky(canvas, w, h);

        frameFaces.clear();
        if (scene != null) {
            for (SceneModel.Node part : scene.all3DParts()) {
                if (part.transparency >= 0.99f) continue;
                emitPartFaces(part, w, h);
            }
        }
        if (playMode) {
            emitPlayerCharacter(w, h);
        }

        Collections.sort(frameFaces, new Comparator<Face>() {
            @Override public int compare(Face a, Face b) {
                return Float.compare(b.depth, a.depth);
            }
        });
        lastSortedFaces.clear();
        lastSortedFaces.addAll(frameFaces);

        for (int i = 0; i < frameFaces.size(); ++i) {
            Face f = frameFaces.get(i);
            polyPath.reset();
            polyPath.moveTo(f.sx[0], f.sy[0]);
            for (int k = 1; k < f.count; ++k) polyPath.lineTo(f.sx[k], f.sy[k]);
            polyPath.close();
            fillPaint.setColor(f.fillColor);
            canvas.drawPath(polyPath, fillPaint);

            if (f.strokeWidth > 0f) {
                strokePaint.setColor(f.strokeColor);
                strokePaint.setStrokeWidth(f.strokeWidth);
                canvas.drawPath(polyPath, strokePaint);
            }
            if (f.isBaseplateTop && showGrid) {
                drawBaseplateGrid(canvas, f.owner, w, h);
            }
            if (f.isSpawnTop) {
                drawSpawnDecal(canvas, f.owner, w, h);
            }
        }

        if (!playMode && selected != null && selected.is3DPart()) {
            drawSelectionBox(canvas, selected, w, h);
            drawGizmos(canvas, selected, w, h);
        } else {
            activeHandles.clear();
        }

        drawStarterGuiOverlays(canvas, w, h);

        if (playMode) {
            drawPlayControls(canvas, w, h);
        } else {
            // Subtle top-right view indicator like Roblox Studio
            textPaint.setTypeface(Typeface.MONOSPACE);
            textPaint.setTextSize(11f * getResources().getDisplayMetrics().density);
            textPaint.setTextAlign(Paint.Align.RIGHT);
            textPaint.setColor(0xCCFFFFFF);
            String modeLabel = toolMode.name() + (selected != null ? " • " + selected.name : "");
            canvas.drawText(modeLabel, w - 12f, 22f * getResources().getDisplayMetrics().density, textPaint);
            textPaint.setTextAlign(Paint.Align.LEFT);
        }
    }

    private void drawSky(Canvas canvas, int w, int h) {
        SceneModel.Node light = scene != null ? scene.lighting() : null;
        float ct = light != null ? light.clockTime : 14f;
        boolean night = (ct < 6f || ct > 19f);
        boolean sunset = (!night && (ct < 7.5f || ct > 17.2f));
        int topCol = night ? 0xFF0A1124 : (sunset ? 0xFF284B7E : 0xFF1F6EC4);
        int botCol = night ? 0xFF1D2945 : (sunset ? 0xFFE59872 : 0xFF8CC5F2);
        fillPaint.setShader(new LinearGradient(0, 0, 0, h * 0.72f, topCol, botCol, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fillPaint);
        fillPaint.setShader(null);

        // Sun / Moon disc in sky
        Vec3 sunDir = new Vec3(0.55f, 0.72f, -0.45f);
        Vec3 sunWorld = new Vec3(eyeX + sunDir.x * 200f, eyeY + sunDir.y * 200f, eyeZ + sunDir.z * 200f);
        float[] sxy = new float[2];
        if (projectWorld(sunWorld, w, h, sxy)) {
            float sunR = Math.min(w, h) * 0.055f;
            fillPaint.setColor(night ? 0x66DDE8FF : 0x55FFF7CC);
            canvas.drawCircle(sxy[0], sxy[1], sunR * 1.55f, fillPaint);
            fillPaint.setColor(night ? 0xFFE5EEFF : 0xFFFFFBE0);
            canvas.drawCircle(sxy[0], sxy[1], sunR, fillPaint);
        }
    }

    private void emitPartFaces(SceneModel.Node part, int w, int h) {
        float hx = part.sizeX * 0.5f;
        float hy = part.sizeY * 0.5f;
        float hz = part.sizeZ * 0.5f;

        if ("WedgePart".equals(part.className)) {
            Vec3[] verts = {
                transformVert(part, -hx, -hy, -hz), // 0: back-bot-left
                transformVert(part,  hx, -hy, -hz), // 1: back-bot-right
                transformVert(part,  hx, -hy,  hz), // 2: front-bot-right
                transformVert(part, -hx, -hy,  hz), // 3: front-bot-left
                transformVert(part, -hx,  hy,  hz), // 4: front-top-left
                transformVert(part,  hx,  hy,  hz)  // 5: front-top-right
            };
            int[][] wedgeFaces = {
                {0, 1, 2, 3}, // bottom
                {3, 2, 5, 4}, // back/tall vertical
                {0, 3, 4},    // left triangle
                {1, 5, 2},    // right triangle
                {0, 4, 5, 1}  // sloped ramp
            };
            for (int[] idx : wedgeFaces) {
                emitPolygonFace(part, verts, idx, w, h, false, false);
            }
            return;
        }

        if ("Sphere".equals(part.className)) {
            int stacks = 6, slices = 10;
            for (int i = 0; i < stacks; ++i) {
                double p0 = Math.PI * (-0.5 + (double) i / stacks);
                double p1 = Math.PI * (-0.5 + (double) (i + 1) / stacks);
                for (int j = 0; j < slices; ++j) {
                    double t0 = 2.0 * Math.PI * j / slices;
                    double t1 = 2.0 * Math.PI * (j + 1) / slices;
                    Vec3 v0 = spherePt(part, hx, hy, hz, p0, t0);
                    Vec3 v1 = spherePt(part, hx, hy, hz, p0, t1);
                    Vec3 v2 = spherePt(part, hx, hy, hz, p1, t1);
                    Vec3 v3 = spherePt(part, hx, hy, hz, p1, t0);
                    Vec3[] quad = {v0, v1, v2, v3};
                    int[] idx = {0, 1, 2, 3};
                    emitPolygonFace(part, quad, idx, w, h, false, false);
                }
            }
            return;
        }

        if ("Cylinder".equals(part.className)) {
            int segs = 12;
            Vec3[] topPts = new Vec3[segs];
            Vec3[] botPts = new Vec3[segs];
            for (int i = 0; i < segs; ++i) {
                double a = 2.0 * Math.PI * i / segs;
                float lx = (float) Math.cos(a) * hx;
                float lz = (float) Math.sin(a) * hz;
                topPts[i] = transformVert(part, lx, hy, lz);
                botPts[i] = transformVert(part, lx, -hy, lz);
            }
            for (int i = 0; i < segs; ++i) {
                int ni = (i + 1) % segs;
                Vec3[] side = {botPts[i], botPts[ni], topPts[ni], topPts[i]};
                emitPolygonFace(part, side, new int[]{0, 1, 2, 3}, w, h, false, false);
            }
            int[] topIdx = new int[segs];
            for (int i = 0; i < segs; ++i) topIdx[i] = segs - 1 - i;
            emitPolygonFace(part, topPts, topIdx, w, h, false, false);
            return;
        }

        // Standard 6-face Box (Part / SpawnLocation)
        Vec3[] v = {
            transformVert(part, -hx, -hy, -hz), // 0
            transformVert(part,  hx, -hy, -hz), // 1
            transformVert(part,  hx,  hy, -hz), // 2
            transformVert(part, -hx,  hy, -hz), // 3
            transformVert(part, -hx, -hy,  hz), // 4
            transformVert(part,  hx, -hy,  hz), // 5
            transformVert(part,  hx,  hy,  hz), // 6
            transformVert(part, -hx,  hy,  hz)  // 7
        };
        int[][] boxFaces = {
            {3, 2, 6, 7}, // +Y top
            {4, 5, 1, 0}, // -Y bottom
            {4, 7, 6, 5}, // +Z front
            {1, 2, 3, 0}, // -Z back
            {5, 6, 2, 1}, // +X right
            {0, 3, 7, 4}  // -X left
        };
        boolean isBase = "Baseplate".equals(part.name);
        boolean isSpawn = "SpawnLocation".equals(part.className);
        for (int fi = 0; fi < boxFaces.length; ++fi) {
            emitPolygonFace(part, v, boxFaces[fi], w, h, isBase && fi == 0, isSpawn && fi == 0);
        }
    }

    private Vec3 spherePt(SceneModel.Node part, float hx, float hy, float hz, double phi, double theta) {
        float lx = (float) (Math.cos(phi) * Math.cos(theta)) * hx;
        float ly = (float) Math.sin(phi) * hy;
        float lz = (float) (Math.cos(phi) * Math.sin(theta)) * hz;
        return transformVert(part, lx, ly, lz);
    }

    private void emitPolygonFace(SceneModel.Node part, Vec3[] verts, int[] idx, int w, int h, boolean baseTop, boolean spawnTop) {
        Vec3 p0 = verts[idx[0]], p1 = verts[idx[1]], p2 = verts[idx[2]];
        float ux = p1.x - p0.x, uy = p1.y - p0.y, uz = p1.z - p0.z;
        float vx = p2.x - p0.x, vy = p2.y - p0.y, vz = p2.z - p0.z;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        float nl = (float) Math.sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-5f) return;
        nx /= nl; ny /= nl; nz /= nl;

        // Back-face cull in world space against eye -> p0
        float viewX = eyeX - p0.x, viewY = eyeY - p0.y, viewZ = eyeZ - p0.z;
        if (nx * viewX + ny * viewY + nz * viewZ <= 0f) return;

        float[] sx = new float[idx.length];
        float[] sy = new float[idx.length];
        float[] xy = new float[2];
        float avgDepth = 0f;
        for (int i = 0; i < idx.length; ++i) {
            Vec3 c = toCamera(verts[idx[i]]);
            if (!projectCam(c, w, h, xy)) return;
            sx[i] = xy[0];
            sy[i] = xy[1];
            avgDepth += c.z;
        }
        avgDepth /= idx.length;
        if (Float.isNaN(avgDepth) || Float.isInfinite(avgDepth)) return;

        // Sun lighting
        float sunDot = nx * 0.45f + ny * 0.80f - nz * 0.38f;
        float light = "Neon".equals(part.material) ? 1.15f : Math.max(0.42f, Math.min(1.08f, 0.62f + sunDot * 0.42f));
        int baseCol = part.color;
        int alpha = Math.max(25, Math.min(255, Math.round((1f - part.transparency) * 255f)));
        int r = Math.min(255, Math.max(0, Math.round(Color.red(baseCol) * light)));
        int g = Math.min(255, Math.max(0, Math.round(Color.green(baseCol) * light)));
        int b = Math.min(255, Math.max(0, Math.round(Color.blue(baseCol) * light)));

        Face f = new Face();
        f.sx = sx;
        f.sy = sy;
        f.count = idx.length;
        f.depth = avgDepth;
        f.fillColor = Color.argb(alpha, r, g, b);
        f.strokeColor = Color.argb(Math.min(180, alpha), Math.max(0, r - 35), Math.max(0, g - 35), Math.max(0, b - 35));
        f.strokeWidth = "SmoothPlastic".equals(part.material) ? 0.6f : 1.0f;
        f.isBaseplateTop = baseTop;
        f.isSpawnTop = spawnTop;
        f.owner = part;
        frameFaces.add(f);
    }

    private Vec3 transformVert(SceneModel.Node part, float lx, float ly, float lz) {
        float rx = (float) Math.toRadians(part.rotX);
        float ry = (float) Math.toRadians(part.rotY);
        float rz = (float) Math.toRadians(part.rotZ);
        // Rotate X
        if (rx != 0f) {
            float c = (float) Math.cos(rx), s = (float) Math.sin(rx);
            float ny = ly * c - lz * s;
            float nz = ly * s + lz * c;
            ly = ny; lz = nz;
        }
        // Rotate Y
        if (ry != 0f) {
            float c = (float) Math.cos(ry), s = (float) Math.sin(ry);
            float nx = lx * c + lz * s;
            float nz = -lx * s + lz * c;
            lx = nx; lz = nz;
        }
        // Rotate Z
        if (rz != 0f) {
            float c = (float) Math.cos(rz), s = (float) Math.sin(rz);
            float nx = lx * c - ly * s;
            float ny = lx * s + ly * c;
            lx = nx; ly = ny;
        }
        return new Vec3(part.posX + lx, part.posY + ly, part.posZ + lz);
    }

    private void drawBaseplateGrid(Canvas canvas, SceneModel.Node base, int w, int h) {
        float y = base.posY + base.sizeY * 0.5f + 0.01f;
        float hx = Math.min(42f, base.sizeX * 0.5f);
        float hz = Math.min(42f, base.sizeZ * 0.5f);
        float step = 4f;
        strokePaint.setColor(0x44222831);
        strokePaint.setStrokeWidth(1.1f);
        float[] a = new float[2], b = new float[2];
        for (float gx = -hx; gx <= hx; gx += step) {
            if (projectWorld(new Vec3(base.posX + gx, y, base.posZ - hz), w, h, a)
                && projectWorld(new Vec3(base.posX + gx, y, base.posZ + hz), w, h, b)) {
                canvas.drawLine(a[0], a[1], b[0], b[1], strokePaint);
            }
        }
        for (float gz = -hz; gz <= hz; gz += step) {
            if (projectWorld(new Vec3(base.posX - hx, y, base.posZ + gz), w, h, a)
                && projectWorld(new Vec3(base.posX + hx, y, base.posZ + gz), w, h, b)) {
                canvas.drawLine(a[0], a[1], b[0], b[1], strokePaint);
            }
        }
    }

    private void drawSpawnDecal(Canvas canvas, SceneModel.Node spawn, int w, int h) {
        // 8-point Roblox spawn star decal projected on top face
        float y = spawn.posY + spawn.sizeY * 0.5f + 0.02f;
        float rOuter = Math.min(spawn.sizeX, spawn.sizeZ) * 0.38f;
        float rInner = rOuter * 0.34f;
        int pts = 16;
        polyPath.reset();
        float[] xy = new float[2];
        for (int i = 0; i < pts; ++i) {
            double ang = i * Math.PI / 8.0;
            float r = (i % 2 == 0) ? rOuter : rInner;
            Vec3 p = transformVert(spawn, (float) Math.cos(ang) * r, spawn.sizeY * 0.5f + 0.02f, (float) Math.sin(ang) * r);
            if (!projectWorld(p, w, h, xy)) return;
            if (i == 0) polyPath.moveTo(xy[0], xy[1]);
            else polyPath.lineTo(xy[0], xy[1]);
        }
        polyPath.close();
        fillPaint.setColor(0xFF1F2937);
        canvas.drawPath(polyPath, fillPaint);

        // Inner circle cutout
        polyPath.reset();
        float rHole = rInner * 0.65f;
        for (int i = 0; i < 12; ++i) {
            double ang = i * 2.0 * Math.PI / 12.0;
            Vec3 p = transformVert(spawn, (float) Math.cos(ang) * rHole, spawn.sizeY * 0.5f + 0.03f, (float) Math.sin(ang) * rHole);
            if (!projectWorld(p, w, h, xy)) return;
            if (i == 0) polyPath.moveTo(xy[0], xy[1]);
            else polyPath.lineTo(xy[0], xy[1]);
        }
        polyPath.close();
        fillPaint.setColor(spawn.color);
        canvas.drawPath(polyPath, fillPaint);
    }

    private void emitPlayerCharacter(int w, int h) {
        float swing = (float) Math.sin(walkCycle) * 28f;
        emitBoxMini(playerX, playerY + 1.85f, playerZ, 1.1f, 1.1f, 1.1f, 0f, playerYaw, 0xFFFACC15, w, h); // Head
        emitBoxMini(playerX, playerY + 0.4f, playerZ, 1.8f, 1.8f, 0.9f, 0f, playerYaw, 0xFF2563EB, w, h); // Torso
        emitBoxMini(playerX - 0.5f, playerY - 1.4f, playerZ, 0.8f, 1.8f, 0.8f, swing, playerYaw, 0xFF16A34A, w, h); // L Leg
        emitBoxMini(playerX + 0.5f, playerY - 1.4f, playerZ, 0.8f, 1.8f, 0.8f, -swing, playerYaw, 0xFF16A34A, w, h); // R Leg
    }

    private void emitBoxMini(float cx, float cy, float cz, float sx, float sy, float sz, float rotXDeg, float yawRad, int col, int w, int h) {
        SceneModel.Node tmp = new SceneModel.Node("player", "Player", "Part");
        tmp.posX = cx; tmp.posY = cy; tmp.posZ = cz;
        tmp.sizeX = sx; tmp.sizeY = sy; tmp.sizeZ = sz;
        tmp.rotX = rotXDeg; tmp.rotY = (float) Math.toDegrees(yawRad);
        tmp.color = col;
        emitPartFaces(tmp, w, h);
    }

    private void drawSelectionBox(Canvas canvas, SceneModel.Node part, int w, int h) {
        float hx = part.sizeX * 0.5f + 0.08f;
        float hy = part.sizeY * 0.5f + 0.08f;
        float hz = part.sizeZ * 0.5f + 0.08f;
        Vec3[] c = {
            transformVert(part, -hx, -hy, -hz), transformVert(part,  hx, -hy, -hz),
            transformVert(part,  hx,  hy, -hz), transformVert(part, -hx,  hy, -hz),
            transformVert(part, -hx, -hy,  hz), transformVert(part,  hx, -hy,  hz),
            transformVert(part,  hx,  hy,  hz), transformVert(part, -hx,  hy,  hz)
        };
        float[][] p = new float[8][2];
        for (int i = 0; i < 8; ++i) if (!projectWorld(c[i], w, h, p[i])) return;
        int[][] edges = {
            {0,1},{1,2},{2,3},{3,0},
            {4,5},{5,6},{6,7},{7,4},
            {0,4},{1,5},{2,6},{3,7}
        };
        strokePaint.setColor(0xFF00A2FF);
        strokePaint.setStrokeWidth(2.6f);
        for (int[] e : edges) {
            canvas.drawLine(p[e[0]][0], p[e[0]][1], p[e[1]][0], p[e[1]][1], strokePaint);
        }
    }

    private void drawGizmos(Canvas canvas, SceneModel.Node part, int w, int h) {
        activeHandles.clear();
        if (toolMode == ToolMode.SELECT) return;
        float[] center = new float[2];
        if (!projectWorld(new Vec3(part.posX, part.posY, part.posZ), w, h, center)) return;
        float len = Math.max(3.5f, Math.max(part.sizeX, Math.max(part.sizeY, part.sizeZ)) * 0.7f);
        Vec3[] ends = {
            new Vec3(part.posX + len, part.posY, part.posZ),
            new Vec3(part.posX, part.posY + len, part.posZ),
            new Vec3(part.posX, part.posY, part.posZ + len)
        };
        int[] cols = {0xFFEF4444, 0xFF22C55E, 0xFF3B82F6};
        for (int axis = 0; axis < 3; ++axis) {
            float[] end = new float[2];
            if (!projectWorld(ends[axis], w, h, end)) continue;
            strokePaint.setColor(cols[axis]);
            strokePaint.setStrokeWidth(5.5f);
            canvas.drawLine(center[0], center[1], end[0], end[1], strokePaint);
            fillPaint.setColor(cols[axis]);
            if (toolMode == ToolMode.SCALE) {
                canvas.drawRect(end[0] - 11f, end[1] - 11f, end[0] + 11f, end[1] + 11f, fillPaint);
            } else {
                canvas.drawCircle(end[0], end[1], 12f, fillPaint);
            }
            GizmoHandle gh = new GizmoHandle();
            gh.axis = axis;
            gh.x1 = center[0]; gh.y1 = center[1];
            gh.x2 = end[0]; gh.y2 = end[1];
            gh.color = cols[axis];
            activeHandles.add(gh);
        }
    }

    private void drawStarterGuiOverlays(Canvas canvas, int w, int h) {
        if (scene == null) return;
        float density = getResources().getDisplayMetrics().density;
        for (SceneModel.Node ui : scene.allUiNodes()) {
            if ("Frame".equals(ui.className)) {
                fillPaint.setColor(ui.color);
                canvas.drawRoundRect(new RectF(ui.posX * density, ui.posY * density,
                    (ui.posX + ui.sizeX * 20f) * density, (ui.posY + ui.sizeY * 20f) * density), 8f, 8f, fillPaint);
            } else if ("TextLabel".equals(ui.className)) {
                textPaint.setTypeface(resolveFont(ui.fontName));
                textPaint.setTextSize(Math.max(12f, ui.textSize) * density);
                textPaint.setColor(ui.color);
                textPaint.setTextAlign(Paint.Align.LEFT);
                canvas.drawText(ui.text, ui.posX * density, (ui.posY + ui.textSize) * density, textPaint);
            }
        }
    }

    private Typeface resolveFont(String name) {
        if (name == null || name.isEmpty() || "default".equals(name)) return defaultTypeface;
        if ("sans".equals(name)) return Typeface.SANS_SERIF;
        if ("serif".equals(name)) return Typeface.SERIF;
        if ("mono".equals(name)) return Typeface.MONOSPACE;
        if ("bold".equals(name)) return Typeface.DEFAULT_BOLD;
        if ("italic".equals(name)) return Typeface.create(defaultTypeface, Typeface.ITALIC);
        if ("bold_italic".equals(name)) return Typeface.create(defaultTypeface, Typeface.BOLD_ITALIC);
        if (fontCache.containsKey(name)) return fontCache.get(name);
        if (projectDir != null) {
            try {
                File f = new File(projectDir, name);
                if (f.isFile()) {
                    Typeface tf = Typeface.createFromFile(f);
                    if (tf != null) { fontCache.put(name, tf); return tf; }
                }
            } catch (Exception ignored) { }
        }
        return defaultTypeface;
    }

    private void drawPlayControls(Canvas canvas, int w, int h) {
        float density = getResources().getDisplayMetrics().density;
        joyRadius = 48f * density;
        joyCenterX = 72f * density;
        joyCenterY = h - 72f * density;
        jumpBtnR = 32f * density;
        jumpBtnX = w - 64f * density;
        jumpBtnY = h - 68f * density;

        fillPaint.setColor(0x44FFFFFF);
        canvas.drawCircle(joyCenterX, joyCenterY, joyRadius, fillPaint);
        strokePaint.setColor(0x99FFFFFF);
        strokePaint.setStrokeWidth(2f);
        canvas.drawCircle(joyCenterX, joyCenterY, joyRadius, strokePaint);

        fillPaint.setColor(0xCCFFFFFF);
        canvas.drawCircle(joyCenterX + joyDx * joyRadius * 0.65f, joyCenterY + joyDy * joyRadius * 0.65f, joyRadius * 0.36f, fillPaint);

        fillPaint.setColor(0x553B82F6);
        canvas.drawCircle(jumpBtnX, jumpBtnY, jumpBtnR, fillPaint);
        strokePaint.setColor(0xCCFFFFFF);
        canvas.drawCircle(jumpBtnX, jumpBtnY, jumpBtnR, strokePaint);
        textPaint.setTypeface(Typeface.DEFAULT_BOLD);
        textPaint.setTextSize(13f * density);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setColor(Color.WHITE);
        canvas.drawText("JUMP", jumpBtnX, jumpBtnY + 5f * density, textPaint);
        textPaint.setTextAlign(Paint.Align.LEFT);
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int idx = event.getActionIndex();
        float x = event.getX(idx);
        float y = event.getY(idx);

        if (playMode) {
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                if (Math.hypot(x - jumpBtnX, y - jumpBtnY) <= jumpBtnR * 1.4f) {
                    if (playerGrounded) { playerVy = 17.5f; playerGrounded = false; }
                    return true;
                }
                if (Math.hypot(x - joyCenterX, y - joyCenterY) <= joyRadius * 1.6f) {
                    joyPointerId = event.getPointerId(idx);
                    updateJoystick(x, y);
                    return true;
                }
                lastTouchX = x; lastTouchY = y;
                dragMode = 1;
            } else if (action == MotionEvent.ACTION_MOVE) {
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (event.getPointerId(i) == joyPointerId) {
                        updateJoystick(event.getX(i), event.getY(i));
                    } else if (dragMode == 1) {
                        float dx = event.getX(i) - lastTouchX;
                        float dy = event.getY(i) - lastTouchY;
                        camYaw -= dx * 0.008f;
                        camPitch = Math.max(-0.4f, Math.min(1.2f, camPitch + dy * 0.007f));
                        lastTouchX = event.getX(i);
                        lastTouchY = event.getY(i);
                    }
                }
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_CANCEL) {
                if (event.getPointerId(idx) == joyPointerId) {
                    joyPointerId = -1;
                    joyDx = 0f; joyDy = 0f;
                }
                if (event.getPointerCount() <= 1) dragMode = 0;
            }
            invalidate();
            return true;
        }

        // Studio Editor interactions
        if (action == MotionEvent.ACTION_DOWN) {
            downTouchX = lastTouchX = x;
            downTouchY = lastTouchY = y;
            touchMoved = false;
            lastPinchDist = -1f;
            dragAxis = hitTestGizmo(x, y);
            dragMode = (dragAxis >= 0) ? 3 : 1;
            return true;
        }
        if (action == MotionEvent.ACTION_POINTER_DOWN && event.getPointerCount() >= 2) {
            dragMode = 2;
            lastPinchDist = pinchDistance(event);
            lastTouchX = (event.getX(0) + event.getX(1)) * 0.5f;
            lastTouchY = (event.getY(0) + event.getY(1)) * 0.5f;
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            if (event.getPointerCount() >= 2 && dragMode == 2) {
                float d = pinchDistance(event);
                if (lastPinchDist > 0f && d > 10f) {
                    camDist = Math.max(5f, Math.min(120f, camDist * (lastPinchDist / d)));
                }
                lastPinchDist = d;
                float mx = (event.getX(0) + event.getX(1)) * 0.5f;
                float my = (event.getY(0) + event.getY(1)) * 0.5f;
                float panScale = camDist * 0.002f;
                camTargetX -= (mx - lastTouchX) * rightX * panScale - (my - lastTouchY) * upX * panScale;
                camTargetY -= (mx - lastTouchX) * rightY * panScale - (my - lastTouchY) * upY * panScale;
                camTargetZ -= (mx - lastTouchX) * rightZ * panScale - (my - lastTouchY) * upZ * panScale;
                lastTouchX = mx; lastTouchY = my;
                invalidate();
                return true;
            }
            float dx = x - lastTouchX;
            float dy = y - lastTouchY;
            if (Math.hypot(x - downTouchX, y - downTouchY) > 10f) touchMoved = true;
            if (dragMode == 3 && selected != null && !selected.locked) {
                applyGizmoDrag(dx, dy);
            } else if (dragMode == 1) {
                camYaw -= dx * 0.008f;
                camPitch = Math.max(-0.5f, Math.min(1.45f, camPitch + dy * 0.007f));
            }
            lastTouchX = x; lastTouchY = y;
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_UP) {
            if (dragMode == 3 && touchMoved && listener != null) {
                listener.onSceneChanged();
            } else if (!touchMoved && dragMode != 2) {
                SceneModel.Node hit = hitTestPart(x, y);
                selected = hit;
                if (listener != null) listener.onNodeSelected(hit);
            }
            dragMode = 0;
            dragAxis = -1;
            invalidate();
            return true;
        }
        return super.onTouchEvent(event);
    }

    private void updateJoystick(float x, float y) {
        float dx = (x - joyCenterX) / Math.max(1f, joyRadius);
        float dy = (y - joyCenterY) / Math.max(1f, joyRadius);
        float mag = (float) Math.hypot(dx, dy);
        if (mag > 1f) { dx /= mag; dy /= mag; }
        joyDx = dx; joyDy = dy;
    }

    private float pinchDistance(MotionEvent e) {
        float dx = e.getX(0) - e.getX(1);
        float dy = e.getY(0) - e.getY(1);
        return (float) Math.hypot(dx, dy);
    }

    private int hitTestGizmo(float px, float py) {
        for (int i = 0; i < activeHandles.size(); ++i) {
            GizmoHandle g = activeHandles.get(i);
            if (Math.hypot(px - g.x2, py - g.y2) <= 34f) return g.axis;
        }
        return -1;
    }

    private void applyGizmoDrag(float dx, float dy) {
        if (selected == null || dragAxis < 0 || dragAxis > 2) return;
        GizmoHandle matched = null;
        for (GizmoHandle g : activeHandles) if (g.axis == dragAxis) { matched = g; break; }
        if (matched == null) return;
        float vx = matched.x2 - matched.x1;
        float vy = matched.y2 - matched.y1;
        float vlen = (float) Math.hypot(vx, vy);
        if (vlen < 1f) return;
        float proj = (dx * vx + dy * vy) / vlen;
        float worldDelta = proj * camDist * 0.0022f;

        if (toolMode == ToolMode.MOVE) {
            if (dragAxis == 0) selected.posX += worldDelta;
            else if (dragAxis == 1) selected.posY += worldDelta;
            else selected.posZ += worldDelta;
            if (collisionsEnabled && snapGrid > 0.05f) {
                selected.posX = Math.round(selected.posX / snapGrid) * snapGrid;
                selected.posY = Math.round(selected.posY / snapGrid) * snapGrid;
                selected.posZ = Math.round(selected.posZ / snapGrid) * snapGrid;
            }
        } else if (toolMode == ToolMode.SCALE) {
            if (dragAxis == 0) selected.sizeX = Math.max(0.4f, selected.sizeX + worldDelta);
            else if (dragAxis == 1) selected.sizeY = Math.max(0.4f, selected.sizeY + worldDelta);
            else selected.sizeZ = Math.max(0.4f, selected.sizeZ + worldDelta);
        } else if (toolMode == ToolMode.ROTATE) {
            float deg = proj * 0.6f;
            if (dragAxis == 0) selected.rotX = (selected.rotX + deg) % 360f;
            else if (dragAxis == 1) selected.rotY = (selected.rotY + deg) % 360f;
            else selected.rotZ = (selected.rotZ + deg) % 360f;
        }
    }

    private SceneModel.Node hitTestPart(float px, float py) {
        // Iterate front-to-back (lastSortedFaces is back-to-front)
        for (int i = lastSortedFaces.size() - 1; i >= 0; --i) {
            Face f = lastSortedFaces.get(i);
            if (f.owner == null || !f.owner.is3DPart()) continue;
            if (pointInPoly(px, py, f.sx, f.sy, f.count)) {
                return f.owner;
            }
        }
        return null;
    }

    private static boolean pointInPoly(float x, float y, float[] px, float[] py, int n) {
        boolean inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if (((py[i] > y) != (py[j] > y)) &&
                (x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i] + 1e-6f) + px[i])) {
                inside = !inside;
            }
        }
        return inside;
    }
}
