package com.luac.studio;

import android.graphics.Color;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

final class SceneModel {
    static final String[] MATERIALS = {
        "Plastic", "SmoothPlastic", "Metal", "Neon", "Wood", "Slate", "Glass", "Grass"
    };
    static final String[] SHAPES = {"Block", "Wedge", "Sphere", "Cylinder"};
    static final String[] FONTS = {"sans", "bold", "mono", "serif", "italic", "bold_italic"};

    static final class Node {
        String id;
        String name;
        String className; // Workspace, Part, WedgePart, Sphere, Cylinder, SpawnLocation, Folder, Script, LocalScript, PointLight, ScreenGui, TextLabel, Frame, Lighting, Service, Camera
        Node parent;
        final ArrayList<Node> children = new ArrayList<>();
        boolean expanded = true;

        float posX, posY, posZ;
        float sizeX = 4f, sizeY = 2f, sizeZ = 4f;
        float rotX, rotY, rotZ;
        int color = 0xFFA3A2A5;
        String material = "Plastic";
        float transparency = 0f;
        boolean anchored = true;
        boolean canCollide = true;
        boolean castShadow = true;
        boolean locked = false;
        boolean disabled = false;

        // Script / UI / Lighting properties
        String scriptPath = "";
        String text = "FPS Arena";
        int textSize = 18;
        String fontName = "bold";
        float clockTime = 14f;
        float brightness = 2f;
        int skyTopColor = 0xFF2875C6;
        int skyHorizonColor = 0xFF8EC5F0;

        Node(String id, String name, String className) {
            this.id = id;
            this.name = name;
            this.className = className;
        }

        void add(Node child) {
            child.parent = this;
            children.add(child);
        }

        boolean is3DPart() {
            return "Part".equals(className) || "WedgePart".equals(className)
                || "Sphere".equals(className) || "Cylinder".equals(className)
                || "SpawnLocation".equals(className);
        }

        boolean isScript() {
            return "Script".equals(className) || "LocalScript".equals(className);
        }

        boolean isUi() {
            return "TextLabel".equals(className) || "Frame".equals(className);
        }

        String shape() {
            if ("WedgePart".equals(className)) return "Wedge";
            if ("Sphere".equals(className)) return "Sphere";
            if ("Cylinder".equals(className)) return "Cylinder";
            return "Block";
        }

        void setShape(String shape) {
            if ("Wedge".equals(shape)) className = "WedgePart";
            else if ("Sphere".equals(shape)) className = "Sphere";
            else if ("Cylinder".equals(shape)) className = "Cylinder";
            else if (!"SpawnLocation".equals(className)) className = "Part";
        }
    }

    final Node root = new Node("root", "DataModel", "DataModel");
    private int nextId = 1;

    private String genId() {
        return "n" + (nextId++);
    }

    Node findById(String id) {
        return findById(root, id);
    }

    private Node findById(Node node, String id) {
        if (node.id.equals(id)) return node;
        for (Node c : node.children) {
            Node found = findById(c, id);
            if (found != null) return found;
        }
        return null;
    }

    Node findService(String name) {
        for (Node c : root.children) {
            if (c.name.equals(name)) return c;
        }
        return null;
    }

    List<Node> all3DParts() {
        ArrayList<Node> out = new ArrayList<>();
        Node ws = findService("Workspace");
        if (ws != null) collectParts(ws, out);
        return out;
    }

    private void collectParts(Node node, List<Node> out) {
        if (node.is3DPart()) out.add(node);
        for (Node c : node.children) collectParts(c, out);
    }

    List<Node> allUiNodes() {
        ArrayList<Node> out = new ArrayList<>();
        Node sg = findService("StarterGui");
        if (sg != null) collectUi(sg, out);
        return out;
    }

    private void collectUi(Node node, List<Node> out) {
        if (node.isUi()) out.add(node);
        for (Node c : node.children) collectUi(c, out);
    }

    Node lighting() {
        return findService("Lighting");
    }

    Node spawnLocation() {
        for (Node n : all3DParts()) {
            if ("SpawnLocation".equals(n.className)) return n;
        }
        return null;
    }

    Node createPart(Node parent, String className, String name) {
        if (parent == null) parent = findService("Workspace");
        Node n = new Node(genId(), name, className);
        n.posX = 0f;
        n.posY = 3f;
        n.posZ = 0f;
        n.sizeX = 4f;
        n.sizeY = 2f;
        n.sizeZ = 4f;
        if ("Sphere".equals(className)) {
            n.sizeX = 3f; n.sizeY = 3f; n.sizeZ = 3f;
            n.color = 0xFF4ADE80;
        } else if ("WedgePart".equals(className)) {
            n.sizeX = 4f; n.sizeY = 3f; n.sizeZ = 6f;
            n.color = 0xFF94A3B8;
        } else if ("Cylinder".equals(className)) {
            n.sizeX = 3f; n.sizeY = 4f; n.sizeZ = 3f;
            n.color = 0xFF38BDF8;
        } else if ("SpawnLocation".equals(className)) {
            n.sizeX = 6f; n.sizeY = 0.6f; n.sizeZ = 6f;
            n.color = 0xFFE5E7EB;
        } else {
            n.color = 0xFF9CA3AF;
        }
        parent.add(n);
        parent.expanded = true;
        return n;
    }

    Node duplicate(Node src) {
        if (src == null || src.parent == null || src.parent == root) return null;
        Node copy = new Node(genId(), src.name + "Copy", src.className);
        copy.posX = src.posX + 2f;
        copy.posY = src.posY;
        copy.posZ = src.posZ + 2f;
        copy.sizeX = src.sizeX; copy.sizeY = src.sizeY; copy.sizeZ = src.sizeZ;
        copy.rotX = src.rotX; copy.rotY = src.rotY; copy.rotZ = src.rotZ;
        copy.color = src.color;
        copy.material = src.material;
        copy.transparency = src.transparency;
        copy.anchored = src.anchored;
        copy.canCollide = src.canCollide;
        copy.castShadow = src.castShadow;
        copy.locked = src.locked;
        copy.text = src.text;
        copy.textSize = src.textSize;
        copy.fontName = src.fontName;
        src.parent.add(copy);
        return copy;
    }

    boolean remove(Node node) {
        if (node == null || node.parent == null || node.parent == root) return false;
        return node.parent.children.remove(node);
    }

    static SceneModel createFpsArenaDefaults() {
        SceneModel m = new SceneModel();
        Node ws = new Node(m.genId(), "Workspace", "Workspace");
        ws.expanded = true;
        Node cam = new Node(m.genId(), "Camera", "Camera");
        ws.add(cam);

        Node base = new Node(m.genId(), "Baseplate", "Part");
        base.posX = 0f; base.posY = -1f; base.posZ = 0f;
        base.sizeX = 84f; base.sizeY = 2f; base.sizeZ = 84f;
        base.color = 0xFF6B7280; base.material = "Slate"; base.locked = true;
        ws.add(base);

        Node spawn = new Node(m.genId(), "SpawnLocation", "SpawnLocation");
        spawn.posX = 0f; spawn.posY = 0.35f; spawn.posZ = 8f;
        spawn.sizeX = 7f; spawn.sizeY = 0.7f; spawn.sizeZ = 7f;
        spawn.color = 0xFFE5E7EB; spawn.material = "SmoothPlastic";
        ws.add(spawn);

        Node deck = new Node(m.genId(), "UpperDeck", "Part");
        deck.posX = 0f; deck.posY = 2f; deck.posZ = -16f;
        deck.sizeX = 28f; deck.sizeY = 4f; deck.sizeZ = 10f;
        deck.color = 0xFFB0B7C3; deck.material = "SmoothPlastic";
        ws.add(deck);

        Node rampL = new Node(m.genId(), "RampLeft", "WedgePart");
        rampL.posX = -19f; rampL.posY = 2f; rampL.posZ = -16f;
        rampL.sizeX = 10f; rampL.sizeY = 4f; rampL.sizeZ = 10f;
        rampL.rotY = 90f;
        rampL.color = 0xFFA2ABB8; rampL.material = "SmoothPlastic";
        ws.add(rampL);

        Node rampR = new Node(m.genId(), "RampRight", "WedgePart");
        rampR.posX = 19f; rampR.posY = 2f; rampR.posZ = -16f;
        rampR.sizeX = 10f; rampR.sizeY = 4f; rampR.sizeZ = 10f;
        rampR.rotY = -90f;
        rampR.color = 0xFFA2ABB8; rampR.material = "SmoothPlastic";
        ws.add(rampR);

        Node backWall = new Node(m.genId(), "BackWall", "Part");
        backWall.posX = 0f; backWall.posY = 5.5f; backWall.posZ = -24f;
        backWall.sizeX = 58f; backWall.sizeY = 11f; backWall.sizeZ = 2f;
        backWall.color = 0xFF949CA8; backWall.material = "Concrete";
        ws.add(backWall);

        Node leftWall = new Node(m.genId(), "LeftWall", "Part");
        leftWall.posX = -28f; leftWall.posY = 5.5f; leftWall.posZ = -5f;
        leftWall.sizeX = 2f; leftWall.sizeY = 11f; leftWall.sizeZ = 38f;
        leftWall.color = 0xFF838C99; leftWall.material = "Slate";
        ws.add(leftWall);

        Node rightWall = new Node(m.genId(), "RightWall", "Part");
        rightWall.posX = 28f; rightWall.posY = 5.5f; rightWall.posZ = -5f;
        rightWall.sizeX = 2f; rightWall.sizeY = 11f; rightWall.sizeZ = 38f;
        rightWall.color = 0xFFA0A8B4; rightWall.material = "Slate";
        ws.add(rightWall);

        // Cover blocks (reddish cover boxes like screenshot)
        Node crate1 = new Node(m.genId(), "CoverBoxA", "Part");
        crate1.posX = -10f; crate1.posY = 1.5f; crate1.posZ = -7f;
        crate1.sizeX = 4f; crate1.sizeY = 3f; crate1.sizeZ = 4f;
        crate1.color = 0xFFB8635A; crate1.material = "Plastic";
        ws.add(crate1);

        Node crate2 = new Node(m.genId(), "CoverBoxB", "Part");
        crate2.posX = -9.5f; crate2.posY = 4f; crate2.posZ = -7f;
        crate2.sizeX = 2.6f; crate2.sizeY = 2f; crate2.sizeZ = 2.6f;
        crate2.color = 0xFFAE5A52; crate2.material = "Plastic";
        ws.add(crate2);

        Node crate3 = new Node(m.genId(), "CoverBoxC", "Part");
        crate3.posX = 11f; crate3.posY = 2.5f; crate3.posZ = -18f;
        crate3.sizeX = 3.5f; crate3.sizeY = 5f; crate3.sizeZ = 3.5f;
        crate3.color = 0xFFB8635A; crate3.material = "Plastic";
        ws.add(crate3);

        // Target dummies on the deck/arena
        float[] tx = {-12f, -3f, 4f, 9f};
        float[] ty = {5.5f, 2.2f, 2.2f, 5.5f};
        float[] tz = {-15f, -10f, -10f, -15f};
        for (int i = 0; i < tx.length; ++i) {
            Node t = new Node(m.genId(), "Target" + (i + 1), "Part");
            t.posX = tx[i]; t.posY = ty[i]; t.posZ = tz[i];
            t.sizeX = 1.6f; t.sizeY = 3.4f; t.sizeZ = 1.0f;
            t.color = 0xFFDC4C46; t.material = "SmoothPlastic";
            ws.add(t);
        }

        Node mainScript = new Node(m.genId(), "ArenaController", "Script");
        mainScript.scriptPath = "main.lua";
        ws.add(mainScript);

        m.root.add(ws);

        Node players = new Node(m.genId(), "Players", "Players");
        players.expanded = false;
        m.root.add(players);

        Node lighting = new Node(m.genId(), "Lighting", "Lighting");
        lighting.expanded = false;
        lighting.clockTime = 14f;
        lighting.brightness = 2.0f;
        m.root.add(lighting);

        m.root.add(new Node(m.genId(), "MaterialService", "MaterialService")).expanded = false;
        m.root.add(new Node(m.genId(), "ReplicatedFirst", "ReplicatedFirst")).expanded = false;
        m.root.add(new Node(m.genId(), "ReplicatedStorage", "ReplicatedStorage")).expanded = false;

        Node sss = new Node(m.genId(), "ServerScriptService", "ServerScriptService");
        sss.expanded = false;
        Node gameMgr = new Node(m.genId(), "GameScript", "Script");
        gameMgr.scriptPath = "scripts/game.lua";
        sss.add(gameMgr);
        m.root.add(sss);

        m.root.add(new Node(m.genId(), "ServerStorage", "ServerStorage")).expanded = false;

        Node starterGui = new Node(m.genId(), "StarterGui", "StarterGui");
        starterGui.expanded = false;
        Node screenGui = new Node(m.genId(), "ArenaHud", "ScreenGui");
        Node hudLabel = new Node(m.genId(), "TitleLabel", "TextLabel");
        hudLabel.text = "FPS Arena";
        hudLabel.textSize = 20;
        hudLabel.fontName = "bold";
        hudLabel.color = Color.WHITE;
        hudLabel.posX = 20f;
        hudLabel.posY = 18f;
        screenGui.add(hudLabel);
        starterGui.add(screenGui);
        m.root.add(starterGui);

        m.root.add(new Node(m.genId(), "StarterPack", "StarterPack")).expanded = false;
        m.root.add(new Node(m.genId(), "StarterPlayer", "StarterPlayer")).expanded = false;
        m.root.add(new Node(m.genId(), "Teams", "Teams")).expanded = false;
        m.root.add(new Node(m.genId(), "SoundService", "SoundService")).expanded = false;
        return m;
    }

    JSONObject toJson() throws Exception {
        JSONObject obj = new JSONObject();
        obj.put("nextId", nextId);
        obj.put("root", nodeToJson(root));
        return obj;
    }

    private JSONObject nodeToJson(Node n) throws Exception {
        JSONObject o = new JSONObject();
        o.put("id", n.id);
        o.put("name", n.name);
        o.put("className", n.className);
        o.put("expanded", n.expanded);
        o.put("posX", n.posX); o.put("posY", n.posY); o.put("posZ", n.posZ);
        o.put("sizeX", n.sizeX); o.put("sizeY", n.sizeY); o.put("sizeZ", n.sizeZ);
        o.put("rotX", n.rotX); o.put("rotY", n.rotY); o.put("rotZ", n.rotZ);
        o.put("color", n.color);
        o.put("material", n.material);
        o.put("transparency", n.transparency);
        o.put("anchored", n.anchored);
        o.put("canCollide", n.canCollide);
        o.put("castShadow", n.castShadow);
        o.put("locked", n.locked);
        o.put("disabled", n.disabled);
        o.put("scriptPath", n.scriptPath);
        o.put("text", n.text);
        o.put("textSize", n.textSize);
        o.put("fontName", n.fontName);
        o.put("clockTime", n.clockTime);
        o.put("brightness", n.brightness);
        JSONArray kids = new JSONArray();
        for (Node c : n.children) kids.put(nodeToJson(c));
        o.put("children", kids);
        return o;
    }

    static SceneModel fromJson(JSONObject obj) {
        SceneModel m = new SceneModel();
        m.nextId = obj.optInt("nextId", 100);
        JSONObject rootObj = obj.optJSONObject("root");
        if (rootObj != null) {
            m.root.children.clear();
            JSONArray kids = rootObj.optJSONArray("children");
            if (kids != null) {
                for (int i = 0; i < kids.length(); ++i) {
                    JSONObject c = kids.optJSONObject(i);
                    if (c != null) m.root.add(nodeFromJson(c));
                }
            }
        }
        return m;
    }

    private static Node nodeFromJson(JSONObject o) {
        Node n = new Node(o.optString("id", "n"), o.optString("name", "Part"), o.optString("className", "Part"));
        n.expanded = o.optBoolean("expanded", false);
        n.posX = (float) o.optDouble("posX", 0);
        n.posY = (float) o.optDouble("posY", 0);
        n.posZ = (float) o.optDouble("posZ", 0);
        n.sizeX = (float) o.optDouble("sizeX", 4);
        n.sizeY = (float) o.optDouble("sizeY", 2);
        n.sizeZ = (float) o.optDouble("sizeZ", 4);
        n.rotX = (float) o.optDouble("rotX", 0);
        n.rotY = (float) o.optDouble("rotY", 0);
        n.rotZ = (float) o.optDouble("rotZ", 0);
        n.color = o.optInt("color", 0xFFA3A2A5);
        n.material = o.optString("material", "Plastic");
        n.transparency = (float) o.optDouble("transparency", 0);
        n.anchored = o.optBoolean("anchored", true);
        n.canCollide = o.optBoolean("canCollide", true);
        n.castShadow = o.optBoolean("castShadow", true);
        n.locked = o.optBoolean("locked", false);
        n.disabled = o.optBoolean("disabled", false);
        n.scriptPath = o.optString("scriptPath", "");
        n.text = o.optString("text", "");
        n.textSize = o.optInt("textSize", 18);
        n.fontName = o.optString("fontName", "bold");
        n.clockTime = (float) o.optDouble("clockTime", 14);
        n.brightness = (float) o.optDouble("brightness", 2);
        JSONArray kids = o.optJSONArray("children");
        if (kids != null) {
            for (int i = 0; i < kids.length(); ++i) {
                JSONObject c = kids.optJSONObject(i);
                if (c != null) n.add(nodeFromJson(c));
            }
        }
        return n;
    }

    void save(ProjectStore store, File projectDir) throws Exception {
        store.write(new File(projectDir, "scene.json"), toJson().toString(2));
    }

    static SceneModel loadOrCreate(ProjectStore store, File projectDir) {
        File f = new File(projectDir, "scene.json");
        if (f.isFile()) {
            try {
                return fromJson(new JSONObject(store.read(f)));
            } catch (Exception ignored) { }
        }
        SceneModel m = createFpsArenaDefaults();
        try { m.save(store, projectDir); } catch (IOException ignored) { } catch (Exception ignored) { }
        return m;
    }

    JSONArray exportPartsJson() {
        JSONArray arr = new JSONArray();
        for (Node n : all3DParts()) {
            try {
                JSONObject p = new JSONObject();
                p.put("id", n.id);
                p.put("name", n.name);
                p.put("className", n.className);
                p.put("x", n.posX); p.put("y", n.posY); p.put("z", n.posZ);
                p.put("sx", n.sizeX); p.put("sy", n.sizeY); p.put("sz", n.sizeZ);
                p.put("rx", n.rotX); p.put("ry", n.rotY); p.put("rz", n.rotZ);
                p.put("r", Color.red(n.color) / 255.0);
                p.put("g", Color.green(n.color) / 255.0);
                p.put("b", Color.blue(n.color) / 255.0);
                p.put("material", n.material);
                p.put("transparency", n.transparency);
                p.put("anchored", n.anchored);
                p.put("canCollide", n.canCollide);
                arr.put(p);
            } catch (Exception ignored) { }
        }
        return arr;
    }
}
