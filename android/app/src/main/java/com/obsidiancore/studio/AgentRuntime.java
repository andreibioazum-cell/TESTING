package com.obsidiancore.studio;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONObject;

final class AgentRuntime {
    interface Listener {
        void onStreamToken(String token);
        void onThought(String thought);
        void onToolCall(String name, String argsSummary);
        void onToolResult(String name, String result, boolean isDiff, boolean blocked);
        void onAssistantMessage(String message);
        void onError(String error);
        void onRepoChanged();
    }

    static final class ChatMessage {
        final String role; // "system", "user", "assistant"
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
                    repoDesc = m.owner + "/" + m.repo + " (branch: " + m.branch + ")";
                }
            } catch (Exception ignored) {
            }
        }
        return "You are Obsidian AI, an autonomous software engineering agent running locally on Android.\n"
            + "Active repository: " + repoDesc + "\n"
            + "GitHub token configured: " + tokenPresent + "\n\n"
            + "RULES:\n"
            + "1. Think carefully step-by-step inside <think>...</think> before acting.\n"
            + "2. EDIT FILES INCREMENTALLY: Never rewrite an existing file from scratch! "
            + "Always inspect relevant lines with `read_file` first, then apply targeted changes with `edit_file(path, old_text, new_text)`. "
            + "Use `write_new_file` ONLY for brand-new files.\n"
            + "3. SAFETY: Never delete the repository, never delete .git or workspace root, never force-push or delete main/master, and never run destructive system commands.\n"
            + "4. To call a tool, output exactly ONE block per turn:\n"
            + "<tool_call>{\"name\":\"TOOL_NAME\",\"arguments\":{...}}</tool_call>\n\n"
            + "AVAILABLE TOOLS:\n"
            + "- list_files: {\"dir\": \".\", \"max_depth\": 3}\n"
            + "- read_file: {\"path\": \"relative/path\", \"start_line\": 1, \"end_line\": 120}\n"
            + "- grep_search: {\"query\": \"text\", \"path\": \".\"}\n"
            + "- edit_file: {\"path\": \"relative/path\", \"old_text\": \"exact lines\", \"new_text\": \"replacement lines\"}\n"
            + "- write_new_file: {\"path\": \"relative/new_file\", \"content\": \"file content\"}\n"
            + "- run_bash: {\"command\": \"shell command inside repo root\"}\n"
            + "- git_status_diff: {}\n"
            + "- github_push_commit: {\"message\": \"commit message\", \"branch\": \"optional branch\"}\n"
            + "- github_list_workflows: {}\n"
            + "- github_run_action: {\"workflow\": \"build.yml\", \"ref\": \"branch\", \"inputs\": {}}\n"
            + "- github_action_status: {\"run_id\": 0}\n"
            + "- github_create_branch: {\"from_branch\": \"main\", \"new_branch\": \"feature\"}\n"
            + "- github_create_pr: {\"title\": \"...\", \"body\": \"...\", \"head\": \"...\", \"base\": \"main\"}\n\n"
            + "When the task is complete, explain what you did concisely without a <tool_call> block.";
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
                    String before = work.substring(0, m.start()).trim();
                    if (!before.isEmpty() && thought.length() == 0) {
                        thought.append(before);
                    }
                    work = "";
                }
            } catch (Exception ignored) {
            }
        }

        // Also handle unclosed <think> if model stopped early
        int openThink = work.indexOf("<think>");
        if (openThink >= 0) {
            String tail = work.substring(openThink + 7).trim();
            if (thought.length() == 0) thought.append(tail);
            work = work.substring(0, openThink).trim();
        }

        return new ParsedTurn(thought.toString().trim(), toolName, toolArgs, work);
    }

    void runTurn(String userMessage, String githubToken, RepoWorkspace ws,
                 int maxSteps, float temperature, Listener listener) {
        aborted.set(false);
        if (!LlamaEngine.nativeIsLoaded()) {
            listener.onError("No GGUF model loaded. Open the Models tab to download or pick a model and tap Load.");
            return;
        }
        history.add(new ChatMessage("user", userMessage));
        trimHistory();

        for (int step = 0; step < Math.max(1, maxSteps); step++) {
            if (aborted.get()) {
                listener.onError("Stopped by user.");
                return;
            }
            String sysPrompt = buildSystemPrompt(ws, (githubToken != null && !githubToken.trim().isEmpty()) ? "yes" : "no");
            List<ChatMessage> msgs = new ArrayList<>(history.size() + 1);
            msgs.add(new ChatMessage("system", sysPrompt));
            msgs.addAll(history);

            String[] roles = new String[msgs.size()];
            String[] contents = new String[msgs.size()];
            for (int i = 0; i < msgs.size(); i++) {
                roles[i] = msgs.get(i).role;
                contents[i] = msgs.get(i).content;
            }

            String raw = LlamaEngine.nativeGenerate(roles, contents, 1024, temperature, listener::onStreamToken);
            if (raw == null || raw.startsWith("ERROR:")) {
                listener.onError(raw != null ? raw : "Generation error");
                return;
            }
            if (aborted.get()) {
                listener.onError("Stopped by user.");
                return;
            }

            ParsedTurn parsed = parseResponse(raw);
            if (!parsed.thought.isEmpty()) {
                listener.onThought(parsed.thought);
            }

            if (parsed.toolName != null) {
                history.add(new ChatMessage("assistant", raw));
                String argsText = parsed.toolArgs != null ? parsed.toolArgs.toString() : "{}";
                listener.onToolCall(parsed.toolName, argsText);

                ToolExecResult res = executeTool(parsed.toolName, parsed.toolArgs, githubToken, ws);
                listener.onToolResult(parsed.toolName, res.output, res.isDiff, res.blocked);
                if (res.repoMutated) listener.onRepoChanged();

                String obs = res.output;
                if (obs.length() > 4000) {
                    obs = obs.substring(0, 3800) + "\n... (truncated)";
                }
                history.add(new ChatMessage("user", "<tool_result name=\"" + parsed.toolName + "\">\n" + obs + "\n</tool_result>"));
                trimHistory();
                continue;
            }

            String finalMsg = parsed.visibleText.isEmpty() ? raw : parsed.visibleText;
            history.add(new ChatMessage("assistant", finalMsg));
            listener.onAssistantMessage(finalMsg);
            return;
        }
        listener.onError("Reached max reasoning steps (" + maxSteps + "). Send a follow-up message to continue.");
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
                return new ToolExecResult("ERROR: No repository synced. Ask the user to sync a repo in the Repos tab.", false, false, false);
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
        while (history.size() > 24) {
            history.remove(0);
        }
    }
}
