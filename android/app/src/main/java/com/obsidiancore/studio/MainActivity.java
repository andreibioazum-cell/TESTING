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

    // Minimal header
    private TextView headerTitleText;

    // Chat tab
    private RecyclerView chatRecycler;
    private ChatAdapter chatAdapter;
    private EditText chatInput;
    private Button sendStopBtn;
    private volatile boolean aiBusy = false;
    private int streamingPos = -1;
    private int statusPos = -1;
    private boolean receivedFirstToken = false;

    // Repos tab
    private EditText tokenInput;
    private TextView accountText;
    private EditText repoFilterInput;
    private EditText manualRepoInput;
    private EditText manualBranchInput;
    private LinearLayout repoListBox;
    private TextView repoChangesText;
    private final List<GitHubClient.RepoInfo> fetchedRepos = new ArrayList<>();

    // Models tab
    private TextView activeModelInfoText;
    private TextView downloadStatusText;
    private ProgressBar downloadProgress;
    private Button cancelDownloadBtn;
    private LinearLayout localModelsBox;
    private EditText customUrlInput;
    private Button ctxBtn, threadsBtn, tempBtn, coolBtn;
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
    private int nCtx = 2048;
    private int nThreads = 4;
    private float temperature = 0.15f;
    private int coolDelayMs = 0;

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
        int defaultThreads = Math.min(4, Math.max(2, Runtime.getRuntime().availableProcessors() / 2));
        nCtx = prefs.getInt("n_ctx", 2048);
        nThreads = prefs.getInt("n_threads", defaultThreads);
        temperature = prefs.getFloat("temperature", 0.15f);
        coolDelayMs = prefs.getInt("cool_delay_ms", 0);

        if (!activeOwner.isEmpty() && !activeRepo.isEmpty()) {
            activeWorkspace = new RepoWorkspace(repoDirFor(activeOwner, activeRepo));
        }

        buildUi();
        refreshHeader();
        refreshLocalModelsList();
        refreshRepoChangesUi();

        if (!prefs.getString("github_token", "").isEmpty()) {
            verifyTokenAndLoadRepos(true);
        }
        // Auto-load previously selected model silently so user can start chatting right away
        if (!selectedModelPathOrUri.isEmpty() && !LlamaEngine.nativeIsLoaded()) {
            loadSelectedModel(true);
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
    // UI Construction (Clean Minimal Monochrome)
    // -------------------------------------------------------------------------
    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF101010);
        root.setFitsSystemWindows(true);

        // Clean single-line header
        LinearLayout topBar = new LinearLayout(this);
        topBar.setOrientation(LinearLayout.HORIZONTAL);
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        topBar.setBackgroundColor(0xFF161616);
        topBar.setPadding(dpInt(12), dpInt(8), dpInt(12), dpInt(8));

        headerTitleText = new TextView(this);
        headerTitleText.setTextColor(Color.WHITE);
        headerTitleText.setTypeface(Typeface.DEFAULT_BOLD);
        headerTitleText.setTextSize(14f);
        headerTitleText.setOnClickListener(v -> {
            if (activeOwner.isEmpty()) switchTab(1);
            else if (selectedModelName.isEmpty()) switchTab(2);
        });
        topBar.addView(headerTitleText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView clearBtn = new TextView(this);
        clearBtn.setText("New Chat");
        clearBtn.setTextSize(12f);
        clearBtn.setTextColor(0xFFB8B8B8);
        clearBtn.setPadding(dpInt(8), dpInt(4), dpInt(4), dpInt(4));
        clearBtn.setOnClickListener(v -> {
            agent.clearHistory();
            chatAdapter.clear();
        });
        topBar.addView(clearBtn);

        root.addView(topBar, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(divider());

        flipper = new ViewFlipper(this);
        flipper.addView(buildChatTab());
        flipper.addView(buildReposTab());
        flipper.addView(buildModelsTab());
        flipper.addView(buildActionsTab());
        root.addView(flipper, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        root.addView(divider());

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
        chatRecycler.setItemAnimator(null);
        chatAdapter = new ChatAdapter();
        chatRecycler.setAdapter(chatAdapter);
        box.addView(chatRecycler, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        box.addView(divider());

        LinearLayout composer = new LinearLayout(this);
        composer.setOrientation(LinearLayout.HORIZONTAL);
        composer.setGravity(Gravity.BOTTOM);
        composer.setBackgroundColor(0xFF181818);
        composer.setPadding(dpInt(10), dpInt(8), dpInt(10), dpInt(8));

        chatInput = styledEdit("Message...");
        chatInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        chatInput.setMinLines(1);
        chatInput.setMaxLines(5);
        composer.addView(chatInput, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        composer.addView(spaceW(8));

        sendStopBtn = primaryBtn("Send", v -> onSendOrStopClicked());
        composer.addView(sendStopBtn);

        box.addView(composer, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return box;
    }

    private View buildReposTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dpInt(12), dpInt(12), dpInt(12), dpInt(16));

        box.addView(sectionTitle("GitHub Token"));
        tokenInput = styledEdit("ghp_... or github_pat_...");
        tokenInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        tokenInput.setText(prefs.getString("github_token", ""));
        box.addView(tokenInput);

        LinearLayout tokBtns = new LinearLayout(this);
        tokBtns.setOrientation(LinearLayout.HORIZONTAL);
        tokBtns.setPadding(0, dpInt(6), 0, dpInt(6));
        tokBtns.addView(primaryBtn("Connect", v -> {
            prefs.edit().putString("github_token", tokenInput.getText().toString().trim()).apply();
            verifyTokenAndLoadRepos(false);
        }));
        tokBtns.addView(spaceW(8));
        tokBtns.addView(smallBtn("Clear", v -> {
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
        box.addView(sectionTitle("Active Repository"));
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
        syncRow.addView(primaryBtn("Select Repo", v -> {
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
        syncRow.addView(smallBtn("Files", v -> openRepoFileBrowser()));
        box.addView(syncRow);

        repoChangesText = infoText("");
        box.addView(repoChangesText);

        box.addView(spaceH(8));
        box.addView(sectionTitle("Repositories"));
        repoFilterInput = styledEdit("Filter...");
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

        box.addView(sectionTitle("Model"));
        activeModelInfoText = infoText("No model selected");
        box.addView(activeModelInfoText);

        LinearLayout paramRow = new LinearLayout(this);
        paramRow.setOrientation(LinearLayout.HORIZONTAL);
        paramRow.setPadding(0, dpInt(6), 0, dpInt(6));
        ctxBtn = smallBtn("Ctx: " + nCtx, v -> {
            nCtx = (nCtx == 1024) ? 2048 : (nCtx == 2048 ? 4096 : 1024);
            prefs.edit().putInt("n_ctx", nCtx).apply();
            ctxBtn.setText("Ctx: " + nCtx);
        });
        threadsBtn = smallBtn("Threads: " + nThreads, v -> {
            nThreads = (nThreads == 2) ? 3 : (nThreads == 3 ? 4 : (nThreads == 4 ? 6 : 2));
            prefs.edit().putInt("n_threads", nThreads).apply();
            threadsBtn.setText("Threads: " + nThreads);
        });
        tempBtn = smallBtn(String.format(Locale.US, "T:%.2f", temperature), v -> {
            temperature = (temperature < 0.05f) ? 0.15f : (temperature < 0.30f ? 0.40f : 0.0f);
            prefs.edit().putFloat("temperature", temperature).apply();
            tempBtn.setText(String.format(Locale.US, "T:%.2f", temperature));
        });
        coolBtn = smallBtn(coolDelayMs > 0 ? "Cool" : "Turbo", v -> {
            coolDelayMs = (coolDelayMs > 0) ? 0 : 2;
            prefs.edit().putInt("cool_delay_ms", coolDelayMs).apply();
            coolBtn.setText(coolDelayMs > 0 ? "Cool" : "Turbo");
        });
        paramRow.addView(ctxBtn);
        paramRow.addView(spaceW(4));
        paramRow.addView(threadsBtn);
        paramRow.addView(spaceW(4));
        paramRow.addView(tempBtn);
        paramRow.addView(spaceW(4));
        paramRow.addView(coolBtn);
        box.addView(paramRow);

        LinearLayout loadRow = new LinearLayout(this);
        loadRow.setOrientation(LinearLayout.HORIZONTAL);
        loadRow.setPadding(0, dpInt(4), 0, dpInt(8));
        loadRow.addView(primaryBtn("Load", v -> loadSelectedModel(false)));
        loadRow.addView(spaceW(6));
        loadRow.addView(smallBtn("Unload", v -> {
            LlamaEngine.unload();
            refreshHeader();
            activeModelInfoText.setText(selectedModelName.isEmpty() ? "No model selected" : selectedModelName);
        }));
        loadRow.addView(spaceW(6));
        loadRow.addView(smallBtn("Pick .gguf", v -> modelPicker.launch(new String[]{"*/*"})));
        box.addView(loadRow);

        box.addView(sectionTitle("Local Models"));
        localModelsBox = new LinearLayout(this);
        localModelsBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(localModelsBox);

        box.addView(spaceH(12));
        box.addView(sectionTitle("Download Model"));

        downloadProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        downloadProgress.setMax(100);
        downloadProgress.setVisibility(View.GONE);
        box.addView(downloadProgress);

        LinearLayout dlStatusRow = new LinearLayout(this);
        dlStatusRow.setOrientation(LinearLayout.HORIZONTAL);
        dlStatusRow.setGravity(Gravity.CENTER_VERTICAL);
        downloadStatusText = infoText("");
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
        customRow.addView(primaryBtn("Download URL", v -> {
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
        box.addView(sectionTitle("Workflow Runs"));
        runsBox = new LinearLayout(this);
        runsBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(runsBox);

        box.addView(spaceH(12));
        box.addView(sectionTitle("Bash Sandbox"));
        bashInput = styledEdit("e.g. ls -la");
        box.addView(bashInput);
        LinearLayout bashBtnRow = new LinearLayout(this);
        bashBtnRow.setOrientation(LinearLayout.HORIZONTAL);
        bashBtnRow.setPadding(0, dpInt(6), 0, dpInt(6));
        bashBtnRow.addView(primaryBtn("Run", v -> runManualBash()));
        box.addView(bashBtnRow);

        bashOutputText = new TextView(this);
        bashOutputText.setTypeface(Typeface.MONOSPACE);
        bashOutputText.setTextSize(12f);
        bashOutputText.setTextColor(Color.WHITE);
        bashOutputText.setBackgroundColor(0xFF161616);
        bashOutputText.setPadding(dpInt(10), dpInt(10), dpInt(10), dpInt(10));
        bashOutputText.setTextIsSelectable(true);
        bashOutputText.setText("");
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
    // Chat / Autonomous Agent Loop
    // -------------------------------------------------------------------------
    private void onSendOrStopClicked() {
        if (aiBusy) {
            agent.abort();
            sendStopBtn.setText("...");
            return;
        }
        String text = chatInput.getText().toString().trim();
        if (text.isEmpty()) return;
        chatInput.setText("");

        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.USER, "", text));
        scrollToBottom();

        aiBusy = true;
        sendStopBtn.setText("Stop");
        streamingPos = -1;
        statusPos = -1;
        receivedFirstToken = false;

        String token = prefs.getString("github_token", "");
        aiExec.execute(() -> {
            agent.runTurn(text, token, activeWorkspace, activeOwner, activeRepo, activeBranch,
                8, temperature, coolDelayMs, true, new AgentRuntime.Listener() {
                @Override
                public void onStatus(String status) {
                    runOnUiThread(() -> {
                        if (receivedFirstToken) return;
                        if (statusPos < 0) {
                            statusPos = chatAdapter.getItemCount();
                            chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.STATUS, "", status));
                        } else {
                            chatAdapter.replaceItem(statusPos,
                                new ChatAdapter.Item(ChatAdapter.ItemType.STATUS, "", status));
                        }
                        scrollToBottom();
                    });
                }

                @Override
                public void onStreamToken(String tok) {
                    runOnUiThread(() -> {
                        clearStatusItem();
                        if (streamingPos < 0) {
                            streamingPos = chatAdapter.startStreamingAssistant();
                            receivedFirstToken = false;
                        }
                        if (!receivedFirstToken) {
                            receivedFirstToken = true;
                            chatAdapter.replaceItem(streamingPos,
                                new ChatAdapter.Item(ChatAdapter.ItemType.ASSISTANT, "", tok));
                        } else {
                            chatAdapter.appendStream(streamingPos, tok);
                        }
                        scrollToBottom();
                    });
                }

                @Override
                public void onCodeMessage(String title, String lang, String code) {
                    runOnUiThread(() -> {
                        clearStatusItem();
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.CODE_MESSAGE, title, lang, code));
                        scrollToBottom();
                    });
                }

                @Override
                public void onBlocked(String toolName, String reason) {
                    runOnUiThread(() -> {
                        clearStatusItem();
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.BLOCKED, "Safety Guard", reason));
                        scrollToBottom();
                    });
                }

                @Override
                public void onAssistantMessage(String message) {
                    runOnUiThread(() -> {
                        clearStatusItem();
                        if (streamingPos >= 0) {
                            chatAdapter.replaceItem(streamingPos,
                                new ChatAdapter.Item(ChatAdapter.ItemType.ASSISTANT, "", message));
                            streamingPos = -1;
                        } else {
                            chatAdapter.addItem(new ChatAdapter.Item(
                                ChatAdapter.ItemType.ASSISTANT, "", message));
                        }
                        receivedFirstToken = false;
                        scrollToBottom();
                    });
                }

                @Override
                public void onError(String error) {
                    runOnUiThread(() -> {
                        clearStatusItem();
                        finishOrDropStream();
                        chatAdapter.addItem(new ChatAdapter.Item(ChatAdapter.ItemType.ERROR, "", error));
                        scrollToBottom();
                    });
                }

                @Override
                public void onRepoChanged() {
                    runOnUiThread(() -> {
                        refreshHeader();
                        refreshRepoChangesUi();
                    });
                }
            });

            runOnUiThread(() -> {
                aiBusy = false;
                sendStopBtn.setText("Send");
            });
        });
    }

    private void clearStatusItem() {
        if (statusPos >= 0) {
            chatAdapter.removeItem(statusPos);
            if (streamingPos > statusPos) streamingPos--;
            statusPos = -1;
        }
    }

    private void finishOrDropStream() {
        if (streamingPos >= 0) {
            chatAdapter.removeItem(streamingPos);
            streamingPos = -1;
        }
        receivedFirstToken = false;
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
        accountText.setText("Connecting...");
        netExec.execute(() -> {
            try {
                GitHubClient.UserInfo user = GitHubClient.verifyToken(token);
                List<GitHubClient.RepoInfo> repos = GitHubClient.listRepositories(token);
                runOnUiThread(() -> {
                    accountText.setText("@" + user.login + " (" + repos.size() + " repos)");
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
            title.setText(r.fullName + (r.isPrivate ? " [private]" : ""));
            title.setTextColor(Color.WHITE);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            title.setTextSize(14f);
            TextView sub = new TextView(this);
            sub.setText(r.defaultBranch + (r.language.isEmpty() ? "" : " • " + r.language));
            sub.setTextColor(0xFFC0C0C0);
            sub.setTextSize(12f);
            sub.setPadding(0, dpInt(2), 0, dpInt(6));
            card.addView(title);
            card.addView(sub);
            card.addView(smallBtn("Select", v -> {
                selectAndSyncRepo(r.owner, r.name, r.defaultBranch);
                switchTab(0);
            }));
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
        refreshHeader();
        syncActiveRepo();
    }

    private void syncActiveRepo() {
        if (activeOwner.isEmpty() || activeRepo.isEmpty()) return;
        String token = prefs.getString("github_token", "").trim();
        if (token.isEmpty()) return;
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
                    refreshHeader();
                    refreshRepoChangesUi();
                    toast(msg);
                });
            } catch (Exception e) {
                runOnUiThread(() -> toast("Sync error: " + e.getMessage()));
            }
        });
    }

    private void refreshRepoChangesUi() {
        if (repoChangesText == null) return;
        if (activeWorkspace == null || !activeWorkspace.repoRoot.isDirectory()) {
            repoChangesText.setText("");
            return;
        }
        try {
            repoChangesText.setText(activeWorkspace.statusSummary());
        } catch (Exception e) {
            repoChangesText.setText("");
        }
    }

    private void openRepoFileBrowser() {
        if (activeWorkspace == null || !activeWorkspace.repoRoot.isDirectory()) {
            toast("Select a repository first");
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
                .setTitle(activeOwner + "/" + activeRepo)
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
                .setPositiveButton("Ask AI", (d, w) -> {
                    switchTab(0);
                    chatInput.setText("Check " + relPath);
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
            workflowsBox.addView(infoText("Connect token and select a repository in Repos."));
            runsBox.removeAllViews();
            return;
        }
        workflowsBox.removeAllViews();
        workflowsBox.addView(infoText("Loading..."));
        netExec.execute(() -> {
            try {
                List<GitHubClient.WorkflowInfo> wfs = GitHubClient.listWorkflows(token, activeOwner, activeRepo);
                List<GitHubClient.RunInfo> runs = GitHubClient.listWorkflowRuns(token, activeOwner, activeRepo);
                runOnUiThread(() -> {
                    workflowsBox.removeAllViews();
                    if (wfs.isEmpty()) workflowsBox.addView(infoText("No workflows"));
                    for (GitHubClient.WorkflowInfo w : wfs) {
                        LinearLayout card = cardContainer();
                        card.addView(infoText(w.name + " (" + w.path + ")"));
                        card.addView(smallBtn("Run (" + activeBranch + ")", v -> triggerWorkflowUi(w)));
                        workflowsBox.addView(card);
                    }
                    runsBox.removeAllViews();
                    if (runs.isEmpty()) runsBox.addView(infoText("No runs yet"));
                    for (GitHubClient.RunInfo r : runs) {
                        LinearLayout card = cardContainer();
                        String statusLine = "#" + r.id + " " + r.name + "\n"
                            + r.status + (r.conclusion.isEmpty() ? "" : " / " + r.conclusion)
                            + " • " + r.branch;
                        card.addView(infoText(statusLine));
                        card.addView(smallBtn("Jobs", v -> showRunJobsDialog(r.id)));
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
                    .setTitle("Run #" + runId)
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
            toast("Select a repository first");
            return;
        }
        String cmd = bashInput.getText().toString().trim();
        if (cmd.isEmpty()) return;
        bashOutputText.setText("$ " + cmd + "\n...");
        netExec.execute(() -> {
            String out = activeWorkspace.runBash(cmd);
            runOnUiThread(() -> {
                bashOutputText.setText("$ " + cmd + "\n" + out);
                refreshRepoChangesUi();
            });
        });
    }

    // -------------------------------------------------------------------------
    // Models Management
    // -------------------------------------------------------------------------
    private void onPickedModelUri(Uri uri) {
        if (uri == null) return;
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        String name = "model.gguf";
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
                toast("Not a valid GGUF file");
                return;
            }
        } catch (Exception e) {
            toast("Cannot read file: " + e.getMessage());
            return;
        }
        ModelCatalog.addPickedUriModel(prefs, name, uri.toString(), size);
        selectModel(name, uri.toString(), true);
        refreshLocalModelsList();
        loadSelectedModel(false);
    }

    private void startModelDownload(String url, String filename) {
        if (downloading) {
            toast("Download already in progress");
            return;
        }
        downloading = true;
        downloadCancel.set(false);
        downloadProgress.setVisibility(View.VISIBLE);
        downloadProgress.setProgress(0);
        downloadStatusText.setVisibility(View.VISIBLE);
        downloadStatusText.setText("Downloading...");
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
                    loadSelectedModel(false);
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
        refreshHeader();
    }

    private void loadSelectedModel(boolean silent) {
        if (selectedModelPathOrUri.isEmpty()) {
            if (!silent) toast("Select a GGUF model first");
            return;
        }
        if (activeModelInfoText != null) activeModelInfoText.setText("Loading " + selectedModelName + "...");
        aiExec.execute(() -> {
            String res;
            if (selectedModelIsUri) {
                res = LlamaEngine.loadFromUri(getContentResolver(), Uri.parse(selectedModelPathOrUri), nCtx, nThreads);
            } else {
                res = LlamaEngine.loadFromFile(selectedModelPathOrUri, nCtx, nThreads);
            }
            runOnUiThread(() -> {
                refreshHeader();
                if (activeModelInfoText != null) {
                    activeModelInfoText.setText(res.startsWith("OK:") ? selectedModelName : res);
                }
                if (!silent) toast(res.startsWith("OK:") ? "Loaded " + selectedModelName : res);
            });
        });
    }

    private void refreshLocalModelsList() {
        if (localModelsBox == null) return;
        localModelsBox.removeAllViews();
        List<ModelCatalog.LocalModel> list = ModelCatalog.listLocalModels(this, prefs);
        if (list.isEmpty()) {
            localModelsBox.addView(infoText("No local models yet."));
            return;
        }
        for (ModelCatalog.LocalModel m : list) {
            boolean selected = m.pathOrUri.equals(selectedModelPathOrUri);
            LinearLayout card = cardContainer();
            String sz = m.sizeBytes > 0 ? String.format(Locale.US, " (%.1f MB)", m.sizeBytes / (1024.0 * 1024.0)) : "";
            card.addView(infoText((selected ? "● " : "") + m.name + sz));

            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(0, dpInt(4), 0, 0);
            row.addView(smallBtn("Use", v -> {
                selectModel(m.name, m.pathOrUri, m.isUri);
                refreshLocalModelsList();
                loadSelectedModel(false);
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

    private void refreshHeader() {
        if (!activeOwner.isEmpty() && !activeRepo.isEmpty()) {
            headerTitleText.setText(activeOwner + "/" + activeRepo);
        } else if (!selectedModelName.isEmpty()) {
            headerTitleText.setText(selectedModelName);
        } else {
            headerTitleText.setText("Obsidian AI");
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
