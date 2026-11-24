package com.obsidiancore.studio;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.*;

public class RepoWorkspaceTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void incrementalEditAndNoOverwriteFromScratch() throws Exception {
        File dir = tmp.newFolder("demo_repo");
        RepoWorkspace ws = new RepoWorkspace(dir);
        File src = new File(dir, "app.py");
        RepoWorkspace.writeBytes(src, ("def greet():\n    return 'hi'\n\ndef bye():\n    return 'bye'\n").getBytes(StandardCharsets.UTF_8));
        ws.saveBaseline("alice", "demo", "main", "abc1234");

        // read_file returns numbered lines
        String readOut = ws.readFile("app.py", 1, 10);
        assertTrue(readOut.contains("   1 | def greet():"));
        assertTrue(readOut.contains("   2 |     return 'hi'"));

        // write_new_file refuses to overwrite existing non-empty file
        String overwriteAttempt = ws.writeNewFile("app.py", "completely new");
        assertTrue(overwriteAttempt.contains("Refusing to overwrite"));

        // edit_file updates only targeted fragment and produces diff
        String editOut = ws.editFile("app.py", "def greet():\n    return 'hi'", "def greet():\n    return 'hello world'");
        assertTrue(editOut.startsWith("Edited app.py"));
        assertTrue(editOut.contains("-     return 'hi'"));
        assertTrue(editOut.contains("+     return 'hello world'"));

        String after = new String(RepoWorkspace.readBytes(src), StandardCharsets.UTF_8);
        assertTrue(after.contains("return 'hello world'"));
        assertTrue(after.contains("def bye():")); // untouched lines preserved

        // write_new_file works for brand new files
        String createOut = ws.writeNewFile("tests/test_app.py", "assert True\n");
        assertTrue(createOut.startsWith("Created new file tests/test_app.py"));

        // computeChangedFiles detects 1 MODIFIED and 1 ADDED
        List<RepoWorkspace.ChangedFile> changes = ws.computeChangedFiles();
        assertEquals(2, changes.size());
    }

    @Test
    public void editRejectsMissingOrAmbiguousSnippet() throws Exception {
        File dir = tmp.newFolder("demo_repo2");
        RepoWorkspace ws = new RepoWorkspace(dir);
        RepoWorkspace.writeBytes(new File(dir, "dup.txt"), "x = 1\ny = 2\nx = 1\n".getBytes(StandardCharsets.UTF_8));

        String missing = ws.editFile("dup.txt", "z = 99", "z = 100");
        assertTrue(missing.contains("not found"));

        String ambig = ws.editFile("dup.txt", "x = 1", "x = 2");
        assertTrue(ambig.contains("multiple locations"));
    }

    @Test
    public void grepSearchAndSafeBashWork() throws Exception {
        File dir = tmp.newFolder("demo_repo3");
        RepoWorkspace ws = new RepoWorkspace(dir);
        RepoWorkspace.writeBytes(new File(dir, "hello.txt"), "alpha\nbeta needle\ngamma\n".getBytes(StandardCharsets.UTF_8));
        ws.saveBaseline("alice", "demo", "main", "abc");

        String grep = ws.grepSearch("needle", ".");
        assertTrue(grep.contains("hello.txt:2: beta needle"));

        String bashOk = ws.runBash("echo ok_from_bash && ls");
        assertTrue(bashOk.contains("Exit code: 0"));
        assertTrue(bashOk.contains("ok_from_bash"));
        assertTrue(bashOk.contains("hello.txt"));

        String bashBlocked = ws.runBash("rm -rf .");
        assertTrue(bashBlocked.startsWith("BLOCKED_BY_SAFETY_GUARD"));
    }
}
