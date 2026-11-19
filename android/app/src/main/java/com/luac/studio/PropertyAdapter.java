package com.luac.studio;

import android.graphics.Color;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.ArrayList;
import java.util.Locale;

final class PropertyAdapter extends RecyclerView.Adapter<PropertyAdapter.VH> {
    interface Listener {
        void onEditProperty(SceneModel.Node node, String key);
    }

    static final class PropRow {
        final String key, value;
        final boolean isColor;
        final int color;
        PropRow(String key, String value, boolean isColor, int color) {
            this.key = key; this.value = value; this.isColor = isColor; this.color = color;
        }
    }

    private final ArrayList<PropRow> rows = new ArrayList<>();
    private final Listener listener;
    private SceneModel.Node node;

    PropertyAdapter(Listener listener) {
        this.listener = listener;
    }

    void setNode(SceneModel.Node node) {
        this.node = node;
        rows.clear();
        if (node != null) {
            rows.add(new PropRow("Name", node.name, false, 0));
            rows.add(new PropRow("ClassName", node.className, false, 0));
            if (node.is3DPart()) {
                rows.add(new PropRow("Color", String.format(Locale.US, "%d, %d, %d",
                    Color.red(node.color), Color.green(node.color), Color.blue(node.color)), true, node.color));
                rows.add(new PropRow("Material", node.material, false, 0));
                if (!"SpawnLocation".equals(node.className)) {
                    rows.add(new PropRow("Shape", node.shape(), false, 0));
                }
                rows.add(new PropRow("Transparency", String.format(Locale.US, "%.2f", node.transparency), false, 0));
                rows.add(new PropRow("Position", fmt3(node.posX, node.posY, node.posZ), false, 0));
                rows.add(new PropRow("Size", fmt3(node.sizeX, node.sizeY, node.sizeZ), false, 0));
                rows.add(new PropRow("Rotation", fmt3(node.rotX, node.rotY, node.rotZ), false, 0));
                rows.add(new PropRow("Anchored", node.anchored ? "☑ true" : "☐ false", false, 0));
                rows.add(new PropRow("CanCollide", node.canCollide ? "☑ true" : "☐ false", false, 0));
                rows.add(new PropRow("CastShadow", node.castShadow ? "☑ true" : "☐ false", false, 0));
                rows.add(new PropRow("Locked", node.locked ? "☑ true" : "☐ false", false, 0));
            } else if ("Lighting".equals(node.className)) {
                rows.add(new PropRow("ClockTime", String.format(Locale.US, "%.1f", node.clockTime), false, 0));
                rows.add(new PropRow("Brightness", String.format(Locale.US, "%.1f", node.brightness), false, 0));
            } else if (node.isUi()) {
                if ("TextLabel".equals(node.className)) {
                    rows.add(new PropRow("Text", node.text, false, 0));
                    rows.add(new PropRow("Font", node.fontName, false, 0));
                    rows.add(new PropRow("TextSize", String.valueOf(node.textSize), false, 0));
                }
                rows.add(new PropRow("Color", String.format(Locale.US, "%d, %d, %d",
                    Color.red(node.color), Color.green(node.color), Color.blue(node.color)), true, node.color));
                rows.add(new PropRow("Position", String.format(Locale.US, "%.0f, %.0f", node.posX, node.posY), false, 0));
            } else if (node.isScript()) {
                rows.add(new PropRow("Disabled", node.disabled ? "☑ true" : "☐ false", false, 0));
                rows.add(new PropRow("Source", "Tap to Edit Code →", false, 0));
            }
        }
        notifyDataSetChanged();
    }

    private static String fmt3(float a, float b, float c) {
        return String.format(Locale.US, "%.1f, %.1f, %.1f", a, b, c);
    }

    @NonNull @Override public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        return new VH(LayoutInflater.from(parent.getContext()).inflate(R.layout.item_property, parent, false));
    }

    @Override public void onBindViewHolder(@NonNull VH h, int pos) {
        PropRow row = rows.get(pos);
        h.itemView.setBackgroundColor((pos % 2 == 0) ? 0xFF22242A : 0xFF1D1F24);
        h.key.setText(row.key);
        h.value.setText(row.value);
        if (row.isColor) {
            h.swatch.setVisibility(View.VISIBLE);
            h.swatch.setBackgroundColor(row.color);
        } else {
            h.swatch.setVisibility(View.GONE);
        }
        h.itemView.setOnClickListener(v -> {
            if (node != null) listener.onEditProperty(node, row.key);
        });
    }

    @Override public int getItemCount() { return rows.size(); }

    static final class VH extends RecyclerView.ViewHolder {
        final TextView key, value;
        final View swatch;
        VH(@NonNull View v) {
            super(v);
            key = v.findViewById(R.id.prop_key);
            value = v.findViewById(R.id.prop_value);
            swatch = v.findViewById(R.id.prop_swatch);
        }
    }
}
