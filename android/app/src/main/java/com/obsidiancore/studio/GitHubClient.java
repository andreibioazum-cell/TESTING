package com.obsidiancore.studio;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONArray;
import org.json.JSONObject;

final class GitHubClient {
    private static final String API_BASE = "https://api.github.com";

    static final class UserInfo {
        final String login;
        final String name;
        UserInfo(String login, String name) {
            this.login = login;
            this.name = name;
        }
    }

    static final class RepoInfo {
        final String owner;
        final String name;
        final String fullName;
        final String defaultBranch;
        final String language;
        final String description;
        final boolean isPrivate;

        RepoInfo(String owner, String name, String fullName, String defaultBranch,
                 String language, String description, boolean isPrivate) {
            this.owner = owner;
            this.name = name;
            this.fullName = fullName;
            this.defaultBranch = defaultBranch;
            this.language = language;
            this.description = description;
            this.isPrivate = isPrivate;
        }
    }

    static final class WorkflowInfo {
        final long id;
        final String name;
        final String path;
        final String state;

        WorkflowInfo(long id, String name, String path, String state) {
            this.id = id;
            this.name = name;
            this.path = path;
            this.state = state;
        }
    }

    static final class RunInfo {
        final long id;
        final String name;
        final String status;
        final String conclusion;
        final String branch;
        final String event;
        final String createdAt;

        RunInfo(long id, String name, String status, String conclusion,
                String branch, String event, String createdAt) {
            this.id = id;
            this.name = name;
            this.status = status;
            this.conclusion = conclusion;
            this.branch = branch;
            this.event = event;
            this.createdAt = createdAt;
        }
    }

    static UserInfo verifyToken(String token) throws Exception {
        JSONObject obj = new JSONObject(requestJson(token, "GET", "/user", null));
        return new UserInfo(obj.optString("login", ""), obj.optString("name", ""));
    }

    static List<RepoInfo> listRepositories(String token) throws Exception {
        String raw = requestJson(token, "GET",
            "/user/repos?sort=updated&per_page=100&affiliation=owner,collaborator,organization_member", null);
        JSONArray arr = new JSONArray(raw);
        List<RepoInfo> list = new ArrayList<>(arr.length());
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.getJSONObject(i);
            JSONObject ownerObj = o.optJSONObject("owner");
            String owner = ownerObj != null ? ownerObj.optString("login", "") : "";
            list.add(new RepoInfo(
                owner,
                o.optString("name", ""),
                o.optString("full_name", ""),
                o.optString("default_branch", "main"),
                o.optString("language", ""),
                o.optString("description", ""),
                o.optBoolean("private", false)
            ));
        }
        return list;
    }

    static RepoInfo getRepository(String token, String owner, String repo) throws Exception {
        JSONObject o = new JSONObject(requestJson(token, "GET", "/repos/" + enc(owner) + "/" + enc(repo), null));
        JSONObject ownerObj = o.optJSONObject("owner");
        String own = ownerObj != null ? ownerObj.optString("login", owner) : owner;
        return new RepoInfo(
            own,
            o.optString("name", repo),
            o.optString("full_name", own + "/" + repo),
            o.optString("default_branch", "main"),
            o.optString("language", ""),
            o.optString("description", ""),
            o.optBoolean("private", false)
        );
    }

    static String cloneOrSyncRepo(String token, String owner, String repo, String branch, RepoWorkspace ws) throws Exception {
        if (branch == null || branch.trim().isEmpty()) {
            branch = getRepository(token, owner, repo).defaultBranch;
        }
        String refPath = "/repos/" + enc(owner) + "/" + enc(repo) + "/git/ref/heads/" + encPath(branch);
        JSONObject refObj = new JSONObject(requestJson(token, "GET", refPath, null));
        String headSha = refObj.getJSONObject("object").getString("sha");

        String zipUrl = API_BASE + "/repos/" + enc(owner) + "/" + enc(repo) + "/zipball/" + encPath(branch);
        byte[] zipBytes = downloadFollowRedirects(zipUrl, token);

        deleteDirContents(ws.repoRoot);
        if (!ws.repoRoot.exists()) ws.repoRoot.mkdirs();

        int fileCount = 0;
        String rootCanon = ws.repoRoot.getCanonicalPath();
        try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(new java.io.ByteArrayInputStream(zipBytes)))) {
            ZipEntry entry;
            byte[] buf = new byte[16384];
            while ((entry = zis.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                int firstSlash = name.indexOf('/');
                if (firstSlash < 0 || firstSlash == name.length() - 1) continue;
                String rel = name.substring(firstSlash + 1);
                if (rel.isEmpty() || rel.startsWith(".git/") || rel.equals(".obsidian_ai_meta.json")) continue;

                File out = new File(ws.repoRoot, rel);
                String outCanon = out.getCanonicalPath();
                if (!outCanon.startsWith(rootCanon + File.separator)) {
                    throw new IOException("Blocked unsafe zip path: " + rel);
                }
                if (entry.isDirectory()) {
                    out.mkdirs();
                } else {
                    File parent = out.getParentFile();
                    if (parent != null && !parent.exists()) parent.mkdirs();
                    try (FileOutputStream fos = new FileOutputStream(out)) {
                        int n;
                        while ((n = zis.read(buf)) >= 0) fos.write(buf, 0, n);
                    }
                    fileCount++;
                }
            }
        }
        ws.saveBaseline(owner, repo, branch, headSha);
        return "Synced " + owner + "/" + repo + " @ " + branch + " (" + headSha.substring(0, Math.min(7, headSha.length())) + ", " + fileCount + " files)";
    }

    static String pushChanges(String token, String owner, String repo, String branch,
                              String commitMessage, RepoWorkspace ws) throws Exception {
        List<RepoWorkspace.ChangedFile> changed = ws.computeChangedFiles();
        if (changed.isEmpty()) return "Nothing to push: working tree is clean.";

        RepoWorkspace.Meta meta = ws.loadMeta();
        String targetBranch = (branch != null && !branch.trim().isEmpty()) ? branch.trim() : meta.branch;
        if (targetBranch.isEmpty()) targetBranch = "main";
        String msg = (commitMessage != null && !commitMessage.trim().isEmpty())
            ? commitMessage.trim() : "Obsidian AI: update " + changed.size() + " file(s)";

        String refPath = "/repos/" + enc(owner) + "/" + enc(repo) + "/git/ref/heads/" + encPath(targetBranch);
        JSONObject refObj = new JSONObject(requestJson(token, "GET", refPath, null));
        String parentSha = refObj.getJSONObject("object").getString("sha");

        JSONObject parentCommit = new JSONObject(requestJson(
            token, "GET", "/repos/" + enc(owner) + "/" + enc(repo) + "/git/commits/" + parentSha, null));
        String baseTreeSha = parentCommit.getJSONObject("tree").getString("sha");

        JSONArray treeArr = new JSONArray();
        Base64.Encoder b64 = Base64.getEncoder();
        for (RepoWorkspace.ChangedFile cf : changed) {
            JSONObject blobReq = new JSONObject();
            blobReq.put("content", b64.encodeToString(cf.bytes));
            blobReq.put("encoding", "base64");
            JSONObject blobResp = new JSONObject(requestJson(
                token, "POST", "/repos/" + enc(owner) + "/" + enc(repo) + "/git/blobs", blobReq.toString()));
            JSONObject item = new JSONObject();
            item.put("path", cf.relPath);
            item.put("mode", "100644");
            item.put("type", "blob");
            item.put("sha", blobResp.getString("sha"));
            treeArr.put(item);
        }

        JSONObject treeReq = new JSONObject();
        treeReq.put("base_tree", baseTreeSha);
        treeReq.put("tree", treeArr);
        JSONObject treeResp = new JSONObject(requestJson(
            token, "POST", "/repos/" + enc(owner) + "/" + enc(repo) + "/git/trees", treeReq.toString()));

        JSONObject commitReq = new JSONObject();
        commitReq.put("message", msg);
        commitReq.put("tree", treeResp.getString("sha"));
        commitReq.put("parents", new JSONArray().put(parentSha));
        JSONObject commitResp = new JSONObject(requestJson(
            token, "POST", "/repos/" + enc(owner) + "/" + enc(repo) + "/git/commits", commitReq.toString()));
        String newCommitSha = commitResp.getString("sha");

        JSONObject patchRef = new JSONObject();
        patchRef.put("sha", newCommitSha);
        patchRef.put("force", false);
        requestJson(token, "PATCH",
            "/repos/" + enc(owner) + "/" + enc(repo) + "/git/refs/heads/" + encPath(targetBranch),
            patchRef.toString());

        ws.saveBaseline(owner, repo, targetBranch, newCommitSha);
        return "Pushed " + changed.size() + " file(s) to " + owner + "/" + repo + " @ " + targetBranch
            + " (" + newCommitSha.substring(0, Math.min(7, newCommitSha.length())) + ")";
    }

    static String createBranch(String token, String owner, String repo, String fromBranch, String newBranch) throws Exception {
        String refPath = "/repos/" + enc(owner) + "/" + enc(repo) + "/git/ref/heads/" + encPath(fromBranch);
        JSONObject refObj = new JSONObject(requestJson(token, "GET", refPath, null));
        String sha = refObj.getJSONObject("object").getString("sha");

        JSONObject body = new JSONObject();
        body.put("ref", "refs/heads/" + newBranch);
        body.put("sha", sha);
        requestJson(token, "POST", "/repos/" + enc(owner) + "/" + enc(repo) + "/git/refs", body.toString());
        return "Created branch " + newBranch + " from " + fromBranch;
    }

    static String createPullRequest(String token, String owner, String repo, String title,
                                    String body, String head, String base) throws Exception {
        JSONObject req = new JSONObject();
        req.put("title", title);
        req.put("body", body == null ? "" : body);
        req.put("head", head);
        req.put("base", base);
        JSONObject resp = new JSONObject(requestJson(
            token, "POST", "/repos/" + enc(owner) + "/" + enc(repo) + "/pulls", req.toString()));
        return "PR #" + resp.optInt("number") + ": " + resp.optString("html_url", "");
    }

    static List<WorkflowInfo> listWorkflows(String token, String owner, String repo) throws Exception {
        JSONObject obj = new JSONObject(requestJson(
            token, "GET", "/repos/" + enc(owner) + "/" + enc(repo) + "/actions/workflows", null));
        JSONArray arr = obj.optJSONArray("workflows");
        List<WorkflowInfo> list = new ArrayList<>();
        if (arr != null) {
            for (int i = 0; i < arr.length(); i++) {
                JSONObject w = arr.getJSONObject(i);
                list.add(new WorkflowInfo(
                    w.optLong("id"),
                    w.optString("name", ""),
                    w.optString("path", ""),
                    w.optString("state", "")
                ));
            }
        }
        return list;
    }

    static String dispatchWorkflow(String token, String owner, String repo,
                                   String workflowIdOrFile, String ref, String inputsJson) throws Exception {
        JSONObject body = new JSONObject();
        body.put("ref", ref);
        if (inputsJson != null && !inputsJson.trim().isEmpty()) {
            body.put("inputs", new JSONObject(inputsJson));
        }
        String path = "/repos/" + enc(owner) + "/" + enc(repo) + "/actions/workflows/"
            + enc(workflowIdOrFile) + "/dispatches";
        requestJson(token, "POST", path, body.toString());
        return "Dispatched workflow '" + workflowIdOrFile + "' on ref '" + ref + "'";
    }

    static List<RunInfo> listWorkflowRuns(String token, String owner, String repo) throws Exception {
        JSONObject obj = new JSONObject(requestJson(
            token, "GET", "/repos/" + enc(owner) + "/" + enc(repo) + "/actions/runs?per_page=15", null));
        JSONArray arr = obj.optJSONArray("workflow_runs");
        List<RunInfo> list = new ArrayList<>();
        if (arr != null) {
            for (int i = 0; i < arr.length(); i++) {
                JSONObject r = arr.getJSONObject(i);
                list.add(new RunInfo(
                    r.optLong("id"),
                    r.optString("name", ""),
                    r.optString("status", ""),
                    r.optString("conclusion", ""),
                    r.optString("head_branch", ""),
                    r.optString("event", ""),
                    r.optString("created_at", "")
                ));
            }
        }
        return list;
    }

    static String getWorkflowRunJobs(String token, String owner, String repo, long runId) throws Exception {
        JSONObject obj = new JSONObject(requestJson(
            token, "GET", "/repos/" + enc(owner) + "/" + enc(repo) + "/actions/runs/" + runId + "/jobs", null));
        JSONArray jobs = obj.optJSONArray("jobs");
        if (jobs == null || jobs.length() == 0) return "No jobs found for run #" + runId;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < jobs.length(); i++) {
            JSONObject j = jobs.getJSONObject(i);
            sb.append("Job: ").append(j.optString("name"))
              .append(" [").append(j.optString("status"))
              .append(j.optString("conclusion", "").isEmpty() ? "" : " / " + j.optString("conclusion"))
              .append("]\n");
            JSONArray steps = j.optJSONArray("steps");
            if (steps != null) {
                for (int s = 0; s < steps.length(); s++) {
                    JSONObject st = steps.getJSONObject(s);
                    sb.append("  ").append(st.optInt("number")).append(". ")
                      .append(st.optString("name")).append(" -> ")
                      .append(st.optString("conclusion", st.optString("status", "")))
                      .append('\n');
                }
            }
        }
        return sb.toString();
    }

    static String requestJson(String token, String method, String apiPath, String jsonBody) throws Exception {
        SafetyGuard.CheckResult guard = SafetyGuard.checkGitHubApiAction(method, apiPath);
        if (!guard.allowed) {
            throw new IOException("BLOCKED_BY_SAFETY_GUARD: " + guard.reason);
        }
        if (token == null || token.trim().isEmpty()) {
            throw new IOException("GitHub token is empty. Set your token in the Repos tab.");
        }
        URL url = new URL(API_BASE + apiPath);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        try {
            conn.setRequestMethod(method);
        } catch (java.net.ProtocolException pe) {
            if ("PATCH".equals(method)) {
                conn.setRequestMethod("POST");
                conn.setRequestProperty("X-HTTP-Method-Override", "PATCH");
            } else {
                throw pe;
            }
        }
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(30000);
        conn.setRequestProperty("Accept", "application/vnd.github+json");
        conn.setRequestProperty("Authorization", "Bearer " + token.trim());
        conn.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
        conn.setRequestProperty("User-Agent", "ObsidianAI-Android/2.0");
        if (jsonBody != null) {
            conn.setDoOutput(true);
            conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            try (OutputStream os = conn.getOutputStream()) {
                os.write(jsonBody.getBytes(StandardCharsets.UTF_8));
            }
        }
        int code = conn.getResponseCode();
        InputStream stream = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
        String resp = stream == null ? "" : readAllUtf8(stream);
        conn.disconnect();
        if (code >= 400) {
            throw new IOException("GitHub API HTTP " + code + " (" + apiPath + "): " + resp);
        }
        return resp;
    }

    private static byte[] downloadFollowRedirects(String startUrl, String token) throws Exception {
        String current = startUrl;
        for (int hop = 0; hop < 5; hop++) {
            HttpURLConnection conn = (HttpURLConnection) new URL(current).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(60000);
            if (current.startsWith(API_BASE) && token != null && !token.trim().isEmpty()) {
                conn.setRequestProperty("Authorization", "Bearer " + token.trim());
                conn.setRequestProperty("Accept", "application/vnd.github+json");
            }
            conn.setRequestProperty("User-Agent", "ObsidianAI-Android/2.0");
            int code = conn.getResponseCode();
            if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                String loc = conn.getHeaderField("Location");
                conn.disconnect();
                if (loc == null || loc.isEmpty()) throw new IOException("Redirect without Location");
                current = loc;
                continue;
            }
            if (code >= 400) {
                String err = conn.getErrorStream() != null ? readAllUtf8(conn.getErrorStream()) : "";
                conn.disconnect();
                throw new IOException("Download HTTP " + code + ": " + err);
            }
            try (InputStream in = conn.getInputStream();
                 ByteArrayOutputStream bos = new ByteArrayOutputStream()) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) >= 0) bos.write(buf, 0, n);
                return bos.toByteArray();
            } finally {
                conn.disconnect();
            }
        }
        throw new IOException("Too many redirects");
    }

    private static void deleteDirContents(File dir) {
        if (dir == null || !dir.exists()) return;
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (f.isDirectory()) deleteDirContents(f);
            f.delete();
        }
    }

    private static String enc(String s) throws Exception {
        return URLEncoder.encode(s, "UTF-8").replace("+", "%20");
    }

    private static String encPath(String path) throws Exception {
        String[] parts = path.split("/");
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < parts.length; i++) {
            if (i > 0) sb.append('/');
            sb.append(enc(parts[i]));
        }
        return sb.toString();
    }

    private static String readAllUtf8(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) >= 0) bos.write(buf, 0, n);
        return bos.toString(StandardCharsets.UTF_8.name());
    }
}
