package com.obsidiancore.studio;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
import java.io.InputStream;

final class LlamaEngine {
    interface TokenCallback {
        void onToken(String tokenPiece);
        void onPrefillProgress(int done, int total);
    }

    static {
        System.loadLibrary("obsidian_ai");
    }

    private static ParcelFileDescriptor activePfd;

    static boolean isGMagicValid(InputStream in) throws IOException {
        byte[] hdr = new byte[4];
        int n = in.read(hdr);
        return n == 4 && hdr[0] == 'G' && hdr[1] == 'G' && hdr[2] == 'U' && hdr[3] == 'F';
    }

    static synchronized String loadFromFile(String filePath, int nCtx, int nThreads) {
        closeActivePfd();
        return nativeLoadModel(filePath, nCtx, nThreads);
    }

    static synchronized String loadFromUri(ContentResolver cr, Uri uri, int nCtx, int nThreads) {
        closeActivePfd();
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null || !isGMagicValid(in)) {
                return "ERROR: Selected file is not a valid GGUF model";
            }
        } catch (Exception e) {
            return "ERROR: Cannot read model file: " + e.getMessage();
        }

        try {
            ParcelFileDescriptor pfd = cr.openFileDescriptor(uri, "r");
            if (pfd == null) {
                return "ERROR: Cannot open file descriptor for model";
            }
            activePfd = pfd;
            String fdPath = "/proc/self/fd/" + pfd.getFd();
            String res = nativeLoadModel(fdPath, nCtx, nThreads);
            if (res == null || !res.startsWith("OK:")) {
                closeActivePfd();
            }
            return res;
        } catch (Exception e) {
            closeActivePfd();
            return "ERROR: " + e.getMessage();
        }
    }

    static synchronized void unload() {
        nativeUnloadModel();
        closeActivePfd();
    }

    private static void closeActivePfd() {
        if (activePfd != null) {
            try {
                activePfd.close();
            } catch (IOException ignored) {
            }
            activePfd = null;
        }
    }

    static native String nativeLoadModel(String path, int nCtx, int nThreads);
    static native void nativeUnloadModel();
    static native void nativeStopGeneration();
    static native boolean nativeIsLoaded();
    static native String nativeGenerate(String[] roles, String[] contents, int maxTokens, float temp, int coolDelayMs, TokenCallback cb);
}
