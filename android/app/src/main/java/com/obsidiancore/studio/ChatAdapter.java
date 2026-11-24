package com.obsidiancore.studio;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ChatAdapter extends RecyclerView.Adapter<ChatAdapter.Holder> {
    enum ItemType {
        USER,
        ASSISTANT,
        CODE_MESSAGE,
        STATUS,
        BLOCKED,
        ERROR
    }

    static final class Item {
        final ItemType type;
        final String title;
        final String lang;
        String body;

        Item(ItemType type, String title, String body) {
            this(type, title, "", body);
        }

        Item(ItemType type, String title, String lang, String body) {
            this.type = type;
            this.title = title;
            this.lang = lang == null ? "" : lang;
            this.body = body == null ? "" : body;
        }
    }

    private static final Pattern CODE_FENCE = Pattern.compile("```([a-zA-Z0-9_.+-]*)\\s*\\n([\\s\\S]*?)```");
    private final List<Item> items = new ArrayList<>();

    void addItem(Item item) {
        items.add(item);
        notifyItemInserted(items.size() - 1);
    }

    int startStreamingAssistant() {
        items.add(new Item(ItemType.ASSISTANT, "", ""));
        int pos = items.size() - 1;
        notifyItemInserted(pos);
        return pos;
    }

    void appendStream(int pos, String chunk) {
        if (pos < 0 || pos >= items.size()) return;
        items.get(pos).body += chunk;
        notifyItemChanged(pos);
    }

    void replaceItem(int pos, Item item) {
        if (pos < 0 || pos >= items.size()) return;
        items.set(pos, item);
        notifyItemChanged(pos);
    }

    void removeItem(int pos) {
        if (pos < 0 || pos >= items.size()) return;
        items.remove(pos);
        notifyItemRemoved(pos);
    }

    void clear() {
        items.clear();
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        float dp = parent.getResources().getDisplayMetrics().density;
        LinearLayout wrap = new LinearLayout(parent.getContext());
        wrap.setOrientation(LinearLayout.VERTICAL);
        RecyclerView.LayoutParams rlp = new RecyclerView.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.setMargins((int) (10 * dp), (int) (4 * dp), (int) (10 * dp), (int) (4 * dp));
        wrap.setLayoutParams(rlp);

        LinearLayout card = new LinearLayout(parent.getContext());
        card.setOrientation(LinearLayout.VERTICAL);
        card.setLayoutParams(new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        wrap.addView(card);
        return new Holder(wrap, card, dp);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder h, int position) {
        Item it = items.get(position);
        Context ctx = h.card.getContext();
        float dp = h.dp;
        h.card.removeAllViews();

        if (it.type == ItemType.STATUS) {
            h.wrap.setGravity(Gravity.START);
            h.card.setPadding((int) (10 * dp), (int) (4 * dp), (int) (10 * dp), (int) (4 * dp));
            h.card.setBackground(null);
            TextView tv = new TextView(ctx);
            tv.setText(it.body);
            tv.setTextSize(12f);
            tv.setTextColor(0xFF9E9E9E);
            h.card.addView(tv);
            return;
        }

        int pad = (int) (12 * dp);
        h.card.setPadding(pad, (int) (9 * dp), pad, (int) (9 * dp));

        int bgColor = 0xFF1A1A1A;
        int strokeColor = 0xFF343434;
        if (it.type == ItemType.USER) {
            bgColor = 0xFF2B2B2B;
            strokeColor = 0xFF525252;
        } else if (it.type == ItemType.CODE_MESSAGE) {
            bgColor = 0xFF161616;
            strokeColor = 0xFF424242;
        } else if (it.type == ItemType.BLOCKED || it.type == ItemType.ERROR) {
            bgColor = 0xFF232323;
            strokeColor = 0xFF707070;
        }

        GradientDrawable gd = new GradientDrawable();
        gd.setColor(bgColor);
        gd.setCornerRadius(10 * dp);
        gd.setStroke((int) Math.max(1, dp), strokeColor);
        h.card.setBackground(gd);

        if (!it.title.isEmpty()) {
            TextView titleView = new TextView(ctx);
            titleView.setText(it.title);
            titleView.setTextSize(11.5f);
            titleView.setTypeface(Typeface.DEFAULT_BOLD);
            titleView.setTextColor(0xFFC8C8C8);
            titleView.setPadding(0, 0, 0, (int) (4 * dp));
            h.card.addView(titleView);
        }

        if (it.type == ItemType.CODE_MESSAGE) {
            h.card.addView(buildCodeBlockView(ctx, dp, it.lang.isEmpty() ? "code" : it.lang, it.body));
            return;
        }

        if (it.type == ItemType.ASSISTANT) {
            renderMarkdownMessage(ctx, dp, h.card, it.body);
            return;
        }

        TextView bodyView = new TextView(ctx);
        bodyView.setText(it.body);
        bodyView.setTextSize(14f);
        bodyView.setTextColor(Color.WHITE);
        bodyView.setTextIsSelectable(true);
        h.card.addView(bodyView);
    }

    private void renderMarkdownMessage(Context ctx, float dp, LinearLayout container, String text) {
        if (text == null || text.isEmpty()) return;
        Matcher m = CODE_FENCE.matcher(text);
        int last = 0;
        boolean added = false;
        while (m.find()) {
            String before = text.substring(last, m.start()).trim();
            if (!before.isEmpty()) {
                container.addView(makeProseView(ctx, dp, before));
                added = true;
            }
            String lang = m.group(1) != null ? m.group(1).trim() : "";
            String code = m.group(2) != null ? m.group(2) : "";
            if (code.endsWith("\n")) code = code.substring(0, code.length() - 1);
            container.addView(buildCodeBlockView(ctx, dp, lang.isEmpty() ? "code" : lang, code));
            added = true;
            last = m.end();
        }
        String tail = text.substring(last).trim();
        if (!tail.isEmpty() || !added) {
            // Handle unclosed ``` fence during live streaming
            int openFence = tail.indexOf("```");
            if (openFence >= 0) {
                String beforeFence = tail.substring(0, openFence).trim();
                if (!beforeFence.isEmpty()) {
                    container.addView(makeProseView(ctx, dp, beforeFence));
                }
                String rest = tail.substring(openFence + 3);
                int nl = rest.indexOf('\n');
                String lang = nl >= 0 ? rest.substring(0, nl).trim() : "code";
                String code = nl >= 0 ? rest.substring(nl + 1) : "";
                container.addView(buildCodeBlockView(ctx, dp, lang.isEmpty() ? "code" : lang, code));
            } else {
                container.addView(makeProseView(ctx, dp, tail));
            }
        }
    }

    private TextView makeProseView(Context ctx, float dp, String text) {
        TextView tv = new TextView(ctx);
        tv.setText(text);
        tv.setTextSize(14f);
        tv.setTextColor(Color.WHITE);
        tv.setTextIsSelectable(true);
        tv.setPadding(0, (int) (2 * dp), 0, (int) (4 * dp));
        return tv;
    }

    private View buildCodeBlockView(Context ctx, float dp, String label, String code) {
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, (int) (4 * dp), 0, (int) (4 * dp));
        box.setLayoutParams(lp);

        GradientDrawable gd = new GradientDrawable();
        gd.setColor(0xFF0D0D0D);
        gd.setCornerRadius(8 * dp);
        gd.setStroke((int) Math.max(1, dp), 0xFF3E3E3E);
        box.setBackground(gd);

        LinearLayout hdr = new LinearLayout(ctx);
        hdr.setOrientation(LinearLayout.HORIZONTAL);
        hdr.setGravity(Gravity.CENTER_VERTICAL);
        hdr.setBackgroundColor(0xFF1F1F1F);
        hdr.setPadding((int) (10 * dp), (int) (4 * dp), (int) (10 * dp), (int) (4 * dp));

        TextView langTv = new TextView(ctx);
        langTv.setText(label);
        langTv.setTextSize(11f);
        langTv.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        langTv.setTextColor(0xFFDADADA);
        hdr.addView(langTv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView copyTv = new TextView(ctx);
        copyTv.setText("Copy");
        copyTv.setTextSize(11f);
        copyTv.setTextColor(Color.WHITE);
        copyTv.setPadding((int) (8 * dp), (int) (2 * dp), (int) (4 * dp), (int) (2 * dp));
        copyTv.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                cm.setPrimaryClip(ClipData.newPlainText(label, code));
                Toast.makeText(ctx, "Copied", Toast.LENGTH_SHORT).show();
            }
        });
        hdr.addView(copyTv);
        box.addView(hdr);

        HorizontalScrollView hsv = new HorizontalScrollView(ctx);
        hsv.setFillViewport(true);
        TextView codeTv = new TextView(ctx);
        codeTv.setTypeface(Typeface.MONOSPACE);
        codeTv.setTextSize(12f);
        codeTv.setTextColor(0xFFF2F2F2);
        codeTv.setPadding((int) (10 * dp), (int) (8 * dp), (int) (10 * dp), (int) (8 * dp));
        codeTv.setTextIsSelectable(true);
        codeTv.setHorizontallyScrolling(true);
        codeTv.setText(formatCodeOrDiff(code));
        hsv.addView(codeTv);
        box.addView(hsv);
        return box;
    }

    private CharSequence formatCodeOrDiff(String code) {
        if (code == null || code.isEmpty()) return "";
        if (!code.contains("\n+ ") && !code.contains("\n- ") && !code.startsWith("+ ") && !code.startsWith("- ")) {
            return code;
        }
        SpannableStringBuilder ssb = new SpannableStringBuilder();
        String[] lines = code.split("\n", -1);
        for (int i = 0; i < lines.length; i++) {
            String l = lines[i];
            int start = ssb.length();
            ssb.append(l);
            int end = ssb.length();
            if (l.startsWith("+ ")) {
                ssb.setSpan(new BackgroundColorSpan(0xFF282828), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                ssb.setSpan(new ForegroundColorSpan(Color.WHITE), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                ssb.setSpan(new StyleSpan(Typeface.BOLD), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else if (l.startsWith("- ")) {
                ssb.setSpan(new BackgroundColorSpan(0xFF1A1A1A), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                ssb.setSpan(new ForegroundColorSpan(0xFFA8A8A8), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else if (l.startsWith("@@") || l.startsWith("---") || l.startsWith("+++")) {
                ssb.setSpan(new ForegroundColorSpan(0xFFBDBDBD), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
            if (i < lines.length - 1) ssb.append('\n');
        }
        return ssb;
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final LinearLayout wrap;
        final LinearLayout card;
        final float dp;

        Holder(LinearLayout wrap, LinearLayout card, float dp) {
            super(wrap);
            this.wrap = wrap;
            this.card = card;
            this.dp = dp;
        }
    }
}
