package dev.fusion.control;

import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Quick Settings tile: tap to toggle hide mode (grab physical pads / Fusion-only).
 *
 * State is fetched when the shade opens (onStartListening) and after a toggle -
 * which is exactly when the tile is visible, so it is always fresh for the user.
 *
 * Do NOT add a timer that re-polls the daemon. Every status query costs one su
 * round-trip, and Magisk audit-logs each of those by spawning
 * `app_process com.android.commands.content.Content`. On ROMs whose boot
 * classpath lacks that class, every single su call aborts with
 * "ClassNotFoundException: com.android.commands.content.Content" and dumps a
 * SIGABRT tombstone into logcat. The old 1s poll therefore produced a crash
 * log every second (see logcat.log); the tile is refresh-on-open because of it.
 */
public class FusionTile extends TileService {

    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override public void onStartListening() { queryOnce(); }

    @Override public void onTileAdded() { queryOnce(); }

    @Override public void onClick() { toggle(); }

    /** One su round-trip, result applied on the main thread. */
    private void queryOnce() {
        if (getQsTile() == null) return;
        EXEC.execute(() -> {
            final String out = Su.run("/data/adb/fusionctl status");
            main.post(() -> apply(out));
        });
    }

    private void apply(String out) {
        Tile t = getQsTile();
        if (t == null) return;
        if (out.contains("\"hide_mode\":1")) {
            t.setState(Tile.STATE_ACTIVE);
            subtitle(t, "pads grabbed - Fusion only");
        } else if (out.contains("\"hide_mode\":0")) {
            t.setState(Tile.STATE_INACTIVE);
            subtitle(t, "physical pads visible");
        } else {
            t.setState(Tile.STATE_UNAVAILABLE);
            subtitle(t, "daemon offline");
        }
        t.updateTile();
    }

    private void toggle() {
        final Tile t = getQsTile();
        if (t == null) return;
        boolean on = t.getState() == Tile.STATE_ACTIVE;
        subtitle(t, "switching\u2026");
        t.updateTile();
        // single-thread executor, so the hide lands before the status query
        EXEC.execute(() -> Su.run("/data/adb/fusionctl hide " + (on ? "off" : "on")));
        queryOnce();
    }

    private static void subtitle(Tile t, String s) {
        if (Build.VERSION.SDK_INT >= 29) t.setSubtitle(s);
    }
}
