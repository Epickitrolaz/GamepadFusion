package dev.fusion.control;

import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Quick Settings tile: tap to restart the fusiond daemon and rescan all connected
 * gamepads. Resets dynamic axis/trigger disambiguation so axes that were mistakenly
 * classified as triggers get properly recognized as sticks.
 */
public class FusionRestartTile extends TileService {

    private static final Pattern P_NPADS = Pattern.compile("\"npads\":(\\d+)");
    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    private boolean listening = false;
    private boolean restarting = false;

    private final Runnable resetSubtitle = this::refreshState;

    @Override public void onStartListening() {
        listening = true;
        refreshState();
    }

    @Override public void onStopListening() {
        listening = false;
        main.removeCallbacks(resetSubtitle);
    }

    @Override public void onTileAdded() {
        refreshState();
    }

    @Override public void onClick() {
        restart();
    }

    private void refreshState() {
        final Tile t = getQsTile();
        if (t == null || restarting) return;

        EXEC.execute(() -> {
            final String out = Su.run("/data/adb/fusionctl status");
            main.post(() -> {
                if (!listening || restarting) return;
                applyStatus(out);
            });
        });
    }

    private void applyStatus(String out) {
        final Tile t = getQsTile();
        if (t == null) return;

        if (out.contains("\"pads\"")) {
            Matcher m = P_NPADS.matcher(out);
            int npads = m.find() ? Integer.parseInt(m.group(1)) : 0;
            t.setState(Tile.STATE_INACTIVE);
            subtitle(t, npads + " pad" + (npads == 1 ? "" : "s") + " \u2022 tap to restart");
        } else {
            t.setState(Tile.STATE_UNAVAILABLE);
            subtitle(t, "daemon offline \u2022 tap to start");
        }
        t.updateTile();
    }

    private void restart() {
        final Tile t = getQsTile();
        if (t == null || restarting) return;

        restarting = true;
        main.removeCallbacks(resetSubtitle);

        t.setState(Tile.STATE_ACTIVE);
        subtitle(t, "restarting\u2026");
        t.updateTile();

        EXEC.execute(() -> {
            String res = Su.run("/data/adb/fusionctl restart");
            if (res.contains("usage: fusionctl")) {
                // Fallback for older script version
                Su.run("echo RESCAN | timeout 3 nc -U /data/adb/fusion.sock || pkill -HUP fusiond");
            }

            // Brief pause for daemon re-enumeration & socket readiness
            try { Thread.sleep(700); } catch (InterruptedException ignored) {}

            final String st = Su.run("/data/adb/fusionctl status");
            main.post(() -> {
                restarting = false;
                onRestartComplete(st);
            });
        });
    }

    private void onRestartComplete(String st) {
        final Tile t = getQsTile();
        if (t == null) return;

        if (st.contains("\"pads\"")) {
            Matcher m = P_NPADS.matcher(st);
            int npads = m.find() ? Integer.parseInt(m.group(1)) : 0;
            t.setState(Tile.STATE_INACTIVE);
            subtitle(t, "rescanned (" + npads + " pad" + (npads == 1 ? "" : "s") + ")");
            t.updateTile();
            main.postDelayed(resetSubtitle, 3000);
        } else {
            t.setState(Tile.STATE_UNAVAILABLE);
            subtitle(t, "daemon offline");
            t.updateTile();
            main.postDelayed(resetSubtitle, 3000);
        }
    }

    private static void subtitle(Tile t, String s) {
        if (Build.VERSION.SDK_INT >= 29) t.setSubtitle(s);
    }
}
