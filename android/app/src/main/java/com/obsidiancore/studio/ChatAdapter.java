package com.obsidiancore.studio;

import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
import java.util.List;

final class ChatAdapter extends RecyclerView.Adapter<ChatAdapter.Holder> {
    enum ItemType {
        USER,
        THOUGHT,
        TOOL_CALL,
        TOOL_RESULT,
        DIFF,
        BLOCKED,
        ASSISTANT,
        ERROR
    }

    static final class Item {
        final ItemType type;
        final String title;
        String body;

        Item(ItemType type, String title, String body) {
            this.type = type;
            this.title = title;
            this.body = body;
        }
    }

    private final List<Item> items = new ArrayList<>();

    void addItem(Item item) {
        items.add(item);
        notifyItemInserted(items.size() - 1);
    }

    int startStreamingAssistant() {
        items.add(new Item(ItemType.ASSISTANT, "Obsidian AI", ""));
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
        rlp.setMargins((int) (10 * dp), (int) (5 * dp), (int) (10 * dp), (int) (5 * dp));
        wrap.setLayoutParams(rlp);

        LinearLayout card = new LinearLayout(parent.getContext());
        card.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (12 * dp);
        card.setPadding(pad, (int) (9 * dp), pad, (int) (9 * dp));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        card.setLayoutParams(clp);

        TextView titleView = new TextView(parent.getContext());
        titleView.setTextSize(11.5f);
        titleView.setTypeface(Typeface.DEFAULT_BOLD);
        titleView.setTextColor(0xFFBDBDBD);

        TextView bodyView = new TextView(parent.getContext());
        bodyView.setTextSize(13.5f);
        bodyView.setTextColor(Color.WHITE);
        bodyView.setPadding(0, (int) (4 * dp), 0, 0);
        bodyView.setTextIsSelectable(true);

        card.addView(titleView);
        card.addView(bodyView);
        wrap.addView(card);
        return new Holder(wrap, card, titleView, bodyView, dp);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder h, int position) {
        Item it = items.get(position);
        h.titleView.setText(it.title);
        h.bodyView.setText(it.body);

        int bgColor = 0xFF1A1A1A;
        int strokeColor = 0xFF343434;
        boolean mono = false;
        h.wrap.setGravity(Gravity.START);

        switch (it.type) {
            case USER:
                bgColor = 0xFF2D2D2D;
                strokeColor = 0xFF565656;
                h.titleView.setTextColor(0xFFE0E0E0);
                break;
            case THOUGHT:
                bgColor = 0xFF161616;
                strokeColor = 0xFF3A3A3A;
                h.titleView.setTextColor(0xFFA8A8A8);
                h.bodyView.setTextColor(0xFFD0D0D0);
                break;
            case TOOL_CALL:
                bgColor = 0xFF1E1E1E;
                strokeColor = 0xFF454545;
                mono = true;
                h.titleView.setTextColor(0xFFC8C8C8);
                break;
            case DIFF:
                bgColor = 0xFF151515;
                strokeColor = 0xFF5C5C5C;
                mono = true;
                h.titleView.setTextColor(0xFFFFFFFF);
                break;
            case TOOL_RESULT:
                bgColor = 0xFF141414;
                strokeColor = 0xFF323232;
                mono = true;
                h.titleView.setTextColor(0xFFB0B0B0);
                break;
            case BLOCKED:
                bgColor = 0xFF262626;
                strokeColor = 0xFF8A8A8A;
                mono = true;
                h.titleView.setTextColor(0xFFFFFFFF);
                break;
            case ERROR:
                bgColor = 0xFF242424;
                strokeColor = 0xFF707070;
                h.titleView.setTextColor(0xFFFFFFFF);
                break;
            case ASSISTANT:
            default:
                bgColor = 0xFF1B1B1B;
                strokeColor = 0xFF3E3E3E;
                h.titleView.setTextColor(0xFFE6E6E6);
                h.bodyView.setTextColor(0xFFFFFFFF);
                break;
        }

        h.bodyView.setTypeface(mono ? Typeface.MONOSPACE : Typeface.DEFAULT);
        h.bodyView.setTextSize(mono ? 12.0f : 14.0f);

        GradientDrawable gd = new GradientDrawable();
        gd.setColor(bgColor);
        gd.setCornerRadius(10 * h.dp);
        gd.setStroke((int) Math.max(1, h.dp), strokeColor);
        h.card.setBackground(gd);
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final LinearLayout wrap;
        final LinearLayout card;
        final TextView titleView;
        final TextView bodyView;
        final float dp;

        Holder(LinearLayout wrap, LinearLayout card, TextView titleView, TextView bodyView, float dp) {
            super(wrap);
            this.wrap = wrap;
            this.card = card;
            this.titleView = titleView;
            this.bodyView = bodyView;
            this.dp = dp;
        }
    }
}
