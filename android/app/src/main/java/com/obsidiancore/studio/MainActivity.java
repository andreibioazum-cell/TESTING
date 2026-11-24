package com.obsidiancore.studio;

import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.ViewFlipper;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import com.google.android.material.button.MaterialButton;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends AppCompatActivity {
    private static final String PREFS_NAME = "obsidian_ai_prefs";

    private SharedPreferences prefs;
    private final ExecutorService netExec = Executors.newCachedThreadPool();
    private final ExecutorService aiExec = Executors.newSingleThreadExecutor();
    private final AgentRuntime agent = new AgentRuntime();

    private ViewFlipper flipper;
    private Button[] navButtons = new Button[4];

    // Top status bar
    private TextView statusModelText;
    private TextView statusRepoText;

    // Chat tab
    private RecyclerView chatRecycler;
    private ChatAdapter chatAdapter;
    private EditText chatInput;
    private Button sendStopBtn;
    private volatile boolean aiBusy = false;
    private int streamingPos = -1;

    // Repos tab
    private EditText tokenInput;
    private TextView accountText;
    private EditText repoFilterInput;
    private EditText manualRepoInput;
    private EditText manualBranchInput;
    private LinearLayout repoListBox;
    private TextView repoChangesText;
    private EditText commitMsgInput;
    private final List<GitHubClient.RepoInfo> fetchedRepos = new ArrayList<>();

    // Models tab
    private TextView activeModelInfoText;
    private TextView downloadStatusText;
    private ProgressBar downloadProgress;
    private Button cancelDownloadBtn;
    private LinearLayout localModelsBox;
    private EditText customUrlInput;
    private Button ctxBtn, threadsBtn, tempBtn;
    private final AtomicBoolean downloadCancel = new AtomicBoolean(false);
    private volatile boolean downloading = false;

    // Actions & Bash tab
    private LinearLayout workflowsBox;
    private LinearLayout runsBox;
    private EditText bashInput;
    private TextView bashOutputText;

    // Active state
    private RepoWorkspace activeWorkspace;
    private String activeOwner = "";
    private String activeRepo = "";
    private String activeBranch = "main";
    private String selectedModelPathOrUri = "";
    private boolean selectedModelIsUri = false;
    private String selectedModelName = "";
    private int nCtx = 4096;
    private int nThreads = 4;
    private float temperature = 0.2f;

    private final ActivityResultLauncher<String[]> modelPicker =
        registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::onPickedModelUri);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        activeOwner = prefs.getString("active_owner", "");
        activeRepo = prefs.getString("active_repo", "");
        activeBranch = prefs.getString("active_branch", "main");
        selectedModelPathOrUri = prefs.getString("selected_model_path", "");
        selectedModelIsUri = prefs.getBoolean("selected_model_is_uri", false);
        selectedModelName = prefs.getString("selected_model_name", "");
        nCtx = prefs.getInt("n_ctx", 4096);
        nThreads = prefs.getInt("n_threads", 4);
        temperature = prefs.getFloat("temperature", 0.2f);

        if (!activeOwner.isEmpty() && !activeRepo.isEmpty()) {
            activeWorkspace = new RepoWorkspace(repoDirFor(activeOwner, activeRepo));
        }

        buildUi();
        refreshStatusBar();
        refreshLocalModelsList();
        refreshRepoChangesUi();

        if (!prefs.getString("github_token", "").isEmpty()) {
            verifyTokenAndLoadRepos(true);
        }
    }

    @Override
    protected void onDestroy() {
        agent.abort();
        downloadCancel.set(true);
        netExec.shutdownNow();
        aiExec.shutdownNow();
        super.onDestroy();
    }

    private float dp(float v) {
        return v * getResources().getDisplayMetrics().density;
    }

    private int dpInt(float v) {
        return Math.round(dp(v));
    }

    private File repoDirFor(String owner, String repo) {
        String safe = (owner + "__" + repo).replaceAll("[^a-zA-Z0-9._-]", "_");
        return new File(new File(getFilesDir(), "repos"), safe);
    }

    // -------------------------------------------------------------------------
    // UI Construction (Monochrome Obsidian Core Palette)
    // -------------------------------------------------------------------------
    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF101010);
        root.setFitsSystemWindows(true);

        // Top compact status bar
        LinearLayout topBar = new LinearLayout(this);
        topBar.setOrientation(LinearLayout.VERTICAL);
        topBar.setBackgroundColor(0xFF181818);
        topBar.setPadding(dpInt(12), dpInt(8), dpInt(12), dpInt(8));

        LinearLayout headerRow = new LinearLayout(this);
        headerRow.setOrientation(LinearLayout.HORIZONTAL);
        headerRow.setGravity(Gravity.CENTER_VERTICAL);

        TextView brand = new TextView(this);
        brand.setText("OBSIDIAN AI");
        brand.setTextColor(Color.WHITE);
        brand.setTypeface(Typeface.DEFAULT_BOLD);
        brand.setTextSize(15f);
        headerRow.addView(brand, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button syncQuickBtn = smallBtn("Sync", v -> syncActiveRepo());
        Button pushQuickBtn = smallBtn("Push", v -> switchTab(1));
        headerRow.addView(syncQuickBtn);
        headerRow.addView(spaceW(6));
        headerRow.addView(pushQuickBtn);
        topBar.addView(headerRow);

        statusModelText = new TextView(this);
        statusModelText.setTextSize(12f);
        statusModelText.setTextColor(0xFFD0D0D0);
        statusModelText.setPadding(0, dpInt(4), 0, 0);
        topBar.addView(statusModelText);

        statusRepoText = new TextView(this);
        statusRepoText.setTextSize(12f);
        statusRepoText.setTextColor(0xFFB8B8B8);
        topBar.addView(statusRepoText);

        root.addView(topBar, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(divider());

        // Main content flipper
        flipper = new ViewFlipper(this);
        flipper.addView(buildChatTab());
        flipper.addView(buildReposTab());
        flipper.addView(buildModelsTab());
        flipper.addView(buildActionsTab());
        root.addView(flipper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        root.addView(divider());

        // Bottom navigation bar
        LinearLayout navBar = new LinearLayout(this);
        navBar.setOrientation(LinearLayout.HORIZONTAL);
        navBar.setBackgroundColor(0xFF161616);
        navBar.setPadding(dpInt(6), dpInt(6), dpInt(6), dpInt(6));
        String[] titles = {"Chat", "Repos", "Models", "Actions"};
        for (int i = 0; i < 4; i++) {
            final int idx = i;
            Button b = new MaterialButton(this);
            b.setText(titles[i]);
            b.setAllCaps(false);
            b.setTextSize(12.5f);
            b.setTextColor(Color.WHITE);
            b.setPadding(dpInt(4), dpInt(6), dpInt(4), dpInt(6));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dpInt(40), 1f);
            lp.setMargins(dpInt(3), 0, dpInt(3), 0);
            b.setLayoutParams(lp);
            b.setOnClickListener(v -> switchTab(idx));
            navButtons[i] = b;
            navBar.addView(b);
        }
        root.addView(navBar, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
        switchTab(0);
    }

    private View buildChatTab() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);

        chatRecycler = new RecyclerView(this);
        chatRecycler.setLayoutManager(new LinearLayoutManager(this));
        chatAdapter = new ChatAdapter();
        chatRecycler.setAdapter(chatAdapter);
        chatAdapter.addItem(new ChatAdapter.Item(
            ChatAdapter.ItemType.ASSISTANT,
            "Obsidian AI Agent",
            "1. Open Repos -> paste your GitHub token and sync a repository.\n"
            + "2. Open Models -> download a GGUF model or pick one from your phone, then tap Load.\n"
            + "3. Ask me to analyze code, make incremental edits (read_file + edit_file), run bash tests, push commits, or trigger GitHub Actions.\n"
            + "Safety Guard is active: repository deletion and destructive commands are blocked."
        ));
        box.addView(chatRecycler, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        box.addView(divider());

        LinearLayout composer = new LinearLayout(this);
        composer.setOrientation(LinearLayout.VERTICAL);
        composer.setBackgroundColor(0xFF181818);
        composer.setPadding(dpInt(10), dpInt(8), dpInt(10), dpInt(8));

        chatInput = styledEdit("Ask AI to inspect, edit files, run bash, push, or trigger Actions...");
        chatInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        chatInput.setMinLines(2);
        chatInput.setMaxLines(5);
        composer.addView(chatInput, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setPadding(0, dpInt(6), 0, 0);

        Button clearBtn = smallBtn("Clear", v -> {
            agent.clearHistory();
            chatAdapter.clear();
        });
        sendStopBtn = primaryBtn("Send", v -> onSendOrStopClicked());
        btnRow.addView(clearBtn);
        btnRow.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1f));
        btnRow.addView(sendStopBtn);
        composer.addView(btnRow);

        box.addView(composer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return box;
    }

    private View buildReposTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dpInt(12), dpInt(12), dpInt(12), dpInt(16));

        box.addView(sectionTitle("GitHub Access Token"));
        tokenInput = styledEdit("ghp_... or github_pat_...");
        tokenInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        tokenInput.setText(prefs.getString("github_token", ""));
        box.addView(tokenInput);

        LinearLayout tokBtns = new LinearLayout(this);
        tokBtns.setOrientation(LinearLayout.HORIZONTAL);
        tokBtns.setPadding(0, dpInt(6), 0, dpInt(6));
        tokBtns.addView(primaryBtn("Connect & Load Repos", v -> {
            prefs.edit().putString("github_token", tokenInput.getText().toString().trim()).apply();
            verifyTokenAndLoadRepos(false);
        }));
        tokBtns.addView(spaceW(8));
        tokBtns.addView(smallBtn("Clear Key", v -> {
            tokenInput.setText("");
            prefs.edit().remove("github_token").apply();
            accountText.setText("Not connected");
            fetchedRepos.clear();
            renderRepoList();
        }));
        box.addView(tokBtns);

        accountText = infoText("Not connected");
        box.addView(accountText);

        box.addView(spaceH(10));
        box.addView(sectionTitle("Manual Repository / Branch Sync"));
        LinearLayout manRow = new LinearLayout(this);
        manRow.setOrientation(LinearLayout.HORIZONTAL);
        manualRepoInput = styledEdit("owner/repo");
        if (!activeOwner.isEmpty() && !activeRepo.isEmpty()) {
            manualRepoInput.setText(activeOwner + "/" + activeRepo);
        }
        manualBranchInput = styledEdit("branch");
        manualBranchInput.setText(activeBranch);
        manRow.addView(manualRepoInput, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2f));
        manRow.addView(spaceW(6));
        manRow.addView(manualBranchInput, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(manRow);

        LinearLayout syncRow = new LinearLayout(this);
        syncRow.setOrientation(LinearLayout.HORIZONTAL);
        syncRow.setPadding(0, dpInt(6), 0, dpInt(6));
        syncRow.addView(primaryBtn("Clone / Sync Repo", v -> {
            String full = manualRepoInput.getText().toString().trim();
            String br = manualBranchInput.getText().toString().trim();
            int sl = full.indexOf('/');
            if (sl <= 0 || sl >= full.length() - 1) {
                toast("Enter owner/repo");
                return;
            }
            selectAndSyncRepo(full.substring(0, sl).trim(), full.substring(sl + 1).trim(), br);
        }));
        syncRow.addView(spaceW(8));
        syncRow.addView(smallBtn("Browse Files", v -> openRepoFileBrowser()));
        box.addView(syncRow);

        box.addView(spaceH(10));
        box.addView(sectionTitle("Local Changes & Push"));
        repoChangesText = infoText("No repository synced");
        box.addView(repoChangesText);

        commitMsgInput = styledEdit("Commit message (e.g. Fix bug in parser)");
        box.addView(commitMsgInput);
        LinearLayout pushRow = new LinearLayout(this);
        pushRow.setOrientation(LinearLayout.HORIZONTAL);
        pushRow.setPadding(0, dpInt(6), 0, dpInt(6));
        pushRow.addView(primaryBtn("Commit & Push", v -> pushActiveRepo()));
        pushRow.addView(spaceW(8));
        pushRow.addView(smallBtn("Refresh Diff", v -> refreshRepoChangesUi()));
        box.addView(pushRow);

        box.addView(spaceH(10));
        box.addView(sectionTitle("Your GitHub Repositories"));
        repoFilterInput = styledEdit("Filter repositories...");
        repoFilterInput.addTextChangedListener(new SimpleTextWatcher(s -> renderRepoList()));
        box.addView(repoFilterInput);

        repoListBox = new LinearLayout(this);
        repoListBox.setOrientation(LinearLayout.VERTICAL);
        repoListBox.setPadding(0, dpInt(6), 0, 0);
        box.addView(repoListBox);

        sv.addView(box);
        return sv;
    }

    private View buildModelsTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dpInt(12), dpInt(12), dpInt(12), dpInt(16));

        box.addView(sectionTitle("Active Local Model (GGUF)"));
        activeModelInfoText = infoText("No model loaded");
        box.addView(activeModelInfoText);

        // Inference params row
        LinearLayout paramRow = new LinearLayout(this);
        paramRow.setOrientation(LinearLayout.HORIZONTAL);
        paramRow.setPadding(0, dpInt(6), 0, dpInt(6));
        ctxBtn = smallBtn("Ctx: " + nCtx, v -> {
            nCtx = (nCtx == 2048) ? 4096 : (nCtx == 4096 ? 8192 : 2048);
            prefs.edit().putInt("n_ctx", nCtx).apply();
            ctxBtn.setText("Ctx: " + nCtx);
        });
        threadsBtn = smallBtn("Threads: " + nThreads, v -> {
            nThreads = (nThreads == 2) ? 4 : (nThreads == 4 ? 6 : 2);
            prefs.edit().putInt("n_threads", nThreads).apply();
            threadsBtn.setText("Threads: " + nThreads);
        });
        tempBtn = smallBtn(String.format(Locale.US, "Temp: %.1f", temperature), v -> {
            temperature = (temperature < 0.1f) ? 0.2f : (temperature < 0.5f ? 0.6f : 0.0f);
            prefs.edit().putFloat("temperature", temperature).apply();
            tempBtn.setText(String.format(Locale.US, "Temp: %.1f", temperature));
        });
        paramRow.addView(ctxBtn);
        paramRow.addView(spaceW(6));
        paramRow.addView(threadsBtn);
        paramRow.addView(spaceW(6));
        paramRow.addView(tempBtn);
        box.addView(paramRow);

        LinearLayout loadRow = new LinearLayout(this);
        loadRow.setOrientation(LinearLayout.HORIZONTAL);
        loadRow.setPadding(0, dpInt(4), 0, dpInt(8));
        loadRow.addView(primaryBtn("Load Selected", v -> loadSelectedModel()));
        loadRow.addView(spaceW(6));
        loadRow.addView(smallBtn("Unload", v -> {
            LlamaEngine.unload();
            refreshStatusBar();
            toast("Model unloaded");
        }));
        loadRow.addView(spaceW(6));
        loadRow.addView(smallBtn("Pick .gguf File", v -> modelPicker.launch(new String[]{"*/*"})));
        box.addView(loadRow);

        box.addView(sectionTitle("Downloaded & Phone GGUF Models"));
        localModelsBox = new LinearLayout(this);
        localModelsBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(localModelsBox);

        box.addView(spaceH(12));
        box.addView(sectionTitle("Download Model from Catalog or URL"));

        downloadProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        downloadProgress.setMax(100);
        downloadProgress.setVisibility(View.GONE);
        box.addView(downloadProgress);

        LinearLayout dlStatusRow = new LinearLayout(this);
        dlStatusRow.setOrientation(LinearLayout.HORIZONTAL);
        dlStatusRow.setGravity(Gravity.CENTER_VERTICAL);
        downloadStatusText = infoText("Idle");
        downloadStatusText.setVisibility(View.GONE);
        cancelDownloadBtn = smallBtn("Cancel", v -> downloadCancel.set(true));
        cancelDownloadBtn.setVisibility(View.GONE);
        dlStatusRow.addView(downloadStatusText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        dlStatusRow.addView(cancelDownloadBtn);
        box.addView(dlStatusRow);

        customUrlInput = styledEdit("https://huggingface.co/.../model.gguf");
        box.addView(customUrlInput);
        LinearLayout customRow = new LinearLayout(this);
        customRow.setOrientation(LinearLayout.HORIZONTAL);
        customRow.setPadding(0, dpInt(6), 0, dpInt(10));
        customRow.addView(primaryBtn("Download Custom URL", v -> {
            String u = customUrlInput.getText().toString().trim();
            if (u.isEmpty()) { toast("Enter GGUF URL"); return; }
            startModelDownload(u, "");
        }));
        box.addView(customRow);

        for (ModelCatalog.CatalogEntry ce : ModelCatalog.CATALOG) {
            LinearLayout card = cardContainer();
            TextView t = new TextView(this);
            t.setText(ce.title);
            t.setTextColor(Color.WHITE);
            t.setTypeface(Typeface.DEFAULT_BOLD);
            t.setTextSize(14f);
            TextView sub = new TextView(this);
            sub.setText(ce.subtitle);
            sub.setTextColor(0xFFC8C8C8);
            sub.setTextSize(12f);
            sub.setPadding(0, dpInt(2), 0, dpInt(6));
            card.addView(t);
            card.addView(sub);
            card.addView(smallBtn("Download", v -> startModelDownload(ce.url, ce.filename)));
            box.addView(card);
        }

        sv.addView(box);
        return sv;
    }

    private View buildActionsTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dpInt(12), dpInt(12), dpInt(12), dpInt(16));

        LinearLayout hdr = new LinearLayout(this);
        hdr.setOrientation(LinearLayout.HORIZONTAL);
        hdr.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = sectionTitle("GitHub Actions");
        hdr.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        hdr.addView(primaryBtn("Refresh", v -> refreshActionsTab()));
        box.addView(hdr);

        workflowsBox = new LinearLayout(this);
        workflowsBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(workflowsBox);

        box.addView(spaceH(8));
        box.addView(sectionTitle("Recent Workflow Runs"));
        runsBox = new LinearLayout(this);
        runsBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(runsBox);

        box.addView(spaceH(12));
        box.addView(sectionTitle("Repo Bash Sandbox (Safety Guard Protected)"));
        bashInput = styledEdit("e.g. ls -la && python3 -m unittest");
        box.addView(bashInput);
        LinearLayout bashBtnRow = new LinearLayout(this);
        bashBtnRow.setOrientation(LinearLayout.HORIZONTAL);
        bashBtnRow.setPadding(0, dpInt(6), 0, dpInt(6));
        bashBtnRow.addView(primaryBtn("Run Bash", v -> runManualBash()));
        box.addView(bashBtnRow);

        bashOutputText = new TextView(this);
        bashOutputText.setTypeface(Typeface.MONOSPACE);
        bashOutputText.setTextSize(12f);
        bashOutputText.setTextColor(Color.WHITE);
        bashOutputText.setBackgroundColor(0xFF161616);
        bashOutputText.setPadding(dpInt(10), dpInt(10), dpInt(10), dpInt(10));
        bashOutputText.setTextIsSelectable(true);
        bashOutputText.setText("Ready.");
        box.addView(bashOutputText);

        sv.addView(box);
        return sv;
    }

    private void switchTab(int idx) {
        flipper.setDisplayedChild(idx);
        for (int i = 0; i < 4; i++) {
            navButtons[i].setBackgroundColor(i == idx ? 0xFF4A4A4A : 0xFF242424);
        }
        if (idx == 1) refreshRepoChangesUi();
        if (idx == 2) refreshLocalModelsList();
        if (idx == 3) refreshActionsTab();
    }

    // -------------------------------------------------------------------------
    // Chat / Agent Loop
    // -------------------------------------------------------------------------
    private void onSendOrStopClicked() {
        if (aiBusy) {
            agent.abort();
            sendStopBtn.setText("Stopping...");
            return;
        }
        String text = chatInput.getText().toString().trim();
        if (text.isEmpty()) return;
        chatInput.setText("");

        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.USER, "You", text));
        scrollToBottom();

        aiBusy = true;
        sendStopBtn.setText("Stop");
        streamingPos = -1;

        String token = prefs.getString("github_token", "");
        aiExec.execute(() -> {
            agent.runTurn(text, token, activeWorkspace, 10, temperature, new AgentRuntime.Listener() {
                @Override
                public void onStreamToken(String tok) {
                    runOnUiThread(() -> {
                        if (streamingPos < 0) {
                            streamingPos = chatAdapter.startStreamingAssistant();
                        }
                        chatAdapter.appendStream(streamingPos, tok);
                        scrollToBottom();
                    });
                }

                @Override
                public void onThought(String thought) {
                    runOnUiThread(() -> {
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.THOUGHT, "Thought", thought));
                        scrollToBottom();
                    });
                }

                @Override
                public void onToolCall(String name, String argsSummary) {
                    runOnUiThread(() -> {
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.TOOL_CALL, "Tool: " + name, argsSummary));
                        scrollToBottom();
                    });
                }

                @Override
                public void onToolResult(String name, String result, boolean isDiff, boolean blocked) {
                    runOnUiThread(() -> {
                        ChatAdapter.ItemType type = blocked
                            ? ChatAdapter.ItemType.BLOCKED
                            : (isDiff ? ChatAdapter.ItemType.DIFF : ChatAdapter.ItemType.TOOL_RESULT);
                        String title = blocked ? "Blocked by Safety Guard (" + name + ")"
                            : (isDiff ? "File Diff (" + name + ")" : "Result (" + name + ")");
                        chatAdapter.addItem(new ChatAdapter.Item(type, title, result));
                        scrollToBottom();
                    });
                }

                @Override
                public void onAssistantMessage(String message) {
                    runOnUiThread(() -> {
                        if (streamingPos >= 0) {
                            chatAdapter.replaceItem(streamingPos,
                                new ChatAdapter.Item(ChatAdapter.ItemType.ASSISTANT, "Obsidian AI", message));
                            streamingPos = -1;
                        } else {
                            chatAdapter.addItem(new ChatAdapter.Item(
                                ChatAdapter.ItemType.ASSISTANT, "Obsidian AI", message));
                        }
                        scrollToBottom();
                    });
                }

                @Override
                public void onError(String error) {
                    runOnUiThread(() -> {
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.ERROR, "Notice", error));
                        scrollToBottom();
                    });
                }

                @Override
                public void onRepoChanged() {
                    runOnUiThread(() -> refreshRepoChangesUi());
                }
            });

            runOnUiThread(() -> {
                aiBusy = false;
                sendStopBtn.setText("Send");
            });
        });
    }

    private void finishOrDropStream() {
        if (streamingPos >= 0) {
            chatAdapter.removeItem(streamingPos);
            streamingPos = -1;
        }
    }

    private void scrollToBottom() {
        int count = chatAdapter.getItemCount();
        if (count > 0) chatRecycler.scrollToPosition(count - 1);
    }

    // -------------------------------------------------------------------------
    // GitHub Repos & Actions
    // -------------------------------------------------------------------------
    private void verifyTokenAndLoadRepos(boolean silent) {
        String token = prefs.getString("github_token", "").trim();
        if (token.isEmpty()) {
            if (!silent) toast("Enter your GitHub Personal Access Token");
            return;
        }
        accountText.setText("Connecting to GitHub...");
        netExec.execute(() -> {
            try {
                GitHubClient.UserInfo user = GitHubClient.verifyToken(token);
                List<GitHubClient.RepoInfo> repos = GitHubClient.listRepositories(token);
                runOnUiThread(() -> {
                    accountText.setText("Connected as @" + user.login + " (" + repos.size() + " repos)");
                    fetchedRepos.clear();
                    fetchedRepos.addAll(repos);
                    renderRepoList();
                });
            } catch (Exception e) {
                runOnUiThread(() -> accountText.setText("Auth error: " + e.getMessage()));
            }
        });
    }

    private void renderRepoList() {
        repoListBox.removeAllViews();
        String filter = repoFilterInput != null ? repoFilterInput.getText().toString().trim().toLowerCase(Locale.US) : "";
        for (GitHubClient.RepoInfo r : fetchedRepos) {
            if (!filter.isEmpty() && !r.fullName.toLowerCase(Locale.US).contains(filter)) continue;
            LinearLayout card = cardContainer();
            TextView title = new TextView(this);
            title.setText(r.fullName + (r.isPrivate ? " [private]" : " [public]"));
            title.setTextColor(Color.WHITE);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            title.setTextSize(14f);
            TextView sub = new TextView(this);
            sub.setText("Branch: " + r.defaultBranch
                + (r.language.isEmpty() ? "" : " • " + r.language)
                + (r.description.isEmpty() ? "" : "\n" + r.description));
            sub.setTextColor(0xFFC0C0C0);
            sub.setTextSize(12f);
            sub.setPadding(0, dpInt(2), 0, dpInt(6));
            card.addView(title);
            card.addView(sub);
            card.addView(smallBtn("Select & Sync", v -> selectAndSyncRepo(r.owner, r.name, r.defaultBranch)));
            repoListBox.addView(card);
        }
    }

    private void selectAndSyncRepo(String owner, String repo, String branch) {
        activeOwner = owner;
        activeRepo = repo;
        activeBranch = (branch == null || branch.isEmpty()) ? "main" : branch;
        prefs.edit()
            .putString("active_owner", activeOwner)
            .putString("active_repo", activeRepo)
            .putString("active_branch", activeBranch)
            .apply();
        activeWorkspace = new RepoWorkspace(repoDirFor(activeOwner, activeRepo));
        if (manualRepoInput != null) manualRepoInput.setText(activeOwner + "/" + activeRepo);
        if (manualBranchInput != null) manualBranchInput.setText(activeBranch);
        syncActiveRepo();
    }

    private void syncActiveRepo() {
        if (activeOwner.isEmpty() || activeRepo.isEmpty()) {
            toast("Select a repository in the Repos tab first");
            switchTab(1);
            return;
        }
        String token = prefs.getString("github_token", "").trim();
        if (token.isEmpty()) {
            toast("Enter your GitHub token in the Repos tab");
            switchTab(1);
            return;
        }
        statusRepoText.setText("Repo: syncing " + activeOwner + "/" + activeRepo + "...");
        netExec.execute(() -> {
            try {
                if (activeWorkspace == null) {
                    activeWorkspace = new RepoWorkspace(repoDirFor(activeOwner, activeRepo));
                }
                String msg = GitHubClient.cloneOrSyncRepo(token, activeOwner, activeRepo, activeBranch, activeWorkspace);
                RepoWorkspace.Meta m = activeWorkspace.loadMeta();
                activeBranch = m.branch;
                runOnUiThread(() -> {
                    prefs.edit().putString("active_branch", activeBranch).apply();
                    refreshStatusBar();
                    refreshRepoChangesUi();
                    toast(msg);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    refreshStatusBar();
                    toast("Sync error: " + e.getMessage());
                });
            }
        });
    }

    private void pushActiveRepo() {
        if (activeWorkspace == null || activeOwner.isEmpty()) {
            toast("No synced repository");
            return;
        }
        String token = prefs.getString("github_token", "").trim();
        String msg = commitMsgInput.getText().toString().trim();
        netExec.execute(() -> {
            try {
                String res = GitHubClient.pushChanges(token, activeOwner, activeRepo, activeBranch, msg, activeWorkspace);
                runOnUiThread(() -> {
                    commitMsgInput.setText("");
                    refreshRepoChangesUi();
                    toast(res);
                });
            } catch (Exception e) {
                runOnUiThread(() -> toast("Push error: " + e.getMessage()));
            }
        });
    }

    private void refreshRepoChangesUi() {
        if (repoChangesText == null) return;
        if (activeWorkspace == null || !activeWorkspace.repoRoot.isDirectory()) {
            repoChangesText.setText("No repository synced yet.");
            return;
        }
        try {
            repoChangesText.setText(activeWorkspace.statusSummary());
        } catch (Exception e) {
            repoChangesText.setText("Status error: " + e.getMessage());
        }
    }

    private void openRepoFileBrowser() {
        if (activeWorkspace == null || !activeWorkspace.repoRoot.isDirectory()) {
            toast("Sync a repository first");
            return;
        }
        try {
            String listing = activeWorkspace.listFiles(".", 4);
            String[] lines = listing.split("\n");
            List<String> files = new ArrayList<>();
            for (String l : lines) {
                if (!l.endsWith("/") && !l.startsWith("...")) {
                    int idx = l.lastIndexOf(" (");
                    files.add(idx > 0 ? l.substring(0, idx) : l);
                }
            }
            if (files.isEmpty()) { toast("Repository is empty"); return; }
            String[] arr = files.toArray(new String[0]);
            new AlertDialog.Builder(this)
                .setTitle(activeOwner + "/" + activeRepo + " Files")
                .setItems(arr, (d, which) -> showFileViewerDialog(arr[which]))
                .setNegativeButton("Close", null)
                .show();
        } catch (Exception e) {
            toast("Browse error: " + e.getMessage());
        }
    }

    private void showFileViewerDialog(String relPath) {
        try {
            String content = activeWorkspace.readFile(relPath, 1, 250);
            TextView tv = new TextView(this);
            tv.setText(content);
            tv.setTypeface(Typeface.MONOSPACE);
            tv.setTextSize(11.5f);
            tv.setTextColor(Color.WHITE);
            tv.setPadding(dpInt(14), dpInt(10), dpInt(14), dpInt(10));
            tv.setTextIsSelectable(true);
            ScrollView sv = new ScrollView(this);
            sv.addView(tv);
            new AlertDialog.Builder(this)
                .setTitle(relPath)
                .setView(sv)
                .setPositiveButton("Ask AI About File", (d, w) -> {
                    switchTab(0);
                    chatInput.setText("Inspect `" + relPath + "` and explain or suggest improvements.");
                })
                .setNegativeButton("Close", null)
                .show();
        } catch (Exception e) {
            toast("Read error: " + e.getMessage());
        }
    }

    private void refreshActionsTab() {
        if (workflowsBox == null || runsBox == null) return;
        String token = prefs.getString("github_token", "").trim();
        if (token.isEmpty() || activeOwner.isEmpty() || activeRepo.isEmpty()) {
            workflowsBox.removeAllViews();
            workflowsBox.addView(infoText("Connect GitHub token and select a repository in the Repos tab."));
            runsBox.removeAllViews();
            return;
        }
        workflowsBox.removeAllViews();
        workflowsBox.addView(infoText("Loading workflows..."));
        netExec.execute(() -> {
            try {
                List<GitHubClient.WorkflowInfo> wfs = GitHubClient.listWorkflows(token, activeOwner, activeRepo);
                List<GitHubClient.RunInfo> runs = GitHubClient.listWorkflowRuns(token, activeOwner, activeRepo);
                runOnUiThread(() -> {
                    workflowsBox.removeAllViews();
                    if (wfs.isEmpty()) workflowsBox.addView(infoText("No workflows in .github/workflows/"));
                    for (GitHubClient.WorkflowInfo w : wfs) {
                        LinearLayout card = cardContainer();
                        TextView tv = infoText(w.name + " (" + w.path + ") [" + w.state + "]");
                        card.addView(tv);
                        card.addView(smallBtn("Run Workflow (" + activeBranch + ")", v -> triggerWorkflowUi(w)));
                        workflowsBox.addView(card);
                    }
                    runsBox.removeAllViews();
                    if (runs.isEmpty()) runsBox.addView(infoText("No workflow runs yet."));
                    for (GitHubClient.RunInfo r : runs) {
                        LinearLayout card = cardContainer();
                        String statusLine = "#" + r.id + " " + r.name + "\n"
                            + r.status + (r.conclusion.isEmpty() ? "" : " / " + r.conclusion)
                            + " • " + r.branch + " (" + r.event + ")";
                        card.addView(infoText(statusLine));
                        card.addView(smallBtn("View Jobs & Steps", v -> showRunJobsDialog(r.id)));
                        runsBox.addView(card);
                    }
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    workflowsBox.removeAllViews();
                    workflowsBox.addView(infoText("Actions error: " + e.getMessage()));
                });
            }
        });
    }

    private void triggerWorkflowUi(GitHubClient.WorkflowInfo wf) {
        String token = prefs.getString("github_token", "").trim();
        netExec.execute(() -> {
            try {
                String res = GitHubClient.dispatchWorkflow(token, activeOwner, activeRepo, Long.toString(wf.id), activeBranch, "");
                runOnUiThread(() -> {
                    toast(res);
                    refreshActionsTab();
                });
            } catch (Exception e) {
                runOnUiThread(() -> toast("Dispatch error: " + e.getMessage()));
            }
        });
    }

    private void showRunJobsDialog(long runId) {
        String token = prefs.getString("github_token", "").trim();
        netExec.execute(() -> {
            try {
                String details = GitHubClient.getWorkflowRunJobs(token, activeOwner, activeRepo, runId);
                runOnUiThread(() -> new AlertDialog.Builder(this)
                    .setTitle("Run #" + runId + " Jobs")
                    .setMessage(details)
                    .setPositiveButton("OK", null)
                    .show());
            } catch (Exception e) {
                runOnUiThread(() -> toast("Jobs error: " + e.getMessage()));
            }
        });
    }

    private void runManualBash() {
        if (activeWorkspace == null || !activeWorkspace.repoRoot.isDirectory()) {
            toast("Sync a repository first");
            return;
        }
        String cmd = bashInput.getText().toString().trim();
        if (cmd.isEmpty()) return;
        bashOutputText.setText("$ " + cmd + "\nRunning...");
        netExec.execute(() -> {
            String out = activeWorkspace.runBash(cmd);
            runOnUiThread(() -> {
                bashOutputText.setText("$ " + cmd + "\n" + out);
                refreshRepoChangesUi();
            });
        });
    }

    // -------------------------------------------------------------------------
    // Models Management (Download + SAF File Picker + llama.cpp Load)
    // -------------------------------------------------------------------------
    private void onPickedModelUri(Uri uri) {
        if (uri == null) return;
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        String name = "picked-model.gguf";
        long size = 0;
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int ni = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                int si = c.getColumnIndex(OpenableColumns.SIZE);
                if (ni >= 0) name = c.getString(ni);
                if (si >= 0) size = c.getLong(si);
            }
        } catch (Exception ignored) {
        }
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null || !LlamaEngine.isGMagicValid(in)) {
                toast("Selected file is not a valid GGUF model");
                return;
            }
        } catch (Exception e) {
            toast("Cannot read selected file: " + e.getMessage());
            return;
        }
        ModelCatalog.addPickedUriModel(prefs, name, uri.toString(), size);
        selectModel(name, uri.toString(), true);
        refreshLocalModelsList();
        toast("Added model: " + name + ". Tap Load Selected.");
    }

    private void startModelDownload(String url, String filename) {
        if (downloading) {
            toast("A download is already in progress");
            return;
        }
        downloading = true;
        downloadCancel.set(false);
        downloadProgress.setVisibility(View.VISIBLE);
        downloadProgress.setProgress(0);
        downloadStatusText.setVisibility(View.VISIBLE);
        downloadStatusText.setText("Connecting...");
        cancelDownloadBtn.setVisibility(View.VISIBLE);

        netExec.execute(() -> {
            try {
                File out = ModelCatalog.downloadModel(this, url, filename, downloadCancel, (dl, tot, txt) ->
                    runOnUiThread(() -> {
                        downloadStatusText.setText(txt);
                        if (tot > 0) downloadProgress.setProgress((int) Math.min(100, (dl * 100L) / tot));
                    })
                );
                runOnUiThread(() -> {
                    downloading = false;
                    downloadProgress.setVisibility(View.GONE);
                    downloadStatusText.setVisibility(View.GONE);
                    cancelDownloadBtn.setVisibility(View.GONE);
                    selectModel(out.getName(), out.getAbsolutePath(), false);
                    refreshLocalModelsList();
                    toast("Downloaded: " + out.getName());
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    downloading = false;
                    downloadProgress.setVisibility(View.GONE);
                    cancelDownloadBtn.setVisibility(View.GONE);
                    downloadStatusText.setText("Download failed: " + e.getMessage());
                });
            }
        });
    }

    private void selectModel(String name, String pathOrUri, boolean isUri) {
        selectedModelName = name;
        selectedModelPathOrUri = pathOrUri;
        selectedModelIsUri = isUri;
        prefs.edit()
            .putString("selected_model_name", name)
            .putString("selected_model_path", pathOrUri)
            .putBoolean("selected_model_is_uri", isUri)
            .apply();
        refreshStatusBar();
    }

    private void loadSelectedModel() {
        if (selectedModelPathOrUri.isEmpty()) {
            toast("Select or download a GGUF model first");
            return;
        }
        activeModelInfoText.setText("Loading " + selectedModelName + "...");
        aiExec.execute(() -> {
            String res;
            if (selectedModelIsUri) {
                res = LlamaEngine.loadFromUri(getContentResolver(), Uri.parse(selectedModelPathOrUri), nCtx, nThreads);
            } else {
                res = LlamaEngine.loadFromFile(selectedModelPathOrUri, nCtx, nThreads);
            }
            runOnUiThread(() -> {
                refreshStatusBar();
                activeModelInfoText.setText(res);
                toast(res.startsWith("OK:") ? "Model loaded!" : res);
            });
        });
    }

    private void refreshLocalModelsList() {
        if (localModelsBox == null) return;
        localModelsBox.removeAllViews();
        List<ModelCatalog.LocalModel> list = ModelCatalog.listLocalModels(this, prefs);
        if (list.isEmpty()) {
            localModelsBox.addView(infoText("No local models yet. Pick a .gguf from your phone or download one below."));
            return;
        }
        for (ModelCatalog.LocalModel m : list) {
            boolean selected = m.pathOrUri.equals(selectedModelPathOrUri);
            LinearLayout card = cardContainer();
            String sz = m.sizeBytes > 0 ? String.format(Locale.US, " (%.1f MB)", m.sizeBytes / (1024.0 * 1024.0)) : "";
            TextView tv = infoText((selected ? "● [Selected] " : "○ ") + m.name + sz + (m.isUri ? " [Phone File]" : ""));
            card.addView(tv);

            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(0, dpInt(4), 0, 0);
            row.addView(smallBtn("Select", v -> {
                selectModel(m.name, m.pathOrUri, m.isUri);
                refreshLocalModelsList();
            }));
            row.addView(spaceW(6));
            row.addView(smallBtn("Select & Load", v -> {
                selectModel(m.name, m.pathOrUri, m.isUri);
                refreshLocalModelsList();
                loadSelectedModel();
            }));
            row.addView(spaceW(6));
            row.addView(smallBtn("Remove", v -> {
                if (selected && LlamaEngine.nativeIsLoaded()) LlamaEngine.unload();
                ModelCatalog.removeLocalModel(prefs, m);
                if (selected) selectModel("", "", false);
                refreshLocalModelsList();
            }));
            card.addView(row);
            localModelsBox.addView(card);
        }
    }

    private void refreshStatusBar() {
        boolean loaded = LlamaEngine.nativeIsLoaded();
        String mLabel = selectedModelName.isEmpty() ? "none" : selectedModelName;
        statusModelText.setText((loaded ? "● Model Ready: " : "○ Model: ") + mLabel);
        if (activeModelInfoText != null && !loaded) {
            activeModelInfoText.setText(selectedModelName.isEmpty()
                ? "No model selected"
                : "Selected (not loaded): " + selectedModelName);
        }
        if (activeOwner.isEmpty() || activeRepo.isEmpty()) {
            statusRepoText.setText("○ Repo: not selected");
        } else {
            boolean synced = activeWorkspace != null && activeWorkspace.metaFile().isFile();
            statusRepoText.setText((synced ? "● Repo: " : "○ Repo (unsynced): ")
                + activeOwner + "/" + activeRepo + " @ " + activeBranch);
        }
    }

    // -------------------------------------------------------------------------
    // View Helpers
    // -------------------------------------------------------------------------
    private TextView sectionTitle(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(Color.WHITE);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextSize(14f);
        tv.setPadding(0, dpInt(4), 0, dpInt(6));
        return tv;
    }

    private TextView infoText(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(0xFFDCDCDC);
        tv.setTextSize(12.5f);
        tv.setPadding(0, dpInt(2), 0, dpInt(4));
        return tv;
    }

    private EditText styledEdit(String hint) {
        EditText et = new EditText(this);
        et.setHint(hint);
        et.setHintTextColor(0xFF9E9E9E);
        et.setTextColor(Color.WHITE);
        et.setTextSize(13.5f);
        et.setPadding(dpInt(10), dpInt(8), dpInt(10), dpInt(8));
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(0xFF1E1E1E);
        gd.setCornerRadius(dp(6));
        gd.setStroke(dpInt(1), 0xFF424242);
        et.setBackground(gd);
        return et;
    }

    private Button primaryBtn(String label, View.OnClickListener l) {
        MaterialButton b = new MaterialButton(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(12.5f);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(0xFF4A4A4A);
        b.setOnClickListener(l);
        return b;
    }

    private Button smallBtn(String label, View.OnClickListener l) {
        MaterialButton b = new MaterialButton(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(12f);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(0xFF2E2E2E);
        b.setMinHeight(dpInt(34));
        b.setPadding(dpInt(10), dpInt(4), dpInt(10), dpInt(4));
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout cardContainer() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dpInt(10), dpInt(8), dpInt(10), dpInt(8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dpInt(4), 0, dpInt(4));
        card.setLayoutParams(lp);
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(0xFF1A1A1A);
        gd.setCornerRadius(dp(8));
        gd.setStroke(dpInt(1), 0xFF383838);
        card.setBackground(gd);
        return card;
    }

    private View divider() {
        View v = new View(this);
        v.setBackgroundColor(0xFF303030);
        v.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dpInt(1))));
        return v;
    }

    private View spaceW(float dpVal) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(dpInt(dpVal), 1));
        return v;
    }

    private View spaceH(float dpVal) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, dpInt(dpVal)));
        return v;
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }

    private static final class SimpleTextWatcher implements android.text.TextWatcher {
        interface OnChange { void onChanged(String s); }
        private final OnChange cb;
        SimpleTextWatcher(OnChange cb) { this.cb = cb; }
        @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
        @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
        @Override public void afterTextChanged(android.text.Editable s) { cb.onChanged(s.toString()); }
    }
}
