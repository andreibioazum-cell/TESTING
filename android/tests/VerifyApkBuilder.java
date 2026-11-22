package com.obsidiancore.studio;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public final class VerifyApkBuilder {
    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("Usage: VerifyApkBuilder <base-apk> <out-apk>");
            System.exit(2);
        }
        File baseApk = new File(args[0]);
        File outApk = new File(args[1]);
        File tmpProj = new File(outApk.getParentFile() != null ? outApk.getParentFile() : new File("."), "ci_sample_game");
        tmpProj.mkdirs();

        try (FileOutputStream fos = new FileOutputStream(new File(tmpProj, "main.obs"))) {
            fos.write("fn on_start()\nend\nfn on_update(dt)\nend\n".getBytes(StandardCharsets.UTF_8));
        }
        try (FileOutputStream fos = new FileOutputStream(new File(tmpProj, "scene.json"))) {
            fos.write("{\"is3D\":true,\"nodes\":[]}".getBytes(StandardCharsets.UTF_8));
        }

        ApkBuilder.SigningIdentity id = ApkBuilder.generateIdentity();
        ApkBuilder.BuildResult res = ApkBuilder.build(
            baseApk, tmpProj, "SpaceDemo", "Space Demo", true, outApk, id, false);

        System.out.println("Built standalone APK: " + res.apkFile.getAbsolutePath()
            + " pkg=" + res.packageId
            + " scripts=" + res.compiledScripts
            + " files=" + res.totalFiles);

        if (!"com.obsidiancore.game.space_demo".equals(res.packageId)) {
            throw new AssertionError("Unexpected packageId: " + res.packageId);
        }
        if (res.compiledScripts != 1 || res.totalFiles != 2) {
            throw new AssertionError("Unexpected counts: scripts=" + res.compiledScripts + " files=" + res.totalFiles);
        }
    }
}
