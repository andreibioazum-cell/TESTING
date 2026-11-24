package com.obsidiancore.studio;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONObject;

final class AgentRuntime {
    interface Listener {
        void onStatus(String status);
        void onStreamToken(String token);
        void onCodeMessage(String title, String lang, String code);
        void onBlocked(String toolName, String reason);
        void onAssistantMessage(String message);
        void onError(String error);
        void onRepoChanged();
    }

    static final class ChatMessage {
        final String role;
        final String content;
        ChatMessage(String role, String content) {
            this.role = role;
            this.content = content;
        }
    }

    static final class ParsedTurn {
        final String thought;
        final String toolName;
        final JSONObject toolArgs;
        final String visibleText;

        ParsedTurn(String thought, String toolName, JSONObject toolArgs, String visibleText) {
            this.thought = thought;
            this.toolName = toolName;
            this.toolArgs = toolArgs;
            this.visibleText = visibleText;
        }
    }

    private static final Pattern THINK_PATTERN = Pattern.compile("<think>([\\s\\S]*?)</think>", Pattern.CASE_INSENSITIVE);
    private static final Pattern TOOL_TAG_PATTERN = Pattern.compile("<tool_call>\\s*(\\{[\\s\\S]*?\\})\\s*</tool_call>", Pattern.CASE_INSENSITIVE);
    private static final Pattern JSON_FENCE_PATTERN = Pattern.compile("```(?:json)?\\s*(\\{[\\s\\S]*?\"name\"\\s*:[\\s\\S]*?\\})\\s*```", Pattern.CASE_INSENSITIVE);

    private final List<ChatMessage> history = new ArrayList<>();
    private final AtomicBoolean aborted = new AtomicBoolean(false);

    void clearHistory() {
        history.clear();
    }

    void abort() {
        aborted.set(true);
        LlamaEngine.nativeStopGeneration();
    }

    static String buildSystemPrompt(RepoWorkspace ws, String tokenPresent) {
        String repoDesc = "(no repository synced yet)";
        if (ws != null) {
            try {
                RepoWorkspace.Meta m = ws.loadMeta();
                if (!m.owner.isEmpty() && !m.repo.isEmpty()) {
                    repoDesc = m.owner + "/" + m.repo + " (" + m.branch + ")";
                }
            } catch (Exception ignored) {
            }
        }
        return "You are Obsidian AI on Android. Repo: " + repoDesc + " (token: " + tokenPresent + ").\n"
            + "RULES: No <think> tags or long monologues. Act autonomously and complete everything yourself (read, edit, test, push, actions) without asking the user to click buttons. "
            + "To modify an existing file, call read_file first then edit_file (never rewrite from scratch; use write_new_file only for new files). "
            + "Wrap any code in your messages with ```lang ... ``` blocks. Never delete the repo, .git, or main/master.\n"
            + "Tool syntax (one per turn):\n"
            + "<tool_call>{\"name\":\"TOOL\",\"arguments\":{...}}</tool_call>\n"
            + "Tools: list_files(dir,max_depth), read_file(path,start_line,end_line), grep_search(query,path), "
            + "edit_file(path,old_text,new_text), write_new_file(path,content), run_bash(command), "
            + "git_status_diff(), github_push_commit(message,branch), github_list_workflows(), "
            + "github_run_action(workflow,ref,inputs), github_action_status(run_id), "
            + "github_create_branch(from_branch,new_branch), github_create_pr(title,body,head,base).";
    }

    static ParsedTurn parseResponse(String raw) {
        if (raw == null) return new ParsedTurn("", null, null, "");
        String work = raw.trim();

        StringBuilder thought = new StringBuilder();
        Matcher tm = THINK_PATTERN.matcher(work);
        while (tm.find()) {
            String t = tm.group(1).trim();
            if (!t.isEmpty()) {
                if (thought.length() > 0) thought.append('\n');
                thought.append(t);
            }
        }
        work = tm.replaceAll("").trim();

        String toolName = null;
        JSONObject toolArgs = null;

        Matcher m = TOOL_TAG_PATTERN.matcher(work);
        if (!m.find()) {
            m = JSON_FENCE_PATTERN.matcher(work);
            if (!m.find()) m = null;
        }
        if (m != null) {
            String jsonStr = m.group(1);
            try {
                JSONObject obj = new JSONObject(jsonStr);
                String n = obj.optString("name", obj.optString("tool", ""));
                if (!n.isEmpty()) {
                    toolName = n;
                    JSONObject args = obj.optJSONObject("arguments");
                    if (args == null) args = obj.optJSONObject("args");
                    toolArgs = args != null ? args : new JSONObject();
                    work = "";
                }
            } catch (Exception ignored) {
            }
        }

        int openThink = work.indexOf("<think>");
        if (openThink >= 0) {
            work = work.substring(0, openThink).trim();
        }

        return new ParsedTurn(thought.toString().trim(), toolName, toolArgs, work);
    }

    void runTurn(String userMessage, String githubToken, RepoWorkspace ws,
                 String activeOwner, String activeRepo, String activeBranch,
                 int maxSteps, float temperature, int coolDelayMs, boolean autoPush, Listener listener) {
        aborted.set(false);
        if (!LlamaEngine.nativeIsLoaded()) {
            listener.onError("Select and load a model in the Models tab first.");
            return;
        }

        // Auto-sync selected repository if not synced locally yet
        boolean hasToken = githubToken != null && !githubToken.trim().isEmpty();
        if (ws != null && !ws.metaFile().isFile() && hasToken
            && activeOwner != null && !activeOwner.isEmpty()
            && activeRepo != null && !activeRepo.isEmpty()) {
            try {
                listener.onStatus("Syncing " + activeOwner + "/" + activeRepo + "...");
                String syncMsg = GitHubClient.cloneOrSyncRepo(githubToken, activeOwner, activeRepo, activeBranch, ws);
                listener.onStatus(syncMsg);
                listener.onRepoChanged();
            } catch (Exception e) {
                listener.onStatus("Sync skipped: " + e.getMessage());
            }
        }

        history.add(new ChatMessage("user", userMessage));
        trimHistory();

        boolean anyFileMutated = false;
        boolean explicitPushed = false;

        for (int step = 0; step < Math.max(1, maxSteps); step++) {
            if (aborted.get()) {
                listener.onError("Stopped.");
                return;
            }
            String sysPrompt = buildSystemPrompt(ws, hasToken ? "yes" : "no");
            List<ChatMessage> msgs = new ArrayList<>(history.size() + 1);
            msgs.add(new ChatMessage("system", sysPrompt));
            msgs.addAll(history);

            String[] roles = new String[msgs.size()];
            String[] contents = new String[msgs.size()];
            for (int i = 0; i < msgs.size(); i++) {
                roles[i] = msgs.get(i).role;
                contents[i] = msgs.get(i).content;
            }

            String raw = LlamaEngine.nativeGenerate(roles, contents, 320, temperature, coolDelayMs,
                new LlamaEngine.TokenCallback() {
                    @Override public void onToken(String tokenPiece) {
                        listener.onStreamToken(tokenPiece);
                    }
                    @Override public void onPrefillProgress(int done, int total) {
                        int pct = total > 0 ? Math.min(100, (done * 100) / total) : 100;
                        listener.onStatus("Processing " + pct + "%...");
                    }
                });
            if (raw == null || raw.startsWith("ERROR:")) {
                listener.onError(raw != null ? raw : "Generation error");
                return;
            }
            if (aborted.get()) {
                listener.onError("Stopped.");
                return;
            }

            ParsedTurn parsed = parseResponse(raw);
            if (parsed.toolName != null) {
                history.add(new ChatMessage("assistant", raw));
                JSONObject args = parsed.toolArgs != null ? parsed.toolArgs : new JSONObject();

                emitToolStatus(parsed.toolName, args, listener);
                ToolExecResult res = executeTool(parsed.toolName, args, githubToken, ws);

                if (res.blocked) {
                    listener.onBlocked(parsed.toolName, res.output);
                } else if ("edit_file".equals(parsed.toolName) && res.isDiff) {
                    String path = args.optString("path", "file");
                    listener.onCodeMessage(path, guessLang(path), res.output);
                } else if ("write_new_file".equals(parsed.toolName) && res.repoMutated) {
                    String path = args.optString("path", "file");
                    listener.onCodeMessage(path + " (new)", guessLang(path), args.optString("content", ""));
                } else if ("run_bash".equals(parsed.toolName)) {
                    String cmd = args.optString("command", "");
                    listener.onCodeMessage("$ " + cmd, "bash", res.output);
                } else if ("github_push_commit".equals(parsed.toolName) && res.repoMutated) {
                    explicitPushed = true;
                    listener.onStatus(res.output);
                } else if (parsed.toolName.startsWith("github_")) {
                    listener.onStatus(res.output);
                }

                if (res.repoMutated) {
                    if (!"github_push_commit".equals(parsed.toolName)) {
                        anyFileMutated = true;
                    }
                    listener.onRepoChanged();
                }

                String obs = res.output;
                if (obs.length() > 3600) {
                    obs = obs.substring(0, 3400) + "\n... (truncated)";
                }
                history.add(new ChatMessage("user", "<tool_result name=\"" + parsed.toolName + "\">\n" + obs + "\n</tool_result>"));
                trimHistory();
                continue;
            }

            // Auto commit & push if agent modified files and did not already push manually
            if (autoPush && anyFileMutated && !explicitPushed && hasToken && ws != null) {
                try {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    if (!m.owner.isEmpty() && !m.repo.isEmpty() && !ws.computeChangedFiles().isEmpty()) {
                        listener.onStatus("Auto-pushing changes to " + m.owner + "/" + m.repo + "...");
                        String shortMsg = userMessage.replace('\n', ' ').trim();
                        if (shortMsg.length() > 60) shortMsg = shortMsg.substring(0, 60);
                        String pushOut = GitHubClient.pushChanges(
                            githubToken, m.owner, m.repo, m.branch, "AI: " + shortMsg, ws);
                        listener.onStatus(pushOut);
                        listener.onRepoChanged();
                    }
                } catch (Exception e) {
                    listener.onStatus("Auto-push failed: " + e.getMessage());
                }
            }

            String finalMsg = parsed.visibleText.isEmpty() ? raw : parsed.visibleText;
            history.add(new ChatMessage("assistant", finalMsg));
            listener.onAssistantMessage(finalMsg);
            return;
        }
        listener.onError("Reached max steps (" + maxSteps + ").");
    }

    private static void emitToolStatus(String name, JSONObject args, Listener listener) {
        switch (name) {
            case "read_file":
                listener.onStatus("Reading " + args.optString("path", "file") + "...");
                break;
            case "list_files":
                listener.onStatus("Listing " + args.optString("dir", ".") + "...");
                break;
            case "grep_search":
                listener.onStatus("Searching \"" + args.optString("query", "") + "\"...");
                break;
            case "github_run_action":
                listener.onStatus("Starting GitHub Action " + args.optString("workflow", "") + "...");
                break;
            case "github_push_commit":
                listener.onStatus("Pushing to GitHub...");
                break;
            default:
                break;
        }
    }

    static String guessLang(String path) {
        if (path == null) return "code";
        String lower = path.toLowerCase(Locale.US);
        int dot = lower.lastIndexOf('.');
        if (dot < 0 || dot == lower.length() - 1) return "code";
        return lower.substring(dot + 1);
    }

    static final class ToolExecResult {
        final String output;
        final boolean isDiff;
        final boolean blocked;
        final boolean repoMutated;

        ToolExecResult(String output, boolean isDiff, boolean blocked, boolean repoMutated) {
            this.output = output;
            this.isDiff = isDiff;
            this.blocked = blocked;
            this.repoMutated = repoMutated;
        }
    }

    static ToolExecResult executeTool(String name, JSONObject args, String token, RepoWorkspace ws) {
        if (args == null) args = new JSONObject();
        try {
            if (ws == null) {
                return new ToolExecResult("ERROR: No repository selected. Pick a repo in the Repos tab.", false, false, false);
            }
            switch (name) {
                case "list_files":
                    return new ToolExecResult(ws.listFiles(args.optString("dir", "."), args.optInt("max_depth", 3)), false, false, false);
                case "read_file":
                    return new ToolExecResult(ws.readFile(args.optString("path", ""), args.optInt("start_line", 1), args.optInt("end_line", 150)), false, false, false);
                case "grep_search":
                    return new ToolExecResult(ws.grepSearch(args.optString("query", ""), args.optString("path", ".")), false, false, false);
                case "edit_file": {
                    String out = ws.editFile(args.optString("path", ""), args.optString("old_text", ""), args.optString("new_text", ""));
                    boolean ok = out.startsWith("Edited ");
                    boolean blk = out.startsWith("BLOCKED_BY_SAFETY_GUARD");
                    return new ToolExecResult(out, ok, blk, ok);
                }
                case "write_new_file": {
                    String out = ws.writeNewFile(args.optString("path", ""), args.optString("content", ""));
                    boolean ok = out.startsWith("Created ");
                    boolean blk = out.startsWith("BLOCKED_BY_SAFETY_GUARD") || out.contains("Refusing to overwrite");
                    return new ToolExecResult(out, false, blk, ok);
                }
                case "run_bash": {
                    String out = ws.runBash(args.optString("command", ""));
                    boolean blk = out.startsWith("BLOCKED_BY_SAFETY_GUARD");
                    return new ToolExecResult(out, false, blk, !blk);
                }
                case "git_status_diff":
                    return new ToolExecResult(ws.statusSummary(), false, false, false);
                case "github_push_commit": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    String br = args.optString("branch", m.branch);
                    String out = GitHubClient.pushChanges(token, m.owner, m.repo, br, args.optString("message", ""), ws);
                    return new ToolExecResult(out, false, false, true);
                }
                case "github_list_workflows": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    List<GitHubClient.WorkflowInfo> wfs = GitHubClient.listWorkflows(token, m.owner, m.repo);
                    StringBuilder sb = new StringBuilder();
                    for (GitHubClient.WorkflowInfo w : wfs) {
                        sb.append(w.id).append(" | ").append(w.name).append(" | ").append(w.path)
                          .append(" [").append(w.state).append("]\n");
                    }
                    return new ToolExecResult(sb.length() == 0 ? "No workflows found." : sb.toString(), false, false, false);
                }
                case "github_run_action": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    String wf = args.optString("workflow", "");
                    String ref = args.optString("ref", m.branch);
                    JSONObject inputs = args.optJSONObject("inputs");
                    String out = GitHubClient.dispatchWorkflow(token, m.owner, m.repo, wf, ref, inputs != null ? inputs.toString() : "");
                    return new ToolExecResult(out, false, false, false);
                }
                case "github_action_status": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    long runId = args.optLong("run_id", 0L);
                    if (runId > 0) {
                        return new ToolExecResult(GitHubClient.getWorkflowRunJobs(token, m.owner, m.repo, runId), false, false, false);
                    }
                    List<GitHubClient.RunInfo> runs = GitHubClient.listWorkflowRuns(token, m.owner, m.repo);
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < Math.min(8, runs.size()); i++) {
                        GitHubClient.RunInfo r = runs.get(i);
                        sb.append('#').append(r.id).append(' ').append(r.name)
                          .append(" [").append(r.status).append('/').append(r.conclusion).append("] on ").append(r.branch).append('\n');
                    }
                    return new ToolExecResult(sb.length() == 0 ? "No workflow runs." : sb.toString(), false, false, false);
                }
                case "github_create_branch": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    String from = args.optString("from_branch", m.branch);
                    String nb = args.optString("new_branch", "");
                    return new ToolExecResult(GitHubClient.createBranch(token, m.owner, m.repo, from, nb), false, false, false);
                }
                case "github_create_pr": {
                    RepoWorkspace.Meta m = ws.loadMeta();
                    String out = GitHubClient.createPullRequest(
                        token, m.owner, m.repo,
                        args.optString("title", "Obsidian AI Update"),
                        args.optString("body", ""),
                        args.optString("head", m.branch),
                        args.optString("base", "main")
                    );
                    return new ToolExecResult(out, false, false, false);
                }
                case "delete_repository":
                case "github_delete_repo":
                    return new ToolExecResult("BLOCKED_BY_SAFETY_GUARD: Deleting repositories is strictly forbidden.", false, true, false);
                default:
                    return new ToolExecResult("ERROR: Unknown tool '" + name + "'.", false, false, false);
            }
        } catch (Exception e) {
            String msg = e.getMessage() != null ? e.getMessage() : e.toString();
            return new ToolExecResult("ERROR: " + msg, false, msg.contains("BLOCKED_BY_SAFETY_GUARD"), false);
        }
    }

    private void trimHistory() {
        while (history.size() > 20) {
            history.remove(0);
        }
    }
}
