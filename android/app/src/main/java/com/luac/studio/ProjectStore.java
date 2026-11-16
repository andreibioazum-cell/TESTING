package com.luac.studio;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

final class ProjectStore {
    final File root;
    private final Context context;

    ProjectStore(Context context) {
        this.context = context.getApplicationContext();
        root = new File(context.getFilesDir(), "projects");
        if (!root.exists()) root.mkdirs();
    }

    // Allows filesystem operations to be tested without an Android Context.
    ProjectStore(File root) {
        this.context = null;
        this.root = root;
        if (!root.exists()) root.mkdirs();
    }

    List<File> projects() {
        ArrayList<File> result = new ArrayList<>();
        File[] files = root.listFiles();
        if (files != null) for (File file : files) if (file.isDirectory()) result.add(file);
        result.sort((a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        return result;
    }

    File createSnakeProject(String requested) throws IOException {
        String name = safeName(requested, "Змейка");
        File project = unique(root, name);
        if (!new File(project, "scripts").mkdirs() || !new File(project, "assets").mkdirs()) throw new IOException("Cannot create project folders");
        write(new File(project, "main.lua"),
            "-- Змейка на LuaC (нативный машинный код + инкрементальный GC)\n" +
            "snake_mod = require('scripts.snake')\n" +
            "math.randomseed(os.time())\n\n" +
            "game = snake_mod.SnakeGame()\n" +
            "game:reset()\n\n" +
            "touch_active = false\n" +
            "touch_start_x = 0.0\n" +
            "touch_start_y = 0.0\n" +
            "swiped = false\n\n" +
            "func on_touch(id, x, y, down):\n" +
            "    if down:\n" +
            "        if not touch_active:\n" +
            "            touch_active = true\n" +
            "            touch_start_x = x\n" +
            "            touch_start_y = y\n" +
            "            swiped = false\n" +
            "            if not game.alive:\n" +
            "                game:reset()\n" +
            "                swiped = true\n" +
            "                return\n" +
            "        else:\n" +
            "            dx = x - touch_start_x\n" +
            "            dy = y - touch_start_y\n" +
            "            if not swiped and (math.abs(dx) > 24 or math.abs(dy) > 24):\n" +
            "                if math.abs(dx) > math.abs(dy):\n" +
            "                    if dx > 0:\n" +
            "                        game:set_dir(1, 0)\n" +
            "                    else:\n" +
            "                        game:set_dir(-1, 0)\n" +
            "                else:\n" +
            "                    if dy > 0:\n" +
            "                        game:set_dir(0, 1)\n" +
            "                    else:\n" +
            "                        game:set_dir(0, -1)\n" +
            "                swiped = true\n" +
            "    else:\n" +
            "        if touch_active and not swiped and game.alive:\n" +
            "            w = screen_width()\n" +
            "            h = screen_height()\n" +
            "            dx = x - w * 0.5\n" +
            "            dy = y - h * 0.5\n" +
            "            if math.abs(dx) > math.abs(dy):\n" +
            "                if dx > 0:\n" +
            "                    game:set_dir(1, 0)\n" +
            "                else:\n" +
            "                    game:set_dir(-1, 0)\n" +
            "            else:\n" +
            "                if dy > 0:\n" +
            "                    game:set_dir(0, 1)\n" +
            "                else:\n" +
            "                    game:set_dir(0, -1)\n" +
            "        touch_active = false\n\n" +
            "func update(float dt):\n" +
            "    game:step(dt)\n\n" +
            "func draw():\n" +
            "    w = screen_width()\n" +
            "    h = screen_height()\n" +
            "    pad = 24.0\n" +
            "    top_hud = 84.0\n" +
            "    avail_w = w - pad * 2.0\n" +
            "    avail_h = h - top_hud - pad\n" +
            "    cell = math.floor(math.min(avail_w / game.cols, avail_h / game.rows))\n" +
            "    if cell < 8:\n" +
            "        cell = 8\n" +
            "    board_w = cell * game.cols\n" +
            "    board_h = cell * game.rows\n" +
            "    ox = math.floor((w - board_w) * 0.5)\n" +
            "    oy = math.floor(top_hud + (avail_h - board_h) * 0.5)\n\n" +
            "    clear_screen(0.05, 0.07, 0.11, 1.0)\n" +
            "    draw_rect(ox, oy, board_w, board_h, 0.09, 0.12, 0.18, 1.0)\n\n" +
            "    if game.food:\n" +
            "        fx = ox + game.food.x * cell + cell * 0.5\n" +
            "        fy = oy + game.food.y * cell + cell * 0.5\n" +
            "        draw_circle(fx, fy, cell * 0.38, 0.96, 0.32, 0.36, 1.0)\n\n" +
            "    for i = 1, #game.body:\n" +
            "        seg = game.body[i]\n" +
            "        sx = ox + seg.x * cell + 2\n" +
            "        sy = oy + seg.y * cell + 2\n" +
            "        sz = cell - 4\n" +
            "        if i == 1:\n" +
            "            draw_rect(sx, sy, sz, sz, 0.25, 0.92, 0.62, 1.0)\n" +
            "        else:\n" +
            "            draw_rect(sx, sy, sz, sz, 0.16, 0.72, 0.48, 1.0)\n\n" +
            "    draw_text('Змейка  •  Счёт: ' .. tostring(game.score) .. '   Рекорд: ' .. tostring(game.best), ox, 24, 22, 1, 1, 1, 1)\n" +
            "    if not game.alive:\n" +
            "        draw_text('Игра окончена! Коснитесь экрана', ox, oy + board_h * 0.5 - 14, 22, 1.0, 0.45, 0.45, 1.0)\n");
        write(new File(project, "scripts/snake.lua"),
            "struct Cell:\n" +
            "    int x = 0\n" +
            "    int y = 0\n\n" +
            "class SnakeGame:\n" +
            "    int cols = 16\n" +
            "    int rows = 20\n" +
            "    int dx = 1\n" +
            "    int dy = 0\n" +
            "    int next_dx = 1\n" +
            "    int next_dy = 0\n" +
            "    int score = 0\n" +
            "    int best = 0\n" +
            "    bool alive = true\n" +
            "    float timer = 0.0\n" +
            "    float step_time = 0.14\n" +
            "    body = nil\n" +
            "    food = nil\n\n" +
            "    func reset():\n" +
            "        self.dx = 1\n" +
            "        self.dy = 0\n" +
            "        self.next_dx = 1\n" +
            "        self.next_dy = 0\n" +
            "        self.score = 0\n" +
            "        self.alive = true\n" +
            "        self.timer = 0.0\n" +
            "        self.step_time = 0.14\n" +
            "        cx = math.floor(self.cols / 2)\n" +
            "        cy = math.floor(self.rows / 2)\n" +
            "        self.body = {\n" +
            "            Cell({x = cx, y = cy}),\n" +
            "            Cell({x = cx - 1, y = cy}),\n" +
            "            Cell({x = cx - 2, y = cy})\n" +
            "        }\n" +
            "        self:spawn_food()\n\n" +
            "    func spawn_food():\n" +
            "        for attempt = 1, 200:\n" +
            "            fx = math.random(0, self.cols - 1)\n" +
            "            fy = math.random(0, self.rows - 1)\n" +
            "            free = true\n" +
            "            for i = 1, #self.body:\n" +
            "                if self.body[i].x == fx and self.body[i].y == fy:\n" +
            "                    free = false\n" +
            "                    break\n" +
            "            if free:\n" +
            "                self.food = Cell({x = fx, y = fy})\n" +
            "                return\n" +
            "        self.food = Cell({x = 0, y = 0})\n\n" +
            "    func set_dir(int ndx, int ndy):\n" +
            "        if ndx == -self.dx and ndy == -self.dy:\n" +
            "            return\n" +
            "        self.next_dx = ndx\n" +
            "        self.next_dy = ndy\n\n" +
            "    func step(float dt):\n" +
            "        if not self.alive:\n" +
            "            return\n" +
            "        self.timer += dt\n" +
            "        if self.timer < self.step_time:\n" +
            "            return\n" +
            "        self.timer -= self.step_time\n" +
            "        self.dx = self.next_dx\n" +
            "        self.dy = self.next_dy\n" +
            "        head = self.body[1]\n" +
            "        nx = head.x + self.dx\n" +
            "        ny = head.y + self.dy\n" +
            "        if nx < 0 or nx >= self.cols or ny < 0 or ny >= self.rows:\n" +
            "            self.alive = false\n" +
            "            return\n" +
            "        ate = self.food and nx == self.food.x and ny == self.food.y\n" +
            "        limit = ate and #self.body or (#self.body - 1)\n" +
            "        for i = 1, limit:\n" +
            "            if self.body[i].x == nx and self.body[i].y == ny:\n" +
            "                self.alive = false\n" +
            "                return\n" +
            "        table.insert(self.body, 1, Cell({x = nx, y = ny}))\n" +
            "        if ate:\n" +
            "            self.score += 1\n" +
            "            if self.score > self.best:\n" +
            "                self.best = self.score\n" +
            "            if self.step_time > 0.065:\n" +
            "                self.step_time -= 0.004\n" +
            "            self:spawn_food()\n" +
            "        else:\n" +
            "            table.remove(self.body)\n\n" +
            "return {SnakeGame = SnakeGame, Cell = Cell}\n");
        return project;
    }

    File createProject(String requested) throws IOException {
        String name = safeName(requested, "Project");
        File project = unique(root, name);
        if (!new File(project, "scripts").mkdirs() || !new File(project, "assets").mkdirs()) throw new IOException("Cannot create project folders");
        write(new File(project, "main.lua"),
            "-- " + name + " (LuaC LÖVE2D-style blank canvas)\n" +
            "x = 120.0\n" +
            "y = 180.0\n\n" +
            "func update(float dt):\n" +
            "    x += 60.0 * dt\n" +
            "    if x > screen_width():\n" +
            "        x = -48.0\n\n" +
            "func draw():\n" +
            "    clear_screen(0.0, 0.0, 0.0, 1.0)\n" +
            "    draw_rect(x, y, 48, 48, 0.25, 0.90, 0.65, 1.0)\n");
        return project;
    }

    List<File> children(File directory) {
        ArrayList<File> result = new ArrayList<>();
        File[] files = directory.listFiles();
        if (files != null) result.addAll(Arrays.asList(files));
        result.sort(Comparator.comparing(File::isFile).thenComparing((File file) -> file.getName().toLowerCase()));
        return result;
    }

    File createFile(File directory, String requested) throws IOException {
        String name = safeName(requested, "script.lua");
        if (!name.contains(".")) name += ".lua";
        File file = checkedChild(directory, name);
        if (file.exists()) throw new IOException("File already exists");
        write(file, "-- " + name + "\n\n");
        return file;
    }

    File createFolder(File directory, String requested) throws IOException {
        File folder = checkedChild(directory, safeName(requested, "folder"));
        if (!folder.mkdir()) throw new IOException("Cannot create folder");
        return folder;
    }

    File rename(File file, String requested) throws IOException {
        if (!file.exists() || file.equals(root) || !file.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("File is outside the projects folder or does not exist");
        if (requested == null || requested.trim().isEmpty()) throw new IOException("Name cannot be empty");
        String name = requested.trim();
        if (name.equals(file.getName())) return file;
        File target = checkedChild(file.getParentFile(), name);
        if (target.exists()) throw new IOException("A file or folder with this name already exists");
        if (!file.renameTo(target)) throw new IOException("Cannot rename " + file.getName());
        return target;
    }

    int fileCount(File directory) {
        int count = 0;
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) {
            if (child.isDirectory()) count += fileCount(child);
            else count++;
        }
        return count;
    }

    File importFile(File directory, String requested, InputStream input) throws IOException {
        String name = safeName(requested, "asset.bin");
        if (!directory.exists() && !directory.mkdirs()) throw new IOException("Cannot create assets folder");
        File destination = unique(directory, name);
        copy(input, destination);
        return destination;
    }

    String read(File file) throws IOException {
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] data = new byte[(int) Math.min(file.length(), 4 * 1024 * 1024)];
            int offset = 0, count;
            while (offset < data.length && (count = input.read(data, offset, data.length - offset)) >= 0) offset += count;
            return new String(data, 0, offset, StandardCharsets.UTF_8);
        }
    }

    void write(File file, String text) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("Cannot create parent folder");
        try (FileOutputStream output = new FileOutputStream(file)) { output.write(text.getBytes(StandardCharsets.UTF_8)); }
    }

    void delete(File file) throws IOException {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) delete(child);
        }
        if (!file.delete()) throw new IOException("Cannot delete " + file.getName());
    }

    File resolve(File project, String relative) throws IOException {
        File result = new File(project, relative);
        String base = project.getCanonicalPath() + File.separator;
        if (!result.getCanonicalPath().startsWith(base)) throw new IOException("Path leaves the project");
        return result;
    }

    private File checkedChild(File directory, String name) throws IOException {
        if (name.contains("/") || name.contains("\\") || name.equals(".") || name.equals("..")) throw new IOException("Invalid name");
        return resolve(directory, name);
    }

    private static String safeName(String requested, String fallback) {
        String name = requested == null ? "" : requested.trim().replaceAll("[\\\\/:*?\"<>|]", "_");
        return name.isEmpty() ? fallback : name;
    }

    private static File unique(File directory, String name) {
        File result = new File(directory, name);
        if (!result.exists()) return result;
        int dot = name.lastIndexOf('.');
        String base = dot > 0 ? name.substring(0, dot) : name;
        String extension = dot > 0 ? name.substring(dot) : "";
        for (int index = 2; ; ++index) {
            result = new File(directory, base + " " + index + extension);
            if (!result.exists()) return result;
        }
    }

    private static void copy(InputStream input, File destination) throws IOException {
        try (FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
    }
}
