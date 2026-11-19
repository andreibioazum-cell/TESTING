package com.luac.studio;

import android.graphics.drawable.GradientDrawable;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;

final class ExplorerAdapter extends RecyclerView.Adapter<ExplorerAdapter.VH> {
    interface Listener {
        void onSelect(SceneModel.Node node);
        void onOpenScript(SceneModel.Node node);
        void onInsertChild(SceneModel.Node parent);
    }

    static final class Row {
        final SceneModel.Node node;
        final int depth;
        Row(SceneModel.Node node, int depth) { this.node = node; this.depth = depth; }
    }

    private final ArrayList<Row> rows = new ArrayList<>();
    private final Listener listener;
    private SceneModel model;
    private SceneModel.Node selected;

    ExplorerAdapter(Listener listener) {
        this.listener = listener;
    }

    void update(SceneModel model, SceneModel.Node selected) {
        this.model = model;
        this.selected = selected;
        rebuild();
    }

    void setSelected(SceneModel.Node selected) {
        this.selected = selected;
        notifyDataSetChanged();
    }

    void rebuild() {
        rows.clear();
        if (model != null) {
            for (SceneModel.Node c : model.root.children) {
                flatten(c, 0);
            }
        }
        notifyDataSetChanged();
    }

    private void flatten(SceneModel.Node n, int depth) {
        rows.add(new Row(n, depth));
        if (n.expanded) {
            for (SceneModel.Node c : n.children) {
                flatten(c, depth + 1);
            }
        }
    }

    @NonNull @Override public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        return new VH(LayoutInflater.from(parent.getContext()).inflate(R.layout.item_explorer, parent, false));
    }

    @Override public void onBindViewHolder(@NonNull VH h, int pos) {
        Row row = rows.get(pos);
        SceneModel.Node n = row.node;
        float density = h.itemView.getResources().getDisplayMetrics().density;
        h.itemView.setPadding((int) ((4 + row.depth * 12) * density), 0, (int) (4 * density), 0);

        boolean isSel = n == selected;
        h.itemView.setBackgroundColor(isSel ? 0xFF28559A : 0x00000000);

        if (n.children.isEmpty()) {
            h.arrow.setText("");
            h.arrow.setOnClickListener(null);
        } else {
            h.arrow.setText(n.expanded ? "▾" : "▸");
            h.arrow.setOnClickListener(v -> {
                n.expanded = !n.expanded;
                rebuild();
            });
        }

        h.badge.setText(badgeLetter(n.className));
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(3f * density);
        bg.setColor(badgeColor(n.className));
        h.badge.setBackground(bg);

        h.name.setText(n.name);
        h.itemView.setOnClickListener(v -> {
            selected = n;
            notifyDataSetChanged();
            listener.onSelect(n);
            if (n.isScript()) listener.onOpenScript(n);
        });
        h.addBtn.setOnClickListener(v -> listener.onInsertChild(n));
    }

    @Override public int getItemCount() { return rows.size(); }

    private static String badgeLetter(String cls) {
        if ("Workspace".equals(cls)) return "W";
        if ("SpawnLocation".equals(cls)) return "S";
        if ("Script".equals(cls) || "LocalScript".equals(cls)) return "L";
        if ("Lighting".equals(cls)) return "☀";
        if ("StarterGui".equals(cls) || "ScreenGui".equals(cls)) return "G";
        if ("TextLabel".equals(cls) || "Frame".equals(cls)) return "T";
        if ("Players".equals(cls) || "StarterPlayer".equals(cls)) return "☺";
        if ("Camera".equals(cls)) return "C";
        if ("WedgePart".equals(cls)) return "◢";
        if ("Sphere".equals(cls)) return "●";
        if ("Cylinder".equals(cls)) return "▮";
        if ("Folder".equals(cls)) return "F";
        return "■";
    }

    private static int badgeColor(String cls) {
        if ("Workspace".equals(cls)) return 0xFF2563EB;
        if ("Script".equals(cls) || "LocalScript".equals(cls)) return 0xFF16A34A;
        if ("Lighting".equals(cls)) return 0xFFD97706;
        if ("StarterGui".equals(cls) || "ScreenGui".equals(cls) || "TextLabel".equals(cls)) return 0xFF7C3AED;
        if ("SpawnLocation".equals(cls)) return 0xFF4B5563;
        if ("Players".equals(cls) || "StarterPlayer".equals(cls)) return 0xFF0891B2;
        if ("Part".equals(cls) || "WedgePart".equals(cls) || "Sphere".equals(cls) || "Cylinder".equals(cls)) return 0xFF3B82F6;
        return 0xFF475569;
    }

    static final class VH extends RecyclerView.ViewHolder {
        final TextView arrow, badge, name, addBtn;
        VH(@NonNull View v) {
            super(v);
            arrow = v.findViewById(R.id.arrow);
            badge = v.findViewById(R.id.badge);
            name = v.findViewById(R.id.name);
            addBtn = v.findViewById(R.id.add_btn);
        }
    }
}
