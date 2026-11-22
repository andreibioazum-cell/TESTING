package com.obsidiancore.studio;

import android.content.Context;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.Signature;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONArray;
import org.json.JSONObject;

final class ApkBuilder {
    static final class BuildResult {
        final File apkFile;
        final String packageId;
        final int compiledScripts;
        final int totalFiles;

        BuildResult(File apkFile, String packageId, int compiledScripts, int totalFiles) {
            this.apkFile = apkFile;
            this.packageId = packageId;
            this.compiledScripts = compiledScripts;
            this.totalFiles = totalFiles;
        }
    }

    static native String nativeCompileCheckObs(String source, String filename);

    static String makePackageId(String appName, String projectName) {
        String base = (appName != null && !appName.trim().isEmpty()) ? appName : projectName;
        if (base == null) base = "game";
        StringBuilder slug = new StringBuilder();
        for (int i = 0; i < base.length(); i++) {
            char c = Character.toLowerCase(base.charAt(i));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug.append(c);
            else if (slug.length() > 0 && slug.charAt(slug.length() - 1) != '_') slug.append('_');
        }
        while (slug.length() > 0 && slug.charAt(slug.length() - 1) == '_') slug.deleteCharAt(slug.length() - 1);
        String s = slug.toString();
        if (s.isEmpty()) s = "app";
        if (Character.isDigit(s.charAt(0))) s = "g_" + s;
        if (s.length() > 28) s = s.substring(0, 28);
        return "com.obsidiancore.game." + s;
    }

    static BuildResult buildFromApp(Context ctx, File projectDir, String projectName,
                                    String appName, boolean is3D, File outApk) throws Exception {
        String sourceApk = ctx.getApplicationInfo().sourceDir;
        File keyDir = new File(ctx.getFilesDir(), "signing");
        if (!keyDir.exists()) keyDir.mkdirs();
        SigningIdentity id = loadOrCreateIdentity(new File(keyDir, "obsidron_key.pk8"),
                                                  new File(keyDir, "obsidron_cert.der"));
        return build(new File(sourceApk), projectDir, projectName, appName, is3D, outApk, id, true);
    }

    static BuildResult build(File baseApkFile, File projectDir, String projectName, String appName,
                             boolean is3D, File outApk, SigningIdentity id, boolean runNativeCompileCheck) throws Exception {
        if (projectDir == null || !projectDir.isDirectory()) {
            throw new IOException("Project directory not found");
        }
        String safeAppName = (appName == null || appName.trim().isEmpty()) ? projectName : appName.trim();
        String pkgId = makePackageId(safeAppName, projectName);

        // 1. Collect project files and compile-check every .obs/.lua script
        List<File> projFiles = new ArrayList<>();
        collectFiles(projectDir, projFiles);
        int scriptCount = 0;
        JSONArray scriptNames = new JSONArray();
        for (File f : projFiles) {
            String rel = relativePath(projectDir, f);
            if (rel.equals(".build_id") || rel.startsWith(".git")) continue;
            if (rel.endsWith(".obs") || rel.endsWith(".lua")) {
                String src = readUtf8(f);
                if (runNativeCompileCheck) {
                    String err = nativeCompileCheckObs(src, rel);
                    if (err != null && !err.isEmpty()) {
                        throw new IOException("Compile error in " + rel + ":\n" + err);
                    }
                }
                scriptCount++;
                scriptNames.put(rel);
            }
        }

        // 2. Build unsigned APK entries
        Map<String, ZipItem> items = new LinkedHashMap<>();
        try (ZipFile baseZip = new ZipFile(baseApkFile)) {
            Enumeration<? extends ZipEntry> en = baseZip.entries();
            while (en.hasMoreElements()) {
                ZipEntry ze = en.nextElement();
                if (ze.isDirectory()) continue;
                String name = ze.getName();
                if (name.startsWith("META-INF/")) continue;
                if (name.startsWith("assets/game_project/") || name.equals("assets/game_meta.json")) continue;

                byte[] data = readAll(baseZip.getInputStream(ze));
                if (name.equals("AndroidManifest.xml")) {
                    data = patchManifestAxml(data, pkgId, safeAppName);
                }
                int align = 0;
                boolean stored = false;
                if (name.equals("resources.arsc")) {
                    stored = true;
                    align = 4;
                } else if (name.startsWith("lib/") && name.endsWith(".so")) {
                    stored = true;
                    align = 4096;
                } else if (ze.getMethod() == ZipEntry.STORED) {
                    stored = true;
                    align = 4;
                }
                items.put(name, new ZipItem(name, data, stored, align));
            }
        }

        // 3. Embed game_meta.json and game_project/* files
        String buildId = Long.toString(System.currentTimeMillis());
        JSONObject meta = new JSONObject();
        meta.put("projectName", projectName);
        meta.put("appName", safeAppName);
        meta.put("packageId", pkgId);
        meta.put("is3D", is3D);
        meta.put("buildId", buildId);
        meta.put("scripts", scriptNames);
        byte[] metaBytes = meta.toString(2).getBytes(StandardCharsets.UTF_8);
        items.put("assets/game_meta.json", new ZipItem("assets/game_meta.json", metaBytes, false, 0));

        int totalCount = 0;
        for (File f : projFiles) {
            String rel = relativePath(projectDir, f);
            if (rel.equals(".build_id") || rel.startsWith(".git")) continue;
            byte[] bytes = readFileBytes(f);
            String entryName = "assets/game_project/" + rel;
            items.put(entryName, new ZipItem(entryName, bytes, false, 0));
            totalCount++;
        }

        // 4. Generate JAR v1 manifest & signature files
        addV1SignatureEntries(items, id);

        // 5. Write ZIP + inject APK Signature Scheme v2 block
        if (outApk.getParentFile() != null && !outApk.getParentFile().exists()) {
            outApk.getParentFile().mkdirs();
        }
        byte[] unsignedZip = writeZip(items);
        byte[] signedApk = applyV2Signature(unsignedZip, id);
        try (FileOutputStream fos = new FileOutputStream(outApk)) {
            fos.write(signedApk);
        }
        return new BuildResult(outApk, pkgId, scriptCount, totalCount);
    }

    // -------------------------------------------------------------------------
    // AXML (Binary AndroidManifest.xml) StringPool Patcher
    // -------------------------------------------------------------------------
    static byte[] patchManifestAxml(byte[] axml, String newPkgId, String newAppName) throws IOException {
        if (axml.length < 36) throw new IOException("Invalid AXML manifest");
        ByteBuffer bb = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN);
        int docType = bb.getShort(0) & 0xFFFF;
        int docHdr = bb.getShort(2) & 0xFFFF;
        int fileSize = bb.getInt(4);
        if (docType != 0x0003 || docHdr != 8 || fileSize > axml.length) {
            throw new IOException("Unsupported AXML header");
        }

        int spOff = docHdr;
        int spType = bb.getShort(spOff) & 0xFFFF;
        int spHdrSize = bb.getShort(spOff + 2) & 0xFFFF;
        int spChunkSize = bb.getInt(spOff + 4);
        if (spType != 0x0001 || spHdrSize < 28) {
            throw new IOException("Expected StringPool chunk in AXML");
        }
        int strCount = bb.getInt(spOff + 8);
        int styleCount = bb.getInt(spOff + 12);
        int flags = bb.getInt(spOff + 16);
        int stringsStart = bb.getInt(spOff + 20);
        int stylesStart = bb.getInt(spOff + 24);
        boolean isUtf8 = (flags & 0x100) != 0;

        int offsetsBase = spOff + spHdrSize;
        int absStringsStart = spOff + stringsStart;
        String[] strings = new String[strCount];
        for (int i = 0; i < strCount; i++) {
            int off = absStringsStart + bb.getInt(offsetsBase + i * 4);
            strings[i] = isUtf8 ? decodeAxmlUtf8(axml, off) : decodeAxmlUtf16(axml, off);
        }

        for (int i = 0; i < strCount; i++) {
            strings[i] = rewriteManifestString(strings[i], newPkgId, newAppName);
        }

        ByteArrayOutputStream strPoolData = new ByteArrayOutputStream();
        int[] newStrOffsets = new int[strCount];
        for (int i = 0; i < strCount; i++) {
            newStrOffsets[i] = strPoolData.size();
            if (isUtf8) encodeAxmlUtf8(strPoolData, strings[i]);
            else encodeAxmlUtf16(strPoolData, strings[i]);
        }
        while (strPoolData.size() % 4 != 0) strPoolData.write(0);
        byte[] newStrBytes = strPoolData.toByteArray();

        // Preserve any style offsets and style blob after strings
        int styleBlobStart = (styleCount > 0 && stylesStart > 0) ? (spOff + stylesStart) : (spOff + spChunkSize);
        int styleBlobLen = Math.max(0, (spOff + spChunkSize) - styleBlobStart);
        int styleOffsetsBytesLen = styleCount * 4;

        int newStringsStart = spHdrSize + strCount * 4 + styleOffsetsBytesLen;
        int newStylesStart = (styleCount > 0 && stylesStart > 0) ? (newStringsStart + newStrBytes.length) : 0;
        int newSpChunkSize = newStringsStart + newStrBytes.length + styleBlobLen;
        int tailOff = spOff + spChunkSize;
        int tailLen = fileSize - tailOff;
        int newFileSize = docHdr + newSpChunkSize + tailLen;

        ByteArrayOutputStream out = new ByteArrayOutputStream(newFileSize);
        byte[] docHeader = new byte[8];
        ByteBuffer.wrap(docHeader).order(ByteOrder.LITTLE_ENDIAN)
            .putShort((short) 0x0003).putShort((short) 8).putInt(newFileSize);
        out.write(docHeader);

        byte[] spHeader = Arrays.copyOfRange(axml, spOff, spOff + spHdrSize);
        ByteBuffer.wrap(spHeader).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(4, newSpChunkSize)
            .putInt(20, newStringsStart)
            .putInt(24, newStylesStart);
        out.write(spHeader);

        byte[] offBuf = new byte[strCount * 4];
        ByteBuffer ob = ByteBuffer.wrap(offBuf).order(ByteOrder.LITTLE_ENDIAN);
        for (int off : newStrOffsets) ob.putInt(off);
        out.write(offBuf);

        if (styleOffsetsBytesLen > 0) {
            out.write(axml, offsetsBase + strCount * 4, styleOffsetsBytesLen);
        }
        out.write(newStrBytes);
        if (styleBlobLen > 0) {
            out.write(axml, styleBlobStart, styleBlobLen);
        }
        out.write(axml, tailOff, tailLen);
        return out.toByteArray();
    }

    private static String rewriteManifestString(String s, String newPkgId, String newAppName) {
        if (s == null) return "";
        if (s.equals("com.obsidiancore.studio")) return newPkgId;
        if (s.equals("Obsidian Core")) return newAppName;
        if (s.equals("com.obsidiancore.studio.MainActivity") || s.equals(".MainActivity")) {
            return "com.obsidiancore.studio.GamePlayerActivity";
        }
        if (s.equals("com.obsidiancore.studio.GamePlayerActivity") || s.equals(".GamePlayerActivity")) {
            return "com.obsidiancore.studio.MainActivity";
        }
        if (s.startsWith("com.obsidiancore.studio.") && !s.endsWith("Activity")) {
            return newPkgId + s.substring("com.obsidiancore.studio".length());
        }
        return s;
    }

    private static String decodeAxmlUtf8(byte[] data, int off) {
        int p = off;
        int u16len = data[p++] & 0xFF;
        if ((u16len & 0x80) != 0) p++;
        int u8len = data[p++] & 0xFF;
        if ((u8len & 0x80) != 0) {
            u8len = ((u8len & 0x7F) << 8) | (data[p++] & 0xFF);
        }
        return new String(data, p, Math.min(u8len, data.length - p), StandardCharsets.UTF_8);
    }

    private static void encodeAxmlUtf8(ByteArrayOutputStream out, String s) throws IOException {
        int u16len = s.length();
        byte[] utf8 = s.getBytes(StandardCharsets.UTF_8);
        int u8len = utf8.length;
        if (u16len > 0x7F) {
            out.write(0x80 | ((u16len >> 8) & 0x7F));
            out.write(u16len & 0xFF);
        } else {
            out.write(u16len);
        }
        if (u8len > 0x7F) {
            out.write(0x80 | ((u8len >> 8) & 0x7F));
            out.write(u8len & 0xFF);
        } else {
            out.write(u8len);
        }
        out.write(utf8);
        out.write(0);
    }

    private static String decodeAxmlUtf16(byte[] data, int off) {
        ByteBuffer bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int len = bb.getShort(off) & 0xFFFF;
        int p = off + 2;
        if ((len & 0x8000) != 0) {
            len = ((len & 0x7FFF) << 16) | (bb.getShort(p) & 0xFFFF);
            p += 2;
        }
        return new String(data, p, Math.min(len * 2, data.length - p), StandardCharsets.UTF_16LE);
    }

    private static void encodeAxmlUtf16(ByteArrayOutputStream out, String s) throws IOException {
        int len = s.length();
        if (len > 0x7FFF) {
            int hi = 0x8000 | (len >> 16);
            int lo = len & 0xFFFF;
            out.write(hi & 0xFF); out.write((hi >> 8) & 0xFF);
            out.write(lo & 0xFF); out.write((lo >> 8) & 0xFF);
        } else {
            out.write(len & 0xFF); out.write((len >> 8) & 0xFF);
        }
        out.write(s.getBytes(StandardCharsets.UTF_16LE));
        out.write(0);
        out.write(0);
    }

    // -------------------------------------------------------------------------
    // Pure Java Aligned ZIP Writer
    // -------------------------------------------------------------------------
    static final class ZipItem {
        final String name;
        final byte[] raw;
        final boolean stored;
        final int align;

        ZipItem(String name, byte[] raw, boolean stored, int align) {
            this.name = name;
            this.raw = raw;
            this.stored = stored;
            this.align = align;
        }
    }

    private static final class CdRecord {
        final String name;
        final byte[] nameBytes;
        final int method;
        final int crc;
        final int compSize;
        final int uncompSize;
        final int localOffset;

        CdRecord(String name, byte[] nameBytes, int method, int crc, int compSize, int uncompSize, int localOffset) {
            this.name = name;
            this.nameBytes = nameBytes;
            this.method = method;
            this.crc = crc;
            this.compSize = compSize;
            this.uncompSize = uncompSize;
            this.localOffset = localOffset;
        }
    }

    static byte[] writeZip(Map<String, ZipItem> items) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(8 * 1024 * 1024);
        List<CdRecord> records = new ArrayList<>(items.size());

        for (ZipItem item : items.values()) {
            byte[] nameBytes = item.name.getBytes(StandardCharsets.UTF_8);
            CRC32 crc32 = new CRC32();
            crc32.update(item.raw);
            int crc = (int) crc32.getValue();
            int method = item.stored ? ZipEntry.STORED : ZipEntry.DEFLATED;
            byte[] payload = item.stored ? item.raw : deflateRaw(item.raw);

            int localOffset = out.size();
            int extraLen = 0;
            if (item.stored && item.align > 1) {
                int dataStart = localOffset + 30 + nameBytes.length;
                extraLen = (item.align - (dataStart % item.align)) % item.align;
            }

            byte[] lfh = new byte[30];
            ByteBuffer lb = ByteBuffer.wrap(lfh).order(ByteOrder.LITTLE_ENDIAN);
            lb.putInt(0x04034B50);
            lb.putShort((short) 20);
            lb.putShort((short) 0x0800); // UTF-8 filename
            lb.putShort((short) method);
            lb.putShort((short) 0);
            lb.putShort((short) 0x5421);
            lb.putInt(crc);
            lb.putInt(payload.length);
            lb.putInt(item.raw.length);
            lb.putShort((short) nameBytes.length);
            lb.putShort((short) extraLen);
            out.write(lfh);
            out.write(nameBytes);
            if (extraLen > 0) out.write(new byte[extraLen]);
            out.write(payload);

            records.add(new CdRecord(item.name, nameBytes, method, crc, payload.length, item.raw.length, localOffset));
        }

        int cdOffset = out.size();
        for (CdRecord r : records) {
            byte[] cdh = new byte[46];
            ByteBuffer cb = ByteBuffer.wrap(cdh).order(ByteOrder.LITTLE_ENDIAN);
            cb.putInt(0x02014B50);
            cb.putShort((short) 20);
            cb.putShort((short) 20);
            cb.putShort((short) 0x0800);
            cb.putShort((short) r.method);
            cb.putShort((short) 0);
            cb.putShort((short) 0x5421);
            cb.putInt(r.crc);
            cb.putInt(r.compSize);
            cb.putInt(r.uncompSize);
            cb.putShort((short) r.nameBytes.length);
            cb.putShort((short) 0);
            cb.putShort((short) 0);
            cb.putShort((short) 0);
            cb.putShort((short) 0);
            cb.putInt(0);
            cb.putInt(r.localOffset);
            out.write(cdh);
            out.write(r.nameBytes);
        }
        int cdSize = out.size() - cdOffset;

        byte[] eocd = new byte[22];
        ByteBuffer eb = ByteBuffer.wrap(eocd).order(ByteOrder.LITTLE_ENDIAN);
        eb.putInt(0x06054B50);
        eb.putShort((short) 0);
        eb.putShort((short) 0);
        eb.putShort((short) records.size());
        eb.putShort((short) records.size());
        eb.putInt(cdSize);
        eb.putInt(cdOffset);
        eb.putShort((short) 0);
        out.write(eocd);

        return out.toByteArray();
    }

    private static byte[] deflateRaw(byte[] input) {
        Deflater def = new Deflater(Deflater.DEFAULT_COMPRESSION, true);
        def.setInput(input);
        def.finish();
        ByteArrayOutputStream bos = new ByteArrayOutputStream(Math.max(64, input.length / 2));
        byte[] buf = new byte[16384];
        while (!def.finished()) {
            int n = def.deflate(buf);
            bos.write(buf, 0, n);
        }
        def.end();
        return bos.toByteArray();
    }

    // -------------------------------------------------------------------------
    // Signing Identity (RSA-2048 + Self-Signed X.509 v3 DER)
    // -------------------------------------------------------------------------
    static final class SigningIdentity {
        final PrivateKey privateKey;
        final X509Certificate cert;
        final byte[] certDer;

        SigningIdentity(PrivateKey privateKey, X509Certificate cert, byte[] certDer) {
            this.privateKey = privateKey;
            this.cert = cert;
            this.certDer = certDer;
        }
    }

    static SigningIdentity loadOrCreateIdentity(File keyFile, File certFile) throws Exception {
        if (keyFile.isFile() && certFile.isFile()) {
            try {
                byte[] pk8 = readFileBytes(keyFile);
                byte[] der = readFileBytes(certFile);
                PrivateKey pk = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(pk8));
                X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                    .generateCertificate(new ByteArrayInputStream(der));
                return new SigningIdentity(pk, cert, der);
            } catch (Exception ignored) {
            }
        }
        SigningIdentity id = generateIdentity();
        try (FileOutputStream kOut = new FileOutputStream(keyFile);
             FileOutputStream cOut = new FileOutputStream(certFile)) {
            kOut.write(id.privateKey.getEncoded());
            cOut.write(id.certDer);
        }
        return id;
    }

    static SigningIdentity generateIdentity() throws Exception {
        KeyPairGenerator kpg = KeyPairGenerator.getInstance("RSA");
        kpg.initialize(2048);
        KeyPair kp = kpg.generateKeyPair();

        // Build minimal RFC 5280 self-signed X.509 v3 certificate
        byte[] version = derTag(0xA0, derInteger(new byte[]{2})); // v3
        byte[] serial = derInteger(new byte[]{1});
        byte[] sha256RsaAlg = derSeq(derOid(new int[]{1, 2, 840, 113549, 1, 1, 11}), derNull());
        byte[] cnAttr = derSeq(derOid(new int[]{2, 5, 4, 3}), derUtf8("Obsidian Core"));
        byte[] name = derSeq(derSet(cnAttr));
        byte[] validity = derSeq(derUtcTime("240101000000Z"), derUtcTime("490101000000Z"));
        byte[] spki = kp.getPublic().getEncoded();

        byte[] tbs = derSeq(version, serial, sha256RsaAlg, name, validity, name, spki);
        Signature sig = Signature.getInstance("SHA256withRSA");
        sig.initSign(kp.getPrivate());
        sig.update(tbs);
        byte[] sigBytes = sig.sign();

        byte[] certDer = derSeq(tbs, sha256RsaAlg, derBitString(sigBytes));
        X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
            .generateCertificate(new ByteArrayInputStream(certDer));
        return new SigningIdentity(kp.getPrivate(), cert, certDer);
    }

    // -------------------------------------------------------------------------
    // JAR v1 Signing (MANIFEST.MF, CERT.SF, CERT.RSA)
    // -------------------------------------------------------------------------
    private static void addV1SignatureEntries(Map<String, ZipItem> items, SigningIdentity id) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        Base64.Encoder b64 = Base64.getEncoder();

        StringBuilder mf = new StringBuilder();
        mf.append("Manifest-Version: 1.0\r\n");
        mf.append("Created-By: Obsidian Core Studio\r\n\r\n");

        StringBuilder sfBody = new StringBuilder();
        for (ZipItem item : items.values()) {
            String dig = b64.encodeToString(sha.digest(item.raw));
            mf.append("Name: ").append(item.name).append("\r\n");
            mf.append("SHA-256-Digest: ").append(dig).append("\r\n\r\n");

            String section = "Name: " + item.name + "\r\nSHA-256-Digest: " + dig + "\r\n\r\n";
            String secDig = b64.encodeToString(sha.digest(section.getBytes(StandardCharsets.UTF_8)));
            sfBody.append("Name: ").append(item.name).append("\r\n");
            sfBody.append("SHA-256-Digest: ").append(secDig).append("\r\n\r\n");
        }

        byte[] mfBytes = mf.toString().getBytes(StandardCharsets.UTF_8);
        String mfDigest = b64.encodeToString(sha.digest(mfBytes));
        String sfStr = "Signature-Version: 1.0\r\n" +
            "Created-By: Obsidian Core Studio\r\n" +
            "SHA-256-Digest-Manifest: " + mfDigest + "\r\n\r\n" +
            sfBody;
        byte[] sfBytes = sfStr.getBytes(StandardCharsets.UTF_8);
        byte[] rsaBlock = buildPkcs7SignedData(sfBytes, id);

        items.put("META-INF/MANIFEST.MF", new ZipItem("META-INF/MANIFEST.MF", mfBytes, false, 0));
        items.put("META-INF/CERT.SF", new ZipItem("META-INF/CERT.SF", sfBytes, false, 0));
        items.put("META-INF/CERT.RSA", new ZipItem("META-INF/CERT.RSA", rsaBlock, false, 0));
    }

    private static byte[] buildPkcs7SignedData(byte[] content, SigningIdentity id) throws Exception {
        Signature sig = Signature.getInstance("SHA256withRSA");
        sig.initSign(id.privateKey);
        sig.update(content);
        byte[] signature = sig.sign();

        byte[] sha256Alg = derSeq(derOid(new int[]{2, 16, 840, 1, 101, 3, 4, 2, 1}), derNull());
        byte[] rsaAlg = derSeq(derOid(new int[]{1, 2, 840, 113549, 1, 1, 1}), derNull());
        byte[] dataOid = derOid(new int[]{1, 2, 840, 113549, 1, 7, 1});
        byte[] signedDataOid = derOid(new int[]{1, 2, 840, 113549, 1, 7, 2});

        byte[] encapContentInfo = derSeq(dataOid); // detached signature
        byte[] certs = derTag(0xA0, id.certDer);

        byte[] issuerBytes = id.cert.getIssuerX500Principal().getEncoded();
        byte[] serialBytes = id.cert.getSerialNumber().toByteArray();
        byte[] issuerAndSerial = derSeq(issuerBytes, derInteger(serialBytes));

        byte[] signerInfo = derSeq(
            derInteger(new byte[]{1}),
            issuerAndSerial,
            sha256Alg,
            rsaAlg,
            derOctetString(signature)
        );
        byte[] signedData = derSeq(
            derInteger(new byte[]{1}),
            derSet(sha256Alg),
            encapContentInfo,
            certs,
            derSet(signerInfo)
        );
        return derSeq(signedDataOid, derTag(0xA0, signedData));
    }

    // -------------------------------------------------------------------------
    // APK Signature Scheme v2
    // -------------------------------------------------------------------------
    static byte[] applyV2Signature(byte[] zip, SigningIdentity id) throws Exception {
        int eocdOff = zip.length - 22;
        if (eocdOff < 0 || ByteBuffer.wrap(zip).order(ByteOrder.LITTLE_ENDIAN).getInt(eocdOff) != 0x06054B50) {
            throw new IOException("Invalid ZIP EOCD");
        }
        ByteBuffer eb = ByteBuffer.wrap(zip).order(ByteOrder.LITTLE_ENDIAN);
        int cdSize = eb.getInt(eocdOff + 12);
        int cdOff = eb.getInt(eocdOff + 16);

        // Compute chunked SHA-256 over sections 1 (entries), 3 (CD), 4 (EOCD with cdOff unchanged)
        byte[] rootDigest = computeApkV2ChunkedSha256(
            Arrays.copyOfRange(zip, 0, cdOff),
            Arrays.copyOfRange(zip, cdOff, eocdOff),
            Arrays.copyOfRange(zip, eocdOff, zip.length)
        );

        // signed-data: digests (uint32-prefixed seq), certificates, additional attributes
        byte[] digestItem = lenPrefixed32(concat(
            u32le(0x0103), // SIGNATURE_RSA_PKCS1_V1_5_WITH_SHA256
            lenPrefixed32(rootDigest)
        ));
        byte[] digests = lenPrefixed32(digestItem);
        byte[] certs = lenPrefixed32(lenPrefixed32(id.certDer));
        byte[] attrs = lenPrefixed32(new byte[0]);
        byte[] signedData = concat(digests, certs, attrs);

        Signature sig = Signature.getInstance("SHA256withRSA");
        sig.initSign(id.privateKey);
        sig.update(signedData);
        byte[] sigBytes = sig.sign();

        byte[] sigItem = lenPrefixed32(concat(u32le(0x0103), lenPrefixed32(sigBytes)));
        byte[] signatures = lenPrefixed32(sigItem);
        byte[] pubKey = lenPrefixed32(id.cert.getPublicKey().getEncoded());

        byte[] signer = lenPrefixed32(concat(lenPrefixed32(signedData), signatures, pubKey));
        byte[] v2BlockValue = lenPrefixed32(signer);

        // APK Signing Block: uint64 size-of-block-excluding-first-8, pairs..., uint64 size-of-block, magic "APK Sig Block 42"
        byte[] pairValue = concat(u32le(0x7109871A), v2BlockValue);
        byte[] pair = concat(u64le(pairValue.length), pairValue);
        int blockSizeExclFirst8 = pair.length + 8 + 16;
        // Pad to 4096 boundary if desired; here optional Android Verity padding pair keeps alignment clean
        byte[] sigBlock = concat(
            u64le(blockSizeExclFirst8),
            pair,
            u64le(blockSizeExclFirst8),
            "APK Sig Block 42".getBytes(StandardCharsets.US_ASCII)
        );

        ByteArrayOutputStream out = new ByteArrayOutputStream(zip.length + sigBlock.length);
        out.write(zip, 0, cdOff);
        out.write(sigBlock);
        out.write(zip, cdOff, cdSize);
        byte[] newEocd = Arrays.copyOfRange(zip, eocdOff, zip.length);
        ByteBuffer.wrap(newEocd).order(ByteOrder.LITTLE_ENDIAN).putInt(16, cdOff + sigBlock.length);
        out.write(newEocd);
        return out.toByteArray();
    }

    private static byte[] computeApkV2ChunkedSha256(byte[] sec1, byte[] sec3, byte[] sec4) throws Exception {
        final int CHUNK = 1024 * 1024;
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        List<byte[]> chunkDigests = new ArrayList<>();
        for (byte[] sec : new byte[][]{sec1, sec3, sec4}) {
            if (sec.length == 0) {
                md.reset();
                md.update((byte) 0xA5);
                md.update(u32le(0));
                chunkDigests.add(md.digest());
                continue;
            }
            for (int off = 0; off < sec.length; off += CHUNK) {
                int len = Math.min(CHUNK, sec.length - off);
                md.reset();
                md.update((byte) 0xA5);
                md.update(u32le(len));
                md.update(sec, off, len);
                chunkDigests.add(md.digest());
            }
        }
        md.reset();
        md.update((byte) 0x5A);
        md.update(u32le(chunkDigests.size()));
        for (byte[] d : chunkDigests) md.update(d);
        return md.digest();
    }

    // -------------------------------------------------------------------------
    // DER & Binary Helpers
    // -------------------------------------------------------------------------
    private static byte[] u32le(int v) {
        return ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array();
    }

    private static byte[] u64le(long v) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).array();
    }

    private static byte[] lenPrefixed32(byte[] data) {
        return concat(u32le(data.length), data);
    }

    private static byte[] concat(byte[]... parts) {
        int total = 0;
        for (byte[] p : parts) total += p.length;
        byte[] out = new byte[total];
        int pos = 0;
        for (byte[] p : parts) {
            System.arraycopy(p, 0, out, pos, p.length);
            pos += p.length;
        }
        return out;
    }

    private static byte[] derTag(int tag, byte[] content) {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        bos.write(tag);
        int len = content.length;
        if (len < 128) {
            bos.write(len);
        } else if (len <= 0xFF) {
            bos.write(0x81);
            bos.write(len);
        } else if (len <= 0xFFFF) {
            bos.write(0x82);
            bos.write((len >> 8) & 0xFF);
            bos.write(len & 0xFF);
        } else {
            bos.write(0x83);
            bos.write((len >> 16) & 0xFF);
            bos.write((len >> 8) & 0xFF);
            bos.write(len & 0xFF);
        }
        bos.write(content, 0, content.length);
        return bos.toByteArray();
    }

    private static byte[] derSeq(byte[]... items) {
        return derTag(0x30, concat(items));
    }

    private static byte[] derSet(byte[]... items) {
        return derTag(0x31, concat(items));
    }

    private static byte[] derInteger(byte[] bigEndianUnsignedOrSigned) {
        if (bigEndianUnsignedOrSigned.length == 0) return derTag(0x02, new byte[]{0});
        if ((bigEndianUnsignedOrSigned[0] & 0x80) != 0) {
            byte[] padded = new byte[bigEndianUnsignedOrSigned.length + 1];
            System.arraycopy(bigEndianUnsignedOrSigned, 0, padded, 1, bigEndianUnsignedOrSigned.length);
            return derTag(0x02, padded);
        }
        return derTag(0x02, bigEndianUnsignedOrSigned);
    }

    private static byte[] derOctetString(byte[] data) {
        return derTag(0x04, data);
    }

    private static byte[] derNull() {
        return new byte[]{0x05, 0x00};
    }

    private static byte[] derBitString(byte[] bits) {
        byte[] content = new byte[bits.length + 1];
        content[0] = 0;
        System.arraycopy(bits, 0, content, 1, bits.length);
        return derTag(0x03, content);
    }

    private static byte[] derUtf8(String s) {
        return derTag(0x0C, s.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] derUtcTime(String z) {
        return derTag(0x17, z.getBytes(StandardCharsets.US_ASCII));
    }

    private static byte[] derOid(int[] arcs) {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        bos.write(arcs[0] * 40 + arcs[1]);
        for (int i = 2; i < arcs.length; i++) {
            int v = arcs[i];
            if (v < 128) {
                bos.write(v);
            } else {
                byte[] tmp = new byte[5];
                int k = 0;
                boolean first = true;
                while (v > 0) {
                    int b = v & 0x7F;
                    if (!first) b |= 0x80;
                    first = false;
                    tmp[k++] = (byte) b;
                    v >>>= 7;
                }
                for (int j = k - 1; j >= 0; j--) bos.write(tmp[j]);
            }
        }
        return derTag(0x06, bos.toByteArray());
    }

    // -------------------------------------------------------------------------
    // File I/O Helpers
    // -------------------------------------------------------------------------
    private static void collectFiles(File dir, List<File> out) {
        File[] list = dir.listFiles();
        if (list == null) return;
        Arrays.sort(list, (a, b) -> a.getName().compareTo(b.getName()));
        for (File f : list) {
            if (f.isDirectory()) collectFiles(f, out);
            else if (f.isFile()) out.add(f);
        }
    }

    private static String relativePath(File root, File f) {
        String r = root.getAbsolutePath();
        String p = f.getAbsolutePath();
        if (p.startsWith(r)) {
            p = p.substring(r.length());
            while (p.startsWith("/") || p.startsWith("\\")) p = p.substring(1);
        }
        return p.replace('\\', '/');
    }

    private static String readUtf8(File f) throws IOException {
        return new String(readFileBytes(f), StandardCharsets.UTF_8);
    }

    private static byte[] readFileBytes(File f) throws IOException {
        try (FileInputStream fis = new FileInputStream(f)) {
            return readAll(fis);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        int n;
        while ((n = in.read(buf)) >= 0) bos.write(buf, 0, n);
        return bos.toByteArray();
    }
}
