package dev.fusion.control;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Fusion Control settings screen.
 *  - shows daemon/pad status
 *  - one-tap hide toggle
 *  - per-pad layout and button remapping with reboot persistence -> /data/adb/fusion-layouts.conf
 *  - per-app "disable hide mode while focused" rules -> /data/adb/fusion-apps.conf
 * Everything goes through su + /data/adb/fusionctl (Magisk root, granted on first use).
 */
public class MainActivity extends Activity {

    private static final Pattern P_NPADS  = Pattern.compile("\"npads\":(\\d+)");
    private static final Pattern P_HIDE   = Pattern.compile("\"hide_mode\":(\\d)");

    public static class PadInfo {
        public int idx;
        public String name = "";
        public String node = "";
        public boolean hidden = false;
        public String layout = "DEFAULT";
        public String custom = "";
        public String routes = "";

        public String getDisplayName() {
            return (name != null && !name.trim().isEmpty()) ? name.trim() : "Gamepad #" + idx;
        }

        public String getLayoutDescription() {
            if ("NINTENDO".equalsIgnoreCase(layout)) {
                return "Nintendo (A↔B, X↔Y swapped)";
            } else if ("SWAP_AB".equalsIgnoreCase(layout)) {
                return "Swap A ↔ B Only";
            } else if ("SWAP_XY".equalsIgnoreCase(layout)) {
                return "Swap X ↔ Y Only";
            } else if ("CUSTOM".equalsIgnoreCase(layout)) {
                return "Custom Mapping" + (custom != null && !custom.isEmpty() ? " [" + custom + "]" : "");
            } else if ("XBOX".equalsIgnoreCase(layout)) {
                return "Xbox (Standard Layout)";
            } else {
                return "Default (Xbox Layout)";
            }
        }
    }

    private static final int[] BTN_CODES = {
        304, // BTN_SOUTH (A)
        305, // BTN_EAST (B)
        308, // BTN_WEST (X)
        307, // BTN_NORTH (Y)
        310, // BTN_TL (LB)
        311, // BTN_TR (RB)
        312, // BTN_TL2 (LT)
        313, // BTN_TR2 (RT)
        314, // BTN_SELECT
        315  // BTN_START
    };

    private static final String[] BTN_LABELS = {
        "Button A (Bottom / South)",
        "Button B (Right / East)",
        "Button X (Left / West)",
        "Button Y (Top / North)",
        "Left Bumper (LB / L1)",
        "Right Bumper (RB / R1)",
        "Left Trigger (LT / L2)",
        "Right Trigger (RT / R2)",
        "Select / Back",
        "Start / Menu"
    };

    private static final String[] TARGET_NAMES = {
        "Default (Unchanged)",
        "Action A (South)",
        "Action B (East)",
        "Action X (West)",
        "Action Y (North)",
        "Action LB (Left Bumper)",
        "Action RB (Right Bumper)",
        "Action LT (Left Trigger)",
        "Action RT (Right Trigger)",
        "Action Select",
        "Action Start"
    };

    private static final int[] TARGET_CODES = {
        0,
        304,
        305,
        308,
        307,
        310,
        311,
        312,
        313,
        314,
        315
    };

    private TextView status;
    private TextView log;
    private LinearLayout gamepadContainer;
    private LinearLayout appList;
    private Button hideBtn;
    private final Map<String, Boolean> rules = new TreeMap<>();
    private final List<PadInfo> padList = new ArrayList<>();
    private boolean hideOn = false;
    private String lastRaw = "";
    private int npads = 0;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);

        float d = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * d);

        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);

        TextView title = new TextView(this);
        title.setText("Fusion Controller");
        title.setTextSize(22);
        title.setTypeface(null, Typeface.BOLD);
        root.addView(title);

        status = new TextView(this);
        status.setText("loading…");
        status.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(status);

        hideBtn = new Button(this);
        hideBtn.setOnClickListener(v -> toggleHide());
        root.addView(hideBtn);

        Button restartBtn = new Button(this);
        restartBtn.setText("restart daemon & rescan pads");
        restartBtn.setOnClickListener(v -> restartDaemon());
        root.addView(restartBtn);

        Button refresh = new Button(this);
        refresh.setText("refresh");
        refresh.setOnClickListener(v -> refresh());
        root.addView(refresh);

        // --- Gamepad Remapping Section ---
        TextView remappingTitle = new TextView(this);
        remappingTitle.setText("\nGamepad Remapping");
        remappingTitle.setTextSize(18);
        remappingTitle.setTypeface(null, Typeface.BOLD);
        root.addView(remappingTitle);

        TextView remappingSub = new TextView(this);
        remappingSub.setText("Connected gamepads are shown below. Tap any gamepad to open the remapping menu (persists across reboots):");
        remappingSub.setPadding(0, 0, 0, (int) (6 * d));
        root.addView(remappingSub);

        gamepadContainer = new LinearLayout(this);
        gamepadContainer.setOrientation(LinearLayout.VERTICAL);
        root.addView(gamepadContainer);

        // --- Per-app hide rules Section ---
        TextView appsTitle = new TextView(this);
        appsTitle.setText("\nPer-App Hide Rules\nDisable hide mode for selected apps (keeps pads visible while focused):");
        appsTitle.setTypeface(null, Typeface.BOLD);
        root.addView(appsTitle);

        appList = new LinearLayout(this);
        appList.setOrientation(LinearLayout.VERTICAL);
        root.addView(appList);

        Button save = new Button(this);
        save.setText("save rules");
        save.setOnClickListener(v -> saveRules());
        root.addView(save);

        log = new TextView(this);
        log.setTextIsSelectable(true);
        root.addView(log);
    }

    @Override protected void onResume() { super.onResume(); refresh(); }

    // ---------------------------------------------------------------- state

    private void refresh() {
        status.setText("loading…");
        new Thread(() -> {
            final String raw = Su.run(
                "echo '--- id ---'; id; echo '--- status ---'; /data/adb/fusionctl status; " +
                "echo '--- conf ---'; cat /data/adb/fusion-apps.conf 2>/dev/null; " +
                "echo '--- layouts ---'; cat /data/adb/fusion-layouts.conf 2>/dev/null");
            String st = "", conf = "", layouts = "";
            String[] parts = raw.split("--- [a-z]+ ---");
            if (parts.length > 2) st = parts[2].trim();
            if (parts.length > 3) conf = parts[3].trim();
            if (parts.length > 4) layouts = parts[4].trim();
            final String stF = st, confF = conf, layoutsF = layouts, rawF = raw;
            runOnUiThread(() -> { lastRaw = rawF; apply(stF, confF, layoutsF); });
        }).start();
    }

    private void apply(String st, String conf, String layouts) {
        // parse rules from conf
        rules.clear();
        for (String line : conf.split("\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String pkg = line.substring(0, eq).trim();
            String val = line.substring(eq + 1).trim();
            rules.put(pkg, val.contains("hide"));
        }

        // parse status
        Matcher h = P_HIDE.matcher(st);
        hideOn = h.find() && "1".equals(h.group(1));
        Matcher n = P_NPADS.matcher(st);
        npads = n.find() ? Integer.parseInt(n.group(1)) : 0;
        boolean daemonUp = !st.trim().isEmpty() && !st.startsWith("ERR") && st.contains("\"pads\"");

        padList.clear();
        if (daemonUp) {
            try {
                JSONObject root = new JSONObject(st);
                JSONArray pads = root.optJSONArray("pads");
                if (pads != null) {
                    for (int i = 0; i < pads.length(); i++) {
                        JSONObject p = pads.getJSONObject(i);
                        PadInfo pi = new PadInfo();
                        pi.idx = p.optInt("idx", i);
                        pi.name = p.optString("name", "Gamepad #" + pi.idx);
                        pi.node = p.optString("node", "");
                        pi.hidden = p.optInt("hidden", 0) == 1;
                        pi.layout = p.optString("layout", "DEFAULT");
                        pi.custom = p.optString("custom", "");
                        pi.routes = p.optString("routes", "");
                        padList.add(pi);
                    }
                }
            } catch (Exception ignored) {}
        }

        StringBuilder sb = new StringBuilder();
        if (!daemonUp) {
            sb.append("daemon: OFFLINE (or su blocked)\nRaw su output below:");
            log.setText(lastRaw);
        } else {
            sb.append("daemon: running, ").append(npads).append(" pad(s)\n");
            sb.append("hide mode: ").append(hideOn ? "ON (physical pads grabbed)" : "OFF (all pads visible)");
            for (PadInfo pad : padList) {
                sb.append('\n').append(" \u2022 ").append(pad.getDisplayName())
                  .append(" [").append(pad.layout).append("]")
                  .append(pad.hidden ? "  [grabbed]" : "");
            }
        }
        status.setText(sb.toString());
        hideBtn.setText(hideOn ? "unhide physical pads" : "hide physical pads (Fusion only)");
        hideBtn.setEnabled(daemonUp);

        rebuildGamepadList();
        rebuildAppList();
    }

    private void rebuildGamepadList() {
        gamepadContainer.removeAllViews();
        float d = getResources().getDisplayMetrics().density;
        int p8 = (int) (8 * d);
        int p12 = (int) (12 * d);

        if (padList.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("  No gamepads detected.\n  Connect a controller via Bluetooth or USB to configure remapping.");
            empty.setPadding(p8, p8, p8, p8);
            empty.setTextColor(0xFF888888);
            gamepadContainer.addView(empty);
            return;
        }

        for (final PadInfo pad : padList) {
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(p12, p12, p12, p12);

            GradientDrawable bg = new GradientDrawable();
            bg.setColor(0xFF232323);
            bg.setCornerRadius(10 * d);
            bg.setStroke((int) (1.5f * d), 0xFF444444);
            card.setBackground(bg);

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, 0, (int) (10 * d));
            card.setLayoutParams(lp);

            TextView nameTv = new TextView(this);
            nameTv.setText("🎮  " + pad.getDisplayName());
            nameTv.setTextSize(16);
            nameTv.setTypeface(null, Typeface.BOLD);
            nameTv.setTextColor(0xFFFFFFFF);
            card.addView(nameTv);

            TextView layoutTv = new TextView(this);
            layoutTv.setText("Layout: " + pad.getLayoutDescription());
            layoutTv.setTextSize(14);
            layoutTv.setTypeface(null, Typeface.BOLD);
            if ("NINTENDO".equalsIgnoreCase(pad.layout)) {
                layoutTv.setTextColor(0xFF81C784); // Green
            } else if ("CUSTOM".equalsIgnoreCase(pad.layout)) {
                layoutTv.setTextColor(0xFFFFB74D); // Orange
            } else if ("SWAP_AB".equalsIgnoreCase(pad.layout) || "SWAP_XY".equalsIgnoreCase(pad.layout)) {
                layoutTv.setTextColor(0xFF4DD0E1); // Cyan
            } else {
                layoutTv.setTextColor(0xFF64B5F6); // Blue
            }
            layoutTv.setPadding(0, (int) (4 * d), 0, (int) (2 * d));
            card.addView(layoutTv);

            TextView infoTv = new TextView(this);
            String info = "Pad #" + pad.idx + " • /dev/input/" + pad.node +
                          (pad.hidden ? " • Grabbed (Fusion active)" : " • Visible");
            infoTv.setText(info);
            infoTv.setTextSize(12);
            infoTv.setTextColor(0xFFAAAAAA);
            card.addView(infoTv);

            TextView actionTv = new TextView(this);
            actionTv.setText("Tap to open remapping menu ❯");
            actionTv.setTextSize(12);
            actionTv.setTextColor(0xFF80D8FF);
            actionTv.setPadding(0, (int) (6 * d), 0, 0);
            card.addView(actionTv);

            card.setClickable(true);
            card.setFocusable(true);
            card.setOnClickListener(v -> showRemapMenu(pad));

            gamepadContainer.addView(card);
        }
    }

    private void showRemapMenu(final PadInfo pad) {
        final String[] options = new String[] {
            "Xbox Layout (Standard: A-bottom, B-right, X-left, Y-top)\nDefault Xbox controller order",
            "Nintendo Layout (A↔B and X↔Y swapped: B-bottom, A-right, Y-left, X-top)\nRemaps Xbox controls to Nintendo and vice-versa",
            "Swap A ↔ B Only (B-bottom, A-right)",
            "Swap X ↔ Y Only (Y-left, X-top)",
            "Custom Button Mapping… (customize individual buttons)"
        };

        final String[] modes = new String[] {
            "XBOX",
            "NINTENDO",
            "SWAP_AB",
            "SWAP_XY",
            "CUSTOM"
        };

        int currentIdx = 0;
        if ("NINTENDO".equalsIgnoreCase(pad.layout)) currentIdx = 1;
        else if ("SWAP_AB".equalsIgnoreCase(pad.layout)) currentIdx = 2;
        else if ("SWAP_XY".equalsIgnoreCase(pad.layout)) currentIdx = 3;
        else if ("CUSTOM".equalsIgnoreCase(pad.layout)) currentIdx = 4;
        else currentIdx = 0;

        new AlertDialog.Builder(this)
            .setTitle("Remap: " + pad.getDisplayName())
            .setSingleChoiceItems(options, currentIdx, (dialog, which) -> {
                dialog.dismiss();
                if (which == 4) {
                    showCustomRemapDialog(pad);
                } else {
                    applyLayout(pad, modes[which]);
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void applyLayout(final PadInfo pad, final String mode) {
        status.setText("Applying " + mode + " layout to " + pad.getDisplayName() + "…");
        new Thread(() -> {
            String out = Su.run("/data/adb/fusionctl layout " + pad.idx + " " + mode);
            saveLayoutToConf(pad.name, mode);
            runOnUiThread(() -> {
                log(out);
                Toast.makeText(MainActivity.this,
                    pad.getDisplayName() + " remapped to " + mode + " layout",
                    Toast.LENGTH_SHORT).show();
                refresh();
            });
        }).start();
    }

    private void showCustomRemapDialog(final PadInfo pad) {
        float d = getResources().getDisplayMetrics().density;
        int p8 = (int) (8 * d);
        int p12 = (int) (12 * d);

        ScrollView scroll = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(p12, p12, p12, p12);
        scroll.addView(content);

        TextView intro = new TextView(this);
        intro.setText("Assign an output action to each physical button on " + pad.getDisplayName() + ":\n");
        intro.setTextColor(0xFFCCCCCC);
        content.addView(intro);

        // Parse current custom map if any
        final Map<Integer, Integer> map = new LinkedHashMap<>();
        if (pad.custom != null && !pad.custom.isEmpty()) {
            for (String part : pad.custom.split(",")) {
                String[] kv = part.split("->");
                if (kv.length == 2) {
                    try {
                        map.put(Integer.parseInt(kv[0].trim()), Integer.parseInt(kv[1].trim()));
                    } catch (Exception ignored) {}
                }
            }
        }

        final List<Spinner> spinners = new ArrayList<>();
        for (int i = 0; i < BTN_CODES.length; i++) {
            final int code = BTN_CODES[i];
            TextView lbl = new TextView(this);
            lbl.setText(BTN_LABELS[i] + ":");
            lbl.setTextColor(0xFFFFFFFF);
            lbl.setTypeface(null, Typeface.BOLD);
            lbl.setPadding(0, p8, 0, (int) (2 * d));
            content.addView(lbl);

            Spinner spinner = new Spinner(this);
            ArrayAdapter<String> adapter = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_dropdown_item, TARGET_NAMES);
            spinner.setAdapter(adapter);

            int targetCode = map.containsKey(code) ? map.get(code) : 0;
            int sel = 0;
            for (int k = 0; k < TARGET_CODES.length; k++) {
                if (TARGET_CODES[k] == targetCode) {
                    sel = k;
                    break;
                }
            }
            spinner.setSelection(sel);
            content.addView(spinner);
            spinners.add(spinner);
        }

        new AlertDialog.Builder(this)
            .setTitle("Custom Remap: " + pad.getDisplayName())
            .setView(scroll)
            .setPositiveButton("Save Custom Mapping", (dialog, which) -> {
                StringBuilder sb = new StringBuilder();
                boolean first = true;
                for (int i = 0; i < BTN_CODES.length; i++) {
                    int from = BTN_CODES[i];
                    int sel = spinners.get(i).getSelectedItemPosition();
                    int to = TARGET_CODES[sel];
                    if (to > 0 && to != from) {
                        if (!first) sb.append(',');
                        sb.append(from).append("->").append(to);
                        first = false;
                    }
                }
                final String mapStr = sb.toString();
                if (mapStr.isEmpty()) {
                    applyLayout(pad, "DEFAULT");
                } else {
                    status.setText("Applying custom mapping to " + pad.getDisplayName() + "…");
                    new Thread(() -> {
                        String out = Su.run("/data/adb/fusionctl remap " + pad.idx + " " + mapStr);
                        saveLayoutToConf(pad.name, "CUSTOM:" + mapStr);
                        runOnUiThread(() -> {
                            log(out);
                            Toast.makeText(MainActivity.this,
                                "Custom mapping saved for " + pad.getDisplayName(),
                                Toast.LENGTH_SHORT).show();
                            refresh();
                        });
                    }).start();
                }
            })
            .setNeutralButton("Reset to Default", (dialog, which) -> applyLayout(pad, "DEFAULT"))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void saveLayoutToConf(String padName, String layoutOrMap) {
        try {
            String existing = Su.run("cat /data/adb/fusion-layouts.conf 2>/dev/null");
            Map<String, String> map = new LinkedHashMap<>();
            if (existing != null && !existing.startsWith("ERR")) {
                for (String line : existing.split("\n")) {
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int eq = line.indexOf('=');
                    if (eq > 0) {
                        map.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                    }
                }
            }
            map.put(padName, layoutOrMap);
            File f = new File(getCacheDir(), "fusion-layouts.conf");
            PrintWriter w = new PrintWriter(new FileWriter(f));
            w.println("# Fusion Controller - persistent pad layout configurations");
            w.println("# Format: <pad_name>=<layout>");
            for (Map.Entry<String, String> e : map.entrySet()) {
                w.println(e.getKey() + "=" + e.getValue());
            }
            w.close();
            Su.run("cp '" + f.getAbsolutePath() + "' /data/adb/fusion-layouts.conf && chmod 0644 /data/adb/fusion-layouts.conf");
        } catch (Exception ignored) {}
    }

    private void rebuildAppList() {
        appList.removeAllViews();
        Map<String, String> apps = new TreeMap<>();
        Intent main = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> ris = getPackageManager().queryIntentActivities(main, 0);
        for (ResolveInfo ri : ris) {
            String pkg = ri.activityInfo.packageName;
            if (pkg.equals(getPackageName())) continue;
            String label = String.valueOf(ri.loadLabel(getPackageManager()));
            String old = apps.get(pkg);
            if (old == null || label.length() < old.length()) apps.put(pkg, label);
        }
        // keep rules for apps that are no longer installed
        for (String pkg : rules.keySet()) if (!apps.containsKey(pkg)) apps.put(pkg, "(not installed)");
        if (apps.isEmpty()) {
            TextView tv = new TextView(this);
            tv.setText("  no apps found");
            appList.addView(tv);
        }
        for (Map.Entry<String, String> e : apps.entrySet()) {
            final String pkg = e.getKey();
            CheckBox cb = new CheckBox(this);
            cb.setText(e.getValue() + "   [" + pkg + "]");
            cb.setChecked(Boolean.TRUE.equals(rules.get(pkg)));
            cb.setOnClickListener(v -> {
                if (((CheckBox) v).isChecked()) rules.put(pkg, true);
                else rules.remove(pkg);
            });
            appList.addView(cb);
        }
    }

    private void toggleHide() {
        final String cmd = "/data/adb/fusionctl hide " + (hideOn ? "off" : "on");
        new Thread(() -> {
            String out = Su.run(cmd);
            runOnUiThread(() -> { log(out); refresh(); });
        }).start();
    }

    private void restartDaemon() {
        status.setText("restarting daemon & rescanning pads…");
        new Thread(() -> {
            String out = Su.run("/data/adb/fusionctl restart");
            try { Thread.sleep(700); } catch (InterruptedException ignored) {}
            runOnUiThread(() -> { log(out); refresh(); });
        }).start();
    }

    private void saveRules() {
        new Thread(() -> {
            try {
                File f = new File(getCacheDir(), "fusion-apps.conf");
                PrintWriter w = new PrintWriter(new FileWriter(f));
                w.println("# Fusion Controller - per-app rules (managed by Fusion Control app)");
                w.println("# default: hide mode ON (pads grabbed). <package>=nohide -> pads visible while focused");
                for (Map.Entry<String, Boolean> e : rules.entrySet()) {
                    if (e.getValue()) w.println(e.getKey() + "=nohide");
                }
                w.close();
                String out = Su.run("cp '" + f.getAbsolutePath() + "' /data/adb/fusion-apps.conf");
                int n = 0;
                for (Boolean v : rules.values()) if (v) n++;
                final String msg = "saved " + n + " rule(s)\n" + out;
                runOnUiThread(() -> log(msg));
            } catch (Exception e) {
                final String msg = "save failed: " + e;
                runOnUiThread(() -> log(msg));
            }
        }).start();
    }

    private void log(String s) { log.setText(s); }
}
