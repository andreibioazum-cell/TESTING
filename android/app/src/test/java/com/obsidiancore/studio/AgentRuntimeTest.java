package com.obsidiancore.studio;

import java.io.File;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.*;

public class AgentRuntimeTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void parsesThinkAndToolCallTags() {
        String raw = "<think>First read main.py to inspect lines.</think>\n"
            + "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"main.py\",\"start_line\":1,\"end_line\":50}}</tool_call>";
        AgentRuntime.ParsedTurn p = AgentRuntime.parseResponse(raw);
        assertEquals("First read main.py to inspect lines.", p.thought);
        assertEquals("read_file", p.toolName);
        assertEquals("main.py", p.toolArgs.optString("path"));
        assertEquals(50, p.toolArgs.optInt("end_line"));
    }

    @Test
    public void parsesMarkdownJsonFallbackAndFinalAnswer() {
        String fenced = "I will check tests now.\n```json\n{\"name\":\"run_bash\",\"arguments\":{\"command\":\"ls\"}}\n```";
        AgentRuntime.ParsedTurn p1 = AgentRuntime.parseResponse(fenced);
        assertEquals("run_bash", p1.toolName);
        assertEquals("ls", p1.toolArgs.optString("command"));

        String finalMsg = "<think>All tests passed.</think>\nUpdated `main.py` and verified tests.";
        AgentRuntime.ParsedTurn p2 = AgentRuntime.parseResponse(finalMsg);
        assertNull(p2.toolName);
        assertEquals("All tests passed.", p2.thought);
        assertEquals("Updated `main.py` and verified tests.", p2.visibleText);
    }

    @Test
    public void systemPromptDisablesThinkingAndIsCompact() {
        String prompt = AgentRuntime.buildSystemPrompt(null, "no");
        assertTrue(prompt.contains("No <think> tags"));
        assertTrue(prompt.contains("Act autonomously"));
        assertTrue(prompt.length() < 900);
        assertEquals("py", AgentRuntime.guessLang("src/main.py"));
        assertEquals("java", AgentRuntime.guessLang("App.java"));
        assertEquals("code", AgentRuntime.guessLang("Makefile"));
    }

    @Test
    public void executesToolsAndEnforcesSafetyGuard() throws Exception {
        File dir = tmp.newFolder("agent_repo");
        RepoWorkspace ws = new RepoWorkspace(dir);
        RepoWorkspace.writeBytes(new File(dir, "README.md"), "# Demo\nVersion 1\n".getBytes(StandardCharsets.UTF_8));
        ws.saveBaseline("alice", "demo", "main", "1234567");

        AgentRuntime.ToolExecResult del = AgentRuntime.executeTool("delete_repository", new JSONObject(), "tok", ws);
        assertTrue(del.blocked);
        assertTrue(del.output.contains("BLOCKED_BY_SAFETY_GUARD"));

        AgentRuntime.ToolExecResult rmRoot = AgentRuntime.executeTool(
            "run_bash", new JSONObject().put("command", "rm -rf ."), "tok", ws);
        assertTrue(rmRoot.blocked);

        AgentRuntime.ToolExecResult edit = AgentRuntime.executeTool(
            "edit_file",
            new JSONObject().put("path", "README.md").put("old_text", "Version 1").put("new_text", "Version 2"),
            "tok", ws);
        assertTrue(edit.isDiff);
        assertTrue(edit.repoMutated);
        assertFalse(edit.blocked);
    }
}
