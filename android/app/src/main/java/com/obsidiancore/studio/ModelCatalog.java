package com.obsidiancore.studio;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

final class ModelCatalog {
    static final class CatalogEntry {
        final String id;
        final String title;
        final String subtitle;
        final String filename;
        final String url;

        CatalogEntry(String id, String title, String subtitle, String filename, String url) {
            this.id = id;
            this.title = title;
            this.subtitle = subtitle;
            this.filename = filename;
            this.url = url;
        }
    }

    static final class LocalModel {
        final String name;
        final String pathOrUri;
        final boolean isUri;
        final long sizeBytes;

        LocalModel(String name, String pathOrUri, boolean isUri, long sizeBytes) {
            this.name = name;
            this.pathOrUri = pathOrUri;
            this.isUri = isUri;
            this.sizeBytes = sizeBytes;
        }
    }

    interface DownloadListener {
        void onProgress(long downloadedBytes, long totalBytes, String statusText);
    }

    static final List<CatalogEntry> CATALOG = Arrays.asList(
        new CatalogEntry(
            "qwen25_coder_1_5b",
            "Qwen2.5-Coder 1.5B (Q4_K_M)",
            "~1.1 GB • Recommended balance of speed & code edits",
            "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf",
            "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"
        ),
        new CatalogEntry(
            "qwen25_coder_3b",
            "Qwen2.5-Coder 3B (Q4_K_M)",
            "~2.1 GB • Stronger multi-file reasoning (8+ GB RAM)",
            "qwen2.5-coder-3b-instruct-q4_k_m.gguf",
            "https://huggingface.co/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/resolve/main/qwen2.5-coder-3b-instruct-q4_k_m.gguf"
        ),
        new CatalogEntry(
            "deepseek_r1_1_5b",
            "DeepSeek-R1-Distill-Qwen 1.5B (Q4_K_M)",
            "~1.1 GB • Step-by-step <think> reasoning",
            "DeepSeek-R1-Distill-Qwen-1.5B-Q4_K_M.gguf",
            "https://huggingface.co/bartowski/DeepSeek-R1-Distill-Qwen-1.5B-GGUF/resolve/main/DeepSeek-R1-Distill-Qwen-1.5B-Q4_K_M.gguf"
        ),
        new CatalogEntry(
            "qwen25_0_5b",
            "Qwen2.5 0.5B (Q4_K_M)",
            "~400 MB • Ultra-fast lightweight model",
            "qwen2.5-0.5b-instruct-q4_k_m.gguf",
            "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf"
        ),
        new CatalogEntry(
            "llama32_1b",
            "Llama-3.2 1B (Q4_K_M)",
            "~808 MB • Fast general assistant & shell tasks",
            "Llama-3.2-1B-Instruct-Q4_K_M.gguf",
            "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf"
        )
    );

    static File modelsDir(Context ctx) {
        File dir = new File(ctx.getFilesDir(), "models");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    static List<LocalModel> listLocalModels(Context ctx, SharedPreferences prefs) {
        List<LocalModel> out = new ArrayList<>();
        File dir = modelsDir(ctx);
        File[] files = dir.listFiles();
        if (files != null) {
            Arrays.sort(files, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File f : files) {
                if (f.isFile() && f.getName().endsWith(".gguf")) {
                    out.add(new LocalModel(f.getName(), f.getAbsolutePath(), false, f.length()));
                }
            }
        }
        String uriJson = prefs.getString("picked_model_uris", "[]");
        try {
            JSONArray arr = new JSONArray(uriJson);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                out.add(new LocalModel(
                    o.optString("name", "model.gguf"),
                    o.optString("uri", ""),
                    true,
                    o.optLong("size", 0L)
                ));
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    static void addPickedUriModel(SharedPreferences prefs, String name, String uri, long size) {
        try {
            JSONArray arr = new JSONArray(prefs.getString("picked_model_uris", "[]"));
            JSONArray next = new JSONArray();
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                if (!uri.equals(o.optString("uri"))) next.put(o);
            }
            JSONObject entry = new JSONObject();
            entry.put("name", name);
            entry.put("uri", uri);
            entry.put("size", size);
            next.put(entry);
            prefs.edit().putString("picked_model_uris", next.toString()).apply();
        } catch (Exception ignored) {
        }
    }

    static void removeLocalModel(SharedPreferences prefs, LocalModel model) {
        if (!model.isUri) {
            File f = new File(model.pathOrUri);
            if (f.exists()) f.delete();
        } else {
            try {
                JSONArray arr = new JSONArray(prefs.getString("picked_model_uris", "[]"));
                JSONArray next = new JSONArray();
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject o = arr.getJSONObject(i);
                    if (!model.pathOrUri.equals(o.optString("uri"))) next.put(o);
                }
                prefs.edit().putString("picked_model_uris", next.toString()).apply();
            } catch (Exception ignored) {
            }
        }
    }

    static File downloadModel(Context ctx, String urlStr, String filename,
                              AtomicBoolean cancelFlag, DownloadListener listener) throws Exception {
        if (filename == null || filename.trim().isEmpty()) {
            String pathPart = urlStr;
            int q = pathPart.indexOf('?');
            if (q >= 0) pathPart = pathPart.substring(0, q);
            int slash = pathPart.lastIndexOf('/');
            filename = slash >= 0 ? pathPart.substring(slash + 1) : "model.gguf";
        }
        if (!filename.endsWith(".gguf")) filename += ".gguf";
        filename = filename.replaceAll("[^a-zA-Z0-9._-]", "_");

        File dir = modelsDir(ctx);
        File target = new File(dir, filename);
        File part = new File(dir, filename + ".part");
        if (part.exists()) part.delete();

        String currentUrl = urlStr.trim();
        HttpURLConnection conn = null;
        for (int hop = 0; hop < 6; hop++) {
            conn = (HttpURLConnection) new URL(currentUrl).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(20000);
            conn.setReadTimeout(60000);
            conn.setRequestProperty("User-Agent", "ObsidianAI-Android/2.0");
            int code = conn.getResponseCode();
            if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                String loc = conn.getHeaderField("Location");
                conn.disconnect();
                if (loc == null || loc.isEmpty()) throw new IOException("Redirect without Location");
                currentUrl = loc;
                continue;
            }
            if (code >= 400) {
                conn.disconnect();
                throw new IOException("HTTP " + code + " downloading model");
            }
            break;
        }
        if (conn == null) throw new IOException("Failed to connect");

        long total = conn.getContentLengthLong();
        long downloaded = 0;
        long lastReportMs = 0;
        byte[] buf = new byte[65536];
        try (InputStream in = conn.getInputStream(); FileOutputStream out = new FileOutputStream(part)) {
            int n;
            while ((n = in.read(buf)) >= 0) {
                if (cancelFlag != null && cancelFlag.get()) {
                    part.delete();
                    throw new IOException("Download cancelled");
                }
                out.write(buf, 0, n);
                downloaded += n;
                if (downloaded >= 4 && downloaded - n < 4) {
                    // Check first 4 bytes of partial file for GGUF magic
                    out.flush();
                    try (FileInputStream check = new FileInputStream(part)) {
                        if (!LlamaEngine.isGMagicValid(check)) {
                            part.delete();
                            throw new IOException("Downloaded file is not a valid GGUF binary");
                        }
                    }
                }
                long now = System.currentTimeMillis();
                if (listener != null && (now - lastReportMs >= 250 || (total > 0 && downloaded == total))) {
                    lastReportMs = now;
                    listener.onProgress(downloaded, total, formatProgress(downloaded, total));
                }
            }
        } finally {
            conn.disconnect();
        }

        if (target.exists()) target.delete();
        if (!part.renameTo(target)) {
            throw new IOException("Failed to finalize model file");
        }
        return target;
    }

    static String formatProgress(long downloaded, long total) {
        double mb = downloaded / (1024.0 * 1024.0);
        if (total > 0) {
            double totalMb = total / (1024.0 * 1024.0);
            int pct = (int) Math.min(100, (downloaded * 100L) / total);
            return String.format(java.util.Locale.US, "%.1f / %.1f MB (%d%%)", mb, totalMb, pct);
        }
        return String.format(java.util.Locale.US, "%.1f MB", mb);
    }
}
