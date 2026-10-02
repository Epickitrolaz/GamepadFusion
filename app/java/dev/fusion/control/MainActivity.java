package dev.fusion.control;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Fusion Control settings screen.
 *  - shows daemon/pad status
 *  - one-tap hide toggle
 *  - per-app "disable hide mode while focused" rules -> /data/adb/fusion-apps.conf
 * Everything goes through su + /data/adb/fusionctl (Magisk root, granted on first use).
 */
public class MainActivity extends Activity {

    private static final Pattern P_NPADS  = Pattern.compile("\"npads\":(\\d+)");
    private static final Pattern P_HIDE   = Pattern.compile("\"hide_mode\":(\\d)");
    private static final Pattern P_NAME   = Pattern.compile("\"name\":\"([^\"]+)\"");
    private static final Pattern P_HIDDEN = Pattern.compile("\"hidden\":(\\d)");

    private TextView status;
    private TextView log;
    private LinearLayout appList;
    private Button hideBtn;
    private final Map<String, Boolean> rules = new TreeMap<>();
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
        root.addView(title);

        status = new TextView(this);
        status.setText("loading…");
        status.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(status);

        hideBtn = new Button(this);
        hideBtn.setOnClickListener(v -> toggleHide());
        root.addView(hideBtn);

        Button refresh = new Button(this);
        refresh.setText("refresh");
        refresh.setOnClickListener(v -> refresh());
        root.addView(refresh);

        TextView appsTitle = new TextView(this);
        appsTitle.setText("\nDisable hide mode for selected apps. Selected apps keep pads visible while focused:");
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
                "echo '--- conf ---'; cat /data/adb/fusion-apps.conf 2>/dev/null");
            String st = "", conf = "";
            String[] parts = raw.split("--- [a-z]+ ---");
            if (parts.length > 2) st = parts[2].trim();
            if (parts.length > 3) conf = parts[3].trim();
            final String stF = st, confF = conf, rawF = raw;
            runOnUiThread(() -> { lastRaw = rawF; apply(stF, confF); });
        }).start();
    }

    private void apply(String st, String conf) {
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

        StringBuilder sb = new StringBuilder();
        if (!daemonUp) {
            sb.append("daemon: OFFLINE (or su blocked)\nRaw su output below:");
            log.setText(lastRaw);
        } else {
            sb.append("daemon: running, ").append(npads).append(" pad(s)\n");
            sb.append("hide mode: ").append(hideOn ? "ON (physical pads grabbed)" : "OFF (all pads visible)");
            Matcher nm = P_NAME.matcher(st);
            Matcher hd = P_HIDDEN.matcher(st);
            int i = 0;
            while (nm.find() && hd.find() && i < 8) {
                sb.append('\n').append(" \u2022 ").append(nm.group(1))
                  .append(hd.group(1).equals("1") ? "  [grabbed]" : "");
                i++;
            }
        }
        status.setText(sb.toString());
        hideBtn.setText(hideOn ? "unhide physical pads" : "hide physical pads (Fusion only)");
        hideBtn.setEnabled(daemonUp);

        rebuildAppList();
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
