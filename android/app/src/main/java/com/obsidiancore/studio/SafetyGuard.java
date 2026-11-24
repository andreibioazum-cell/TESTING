package com.obsidiancore.studio;

import java.io.File;
import java.io.IOException;
import java.util.Locale;
import java.util.regex.Pattern;

final class SafetyGuard {
    static final class CheckResult {
        final boolean allowed;
        final String reason;

        private CheckResult(boolean allowed, String reason) {
            this.allowed = allowed;
            this.reason = reason;
        }

        static CheckResult ok() { return new CheckResult(true, ""); }
        static CheckResult block(String reason) { return new CheckResult(false, reason); }
    }

    private static final Pattern DANGEROUS_RM = Pattern.compile(
        "(^|[;&|`(]\\s*|\\s)rm\\s+[^\\n]*(?:-[a-zA-Z]*[rf][a-zA-Z]*|--recursive|--force)\\b[^\\n]*(?:\\s+/|\\s+\\.|\\s+\\.\\.|\\s+\\*|\\s+~|\\s+\\.git|\\s+\\.obsidian_ai_meta\\.json)"
    );
    private static final Pattern PIPE_TO_SHELL = Pattern.compile(
        "(?:curl|wget|fetch)[^\\n|]*\\|\\s*(?:sh|bash|ash|zsh|su)\\b"
    );
    private static final Pattern FORCE_PUSH_PROTECTED = Pattern.compile(
        "git\\s+push[^\\n]*(?:--force|-f|--force-with-lease)[^\\n]*\\b(?:main|master)\\b"
    );
    private static final Pattern DELETE_REMOTE_BRANCH = Pattern.compile(
        "git\\s+push[^\\n]*(?:--delete\\s+(?:main|master)|:(?:main|master)(?:\\s|$))"
    );
    private static final Pattern DESTRUCTIVE_DISK = Pattern.compile(
        "(^|[;&|`(]\\s*|\\s)(?:mkfs(?:\\.[a-z0-9]+)?|fdisk|parted|wipefs|dd\\s+if=|reboot|shutdown|poweroff|halt|su\\b|sudo\\b)"
    );
    private static final Pattern REPO_DELETE_CMD = Pattern.compile(
        "(?:gh\\s+repo\\s+delete|api\\.github\\.com/repos/[^\\s]+.*(?:-X\\s*DELETE|--request\\s+DELETE))",
        Pattern.CASE_INSENSITIVE
    );
    private static final Pattern SENSITIVE_PATHS = Pattern.compile(
        "(?:/data/data/|/data/user/|/system/|/vendor/|/proc/|/sdcard/|/storage/|obsidian_prefs|shared_prefs)"
    );

    static CheckResult checkBashCommand(String rawCommand, File repoRoot) {
        if (rawCommand == null || rawCommand.trim().isEmpty()) {
            return CheckResult.block("Empty bash command");
        }
        String cmd = rawCommand.trim();
        String lower = cmd.toLowerCase(Locale.US);

        if (lower.contains(":(){:|:&};:") || lower.contains(":(){ :|:& };:")) {
            return CheckResult.block("Fork bomb blocked");
        }
        if (REPO_DELETE_CMD.matcher(cmd).find()) {
            return CheckResult.block("Repository deletion commands are strictly forbidden");
        }
        if (DANGEROUS_RM.matcher(lower).find()
            || lower.matches(".*\\brm\\s+-[a-z]*r[a-z]*\\s*$")
            || lower.contains("rmdir .")
            || (lower.contains("find ") && lower.contains("-delete") && (lower.contains("find .") || lower.contains("find /")))) {
            return CheckResult.block("Recursive or root-level deletion is blocked to protect the repository");
        }
        if (DESTRUCTIVE_DISK.matcher(lower).find()) {
            return CheckResult.block("Privileged or destructive system command blocked");
        }
        if (PIPE_TO_SHELL.matcher(lower).find()) {
            return CheckResult.block("Piping remote scripts directly into a shell is blocked");
        }
        if (FORCE_PUSH_PROTECTED.matcher(lower).find() || DELETE_REMOTE_BRANCH.matcher(lower).find()) {
            return CheckResult.block("Force-pushing or deleting protected main/master branch is blocked");
        }
        if (lower.contains(".obsidian_ai_meta.json") && (lower.contains("rm ") || lower.contains(">") || lower.contains("mv "))) {
            return CheckResult.block("Modifying internal workspace metadata is blocked");
        }
        if (SENSITIVE_PATHS.matcher(lower).find()) {
            return CheckResult.block("Access outside the repository sandbox to system or app-private paths is blocked");
        }
        if (repoRoot != null) {
            String abs = repoRoot.getAbsolutePath();
            if (lower.contains("rm -rf " + abs.toLowerCase(Locale.US)) || lower.contains("rm -fr " + abs.toLowerCase(Locale.US))) {
                return CheckResult.block("Deleting repository root directory is forbidden");
            }
        }
        if (cmd.contains(".. /") || cmd.contains("../..")) {
            return CheckResult.block("Parent directory traversal in shell command is blocked");
        }
        return CheckResult.ok();
    }

    static CheckResult checkGitHubApiAction(String method, String apiPath) {
        String m = method == null ? "GET" : method.toUpperCase(Locale.US).trim();
        String p = apiPath == null ? "" : apiPath.trim();
        if ("DELETE".equals(m)) {
            if (p.matches("^/?repos/[^/]+/[^/]+/?$")) {
                return CheckResult.block("Deleting GitHub repositories is strictly forbidden");
            }
            if (p.matches("^/?repos/[^/]+/[^/]+/git/refs/heads/(main|master)/?$")) {
                return CheckResult.block("Deleting protected main/master branch is forbidden");
            }
        }
        return CheckResult.ok();
    }

    static File resolveSafeRepoPath(File repoRoot, String relPath) throws IOException {
        if (repoRoot == null) throw new IOException("No repository workspace selected");
        if (relPath == null || relPath.trim().isEmpty()) {
            throw new IOException("Empty file path");
        }
        String clean = relPath.trim().replace('\\', '/');
        while (clean.startsWith("./")) clean = clean.substring(2);
        if (clean.startsWith("/") || clean.equals("..") || clean.startsWith("../") || clean.contains("/../")) {
            throw new IOException("BLOCKED_BY_SAFETY_GUARD: Path escapes repository root: " + relPath);
        }
        if (clean.equals(".obsidian_ai_meta.json") || clean.equals(".git") || clean.startsWith(".git/")) {
            throw new IOException("BLOCKED_BY_SAFETY_GUARD: Protected internal path: " + relPath);
        }
        File target = new File(repoRoot, clean);
        String rootCanon = repoRoot.getCanonicalPath();
        String targetCanon = target.getCanonicalPath();
        if (!targetCanon.equals(rootCanon) && !targetCanon.startsWith(rootCanon + File.separator)) {
            throw new IOException("BLOCKED_BY_SAFETY_GUARD: Canonical path escapes repository root: " + relPath);
        }
        return target;
    }
}
