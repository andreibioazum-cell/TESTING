package com.obsidiancore.studio;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

final class RepoWorkspace {
    static final class ChangedFile {
        final String relPath;
        final String status; // ADDED or MODIFIED
        final byte[] bytes;

        ChangedFile(String relPath, String status, byte[] bytes) {
            this.relPath = relPath;
            this.status = status;
            this.bytes = bytes;
        }
    }

    static final class Meta {
        String owner = "";
        String repo = "";
        String branch = "main";
        String headSha = "";
        final Map<String, String> baseHashes = new LinkedHashMap<>();
    }

    final File repoRoot;

    RepoWorkspace(File repoRoot) {
        this.repoRoot = repoRoot;
    }

    File metaFile() {
        return new File(repoRoot, ".obsidian_ai_meta.json");
    }

    void saveBaseline(String owner, String repo, String branch, String headSha) throws Exception {
        if (!repoRoot.exists()) repoRoot.mkdirs();
        JSONObject root = new JSONObject();
        root.put("owner", owner);
        root.put("repo", repo);
        root.put("branch", branch);
        root.put("headSha", headSha);

        JSONObject hashes = new JSONObject();
        List<File> all = new ArrayList<>();
        collectFiles(repoRoot, all);
        for (File f : all) {
            String rel = relativePath(f);
            if (rel.equals(".obsidian_ai_meta.json") || rel.startsWith(".git/")) continue;
            hashes.put(rel, sha256Hex(readBytes(f)));
        }
        root.put("baseHashes", hashes);
        writeBytes(metaFile(), root.toString(2).getBytes(StandardCharsets.UTF_8));
    }

    Meta loadMeta() throws Exception {
        Meta m = new Meta();
        File mf = metaFile();
        if (!mf.isFile()) return m;
        JSONObject root = new JSONObject(new String(readBytes(mf), StandardCharsets.UTF_8));
        m.owner = root.optString("owner", "");
        m.repo = root.optString("repo", "");
        m.branch = root.optString("branch", "main");
        m.headSha = root.optString("headSha", "");
        JSONObject hashes = root.optJSONObject("baseHashes");
        if (hashes != null) {
            Iterator<String> keys = hashes.keys();
            while (keys.hasNext()) {
                String k = keys.next();
                m.baseHashes.put(k, hashes.optString(k, ""));
            }
        }
        return m;
    }

    String listFiles(String subDir, int maxDepth) throws IOException {
        File base = (subDir == null || subDir.trim().isEmpty() || ".".equals(subDir.trim()))
            ? repoRoot : SafetyGuard.resolveSafeRepoPath(repoRoot, subDir);
        if (!base.exists() || !base.isDirectory()) {
            return "Directory not found: " + (subDir == null ? "." : subDir);
        }
        int depthCap = maxDepth <= 0 ? 4 : Math.min(8, maxDepth);
        List<String> out = new ArrayList<>();
        walkListing(base, 0, depthCap, out, 300);
        if (out.isEmpty()) return "(empty directory)";
        StringBuilder sb = new StringBuilder();
        for (String line : out) sb.append(line).append('\n');
        return sb.toString();
    }

    private void walkListing(File dir, int depth, int maxDepth, List<String> out, int limit) {
        if (out.size() >= limit) return;
        File[] children = dir.listFiles();
        if (children == null) return;
        Arrays.sort(children, (a, b) -> {
            if (a.isDirectory() != b.isDirectory()) return a.isDirectory() ? -1 : 1;
            return a.getName().compareToIgnoreCase(b.getName());
        });
        for (File f : children) {
            if (out.size() >= limit) {
                out.add("... (truncated at " + limit + " entries)");
                return;
            }
            String name = f.getName();
            if (name.equals(".obsidian_ai_meta.json") || name.equals(".git")) continue;
            String rel = relativePath(f);
            if (f.isDirectory()) {
                out.add(rel + "/");
                if (depth + 1 < maxDepth) {
                    walkListing(f, depth + 1, maxDepth, out, limit);
                }
            } else {
                out.add(rel + " (" + f.length() + " B)");
            }
        }
    }

    String readFile(String relPath, int startLine, int endLine) throws IOException {
        File f = SafetyGuard.resolveSafeRepoPath(repoRoot, relPath);
        if (!f.isFile()) return "ERROR: File not found: " + relPath;
        if (f.length() > 1024 * 1024) {
            return "ERROR: File too large (" + f.length() + " bytes) to read directly";
        }
        String text = new String(readBytes(f), StandardCharsets.UTF_8).replace("\r\n", "\n");
        String[] lines = text.split("\n", -1);
        int total = lines.length;
        int s = Math.max(1, startLine <= 0 ? 1 : startLine);
        int e = endLine <= 0 ? Math.min(total, s + 199) : Math.min(total, endLine);
        if (e - s > 250) e = s + 250;
        if (s > total) return "ERROR: start_line " + s + " exceeds total lines (" + total + ")";

        StringBuilder sb = new StringBuilder();
        sb.append("File: ").append(relPath).append(" (lines ").append(s).append("-").append(e)
          .append(" of ").append(total).append(")\n");
        for (int i = s; i <= e; i++) {
            sb.append(String.format(Locale.US, "%4d | %s\n", i, lines[i - 1]));
        }
        return sb.toString();
    }

    String grepSearch(String query, String subPath) throws IOException {
        if (query == null || query.isEmpty()) return "ERROR: Empty search query";
        File base = (subPath == null || subPath.trim().isEmpty() || ".".equals(subPath.trim()))
            ? repoRoot : SafetyGuard.resolveSafeRepoPath(repoRoot, subPath);
        if (!base.exists()) return "ERROR: Path not found: " + subPath;

        List<File> files = new ArrayList<>();
        if (base.isFile()) files.add(base);
        else collectFiles(base, files);

        StringBuilder sb = new StringBuilder();
        int hits = 0;
        for (File f : files) {
            String rel = relativePath(f);
            if (rel.equals(".obsidian_ai_meta.json") || rel.startsWith(".git/")) continue;
            if (f.length() > 512 * 1024) continue;
            byte[] bytes = readBytes(f);
            if (looksBinary(bytes)) continue;
            String text = new String(bytes, StandardCharsets.UTF_8).replace("\r\n", "\n");
            String[] lines = text.split("\n", -1);
            for (int i = 0; i < lines.length; i++) {
                if (lines[i].contains(query)) {
                    sb.append(rel).append(':').append(i + 1).append(": ").append(lines[i].trim()).append('\n');
                    hits++;
                    if (hits >= 80) {
                        sb.append("... (truncated at 80 matches)\n");
                        return sb.toString();
                    }
                }
            }
        }
        return hits == 0 ? "No matches for: " + query : sb.toString();
    }

    String editFile(String relPath, String oldText, String newText) throws IOException {
        File f = SafetyGuard.resolveSafeRepoPath(repoRoot, relPath);
        if (!f.isFile()) {
            return "ERROR: File does not exist: " + relPath + ". Use write_new_file only for brand-new files.";
        }
        if (oldText == null || oldText.isEmpty()) {
            return "ERROR: old_text must not be empty. Read the file first and provide the exact lines to replace.";
        }
        if (newText == null) newText = "";
        if (oldText.equals(newText)) {
            return "ERROR: new_text is identical to old_text; no change made.";
        }

        String raw = new String(readBytes(f), StandardCharsets.UTF_8);
        boolean hadCrlf = raw.contains("\r\n");
        String norm = raw.replace("\r\n", "\n");
        String normOld = oldText.replace("\r\n", "\n");
        String normNew = newText.replace("\r\n", "\n");

        int firstIdx = norm.indexOf(normOld);
        if (firstIdx < 0) {
            return "ERROR: old_text not found in " + relPath + ". Call read_file first to verify exact whitespace and lines.";
        }
        int secondIdx = norm.indexOf(normOld, firstIdx + 1);
        if (secondIdx >= 0) {
            return "ERROR: old_text matches multiple locations in " + relPath + ". Include more surrounding context lines.";
        }

        String updated = norm.substring(0, firstIdx) + normNew + norm.substring(firstIdx + normOld.length());
        String toWrite = hadCrlf ? updated.replace("\n", "\r\n") : updated;
        writeBytes(f, toWrite.getBytes(StandardCharsets.UTF_8));

        int startLine = 1;
        for (int i = 0; i < firstIdx; i++) {
            if (norm.charAt(i) == '\n') startLine++;
        }
        return formatUnifiedSnippet(relPath, startLine, normOld, normNew);
    }

    String writeNewFile(String relPath, String content) throws IOException {
        File f = SafetyGuard.resolveSafeRepoPath(repoRoot, relPath);
        if (f.exists()) {
            if (f.isDirectory()) return "ERROR: Path is a directory: " + relPath;
            if (f.length() > 0) {
                return "ERROR: Refusing to overwrite existing non-empty file '" + relPath
                    + "' from scratch. Use read_file + edit_file for incremental edits.";
            }
        }
        File parent = f.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return "ERROR: Failed to create directories for " + relPath;
        }
        String text = content == null ? "" : content;
        writeBytes(f, text.getBytes(StandardCharsets.UTF_8));
        int lines = text.isEmpty() ? 0 : text.split("\n", -1).length;
        return "Created new file " + relPath + " (" + lines + " lines, " + text.getBytes(StandardCharsets.UTF_8).length + " bytes)";
    }

    List<ChangedFile> computeChangedFiles() throws Exception {
        Meta meta = loadMeta();
        List<File> current = new ArrayList<>();
        collectFiles(repoRoot, current);
        List<ChangedFile> changed = new ArrayList<>();
        for (File f : current) {
            String rel = relativePath(f);
            if (rel.equals(".obsidian_ai_meta.json") || rel.startsWith(".git/")) continue;
            byte[] bytes = readBytes(f);
            String sha = sha256Hex(bytes);
            String baseSha = meta.baseHashes.get(rel);
            if (baseSha == null) {
                changed.add(new ChangedFile(rel, "ADDED", bytes));
            } else if (!baseSha.equals(sha)) {
                changed.add(new ChangedFile(rel, "MODIFIED", bytes));
            }
        }
        return changed;
    }

    String statusSummary() throws Exception {
        Meta meta = loadMeta();
        List<ChangedFile> changed = computeChangedFiles();
        StringBuilder sb = new StringBuilder();
        sb.append("Repo: ").append(meta.owner).append('/').append(meta.repo)
          .append(" @ ").append(meta.branch).append('\n');
        if (changed.isEmpty()) {
            sb.append("Working tree clean (no modified or added files).\n");
        } else {
            sb.append("Changed files (").append(changed.size()).append("):\n");
            for (ChangedFile cf : changed) {
                sb.append("  ").append(cf.status).append("  ").append(cf.relPath)
                  .append(" (").append(cf.bytes.length).append(" B)\n");
            }
        }
        return sb.toString();
    }

    String runBash(String command) {
        SafetyGuard.CheckResult guard = SafetyGuard.checkBashCommand(command, repoRoot);
        if (!guard.allowed) {
            return "BLOCKED_BY_SAFETY_GUARD: " + guard.reason;
        }
        if (repoRoot == null || !repoRoot.isDirectory()) {
            return "ERROR: No synced repository directory available.";
        }
        try {
            String shell = new File("/system/bin/sh").exists() ? "/system/bin/sh" : "/bin/sh";
            ProcessBuilder pb = new ProcessBuilder(shell, "-c", command);
            pb.directory(repoRoot);
            pb.redirectErrorStream(true);
            Process proc = pb.start();
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            Thread reader = new Thread(() -> {
                byte[] buf = new byte[4096];
                try (InputStream in = proc.getInputStream()) {
                    int n;
                    while ((n = in.read(buf)) >= 0) {
                        if (bos.size() < 24000) bos.write(buf, 0, Math.min(n, 24000 - bos.size()));
                    }
                } catch (IOException ignored) {
                }
            });
            reader.start();
            boolean finished = proc.waitFor(25, TimeUnit.SECONDS);
            if (!finished) {
                proc.destroyForcibly();
                reader.join(500);
                return "TIMEOUT after 25s.\nPartial output:\n" + bos.toString(StandardCharsets.UTF_8.name());
            }
            reader.join(1000);
            int code = proc.exitValue();
            String out = bos.toString(StandardCharsets.UTF_8.name());
            if (out.length() > 12000) {
                out = out.substring(0, 6000) + "\n... (middle truncated) ...\n" + out.substring(out.length() - 5000);
            }
            return "Exit code: " + code + "\n" + (out.isEmpty() ? "(no output)" : out);
        } catch (Exception e) {
            return "ERROR running shell command: " + e.getMessage();
        }
    }

    private static String formatUnifiedSnippet(String path, int startLine, String oldText, String newText) {
        String[] oldLines = oldText.split("\n", -1);
        String[] newLines = newText.split("\n", -1);
        StringBuilder sb = new StringBuilder();
        sb.append("Edited ").append(path).append('\n');
        sb.append("--- a/").append(path).append('\n');
        sb.append("+++ b/").append(path).append('\n');
        sb.append("@@ -").append(startLine).append(',').append(oldLines.length)
          .append(" +").append(startLine).append(',').append(newLines.length).append(" @@\n");
        for (String l : oldLines) sb.append("- ").append(l).append('\n');
        for (String l : newLines) sb.append("+ ").append(l).append('\n');
        return sb.toString();
    }

    private String relativePath(File f) {
        String r = repoRoot.getAbsolutePath();
        String p = f.getAbsolutePath();
        if (p.startsWith(r)) {
            p = p.substring(r.length());
            while (p.startsWith("/") || p.startsWith("\\")) p = p.substring(1);
        }
        return p.replace('\\', '/');
    }

    private static void collectFiles(File dir, List<File> out) {
        File[] list = dir.listFiles();
        if (list == null) return;
        Arrays.sort(list, (a, b) -> a.getName().compareTo(b.getName()));
        for (File f : list) {
            if (f.getName().equals(".git") || f.getName().equals(".obsidian_ai_meta.json")) continue;
            if (f.isDirectory()) collectFiles(f, out);
            else if (f.isFile()) out.add(f);
        }
    }

    private static boolean looksBinary(byte[] b) {
        int lim = Math.min(b.length, 512);
        for (int i = 0; i < lim; i++) {
            if (b[i] == 0) return true;
        }
        return false;
    }

    static byte[] readBytes(File f) throws IOException {
        try (FileInputStream fis = new FileInputStream(f);
             ByteArrayOutputStream bos = new ByteArrayOutputStream()) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = fis.read(buf)) >= 0) bos.write(buf, 0, n);
            return bos.toByteArray();
        }
    }

    static void writeBytes(File f, byte[] data) throws IOException {
        try (FileOutputStream fos = new FileOutputStream(f)) {
            fos.write(data);
        }
    }

    static String sha256Hex(byte[] data) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] dig = md.digest(data);
        StringBuilder sb = new StringBuilder(dig.length * 2);
        for (byte b : dig) sb.append(String.format(Locale.US, "%02x", b & 0xFF));
        return sb.toString();
    }
}
