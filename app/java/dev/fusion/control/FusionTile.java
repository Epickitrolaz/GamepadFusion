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
 * All su round-trips run on a background executor; tile updates are posted back
 * to the main thread so SystemUI is never blocked.
 */
public class FusionTile extends TileService {

    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override public void onStartListening() { refresh(); }
    @Override public void onTileAdded()      { refresh(); }
    @Override public void onClick()          { toggle(); }

    /** Runs a root command in the background, then invokes cb on the main thread. */
    private void su(final String cmd, final Callback cb) {
        final Tile t = getQsTile();
        if (t == null) return;
        EXEC.execute(() -> {
            final String out = Su.run(cmd);
            main.post(() -> cb.done(out));
        });
    }

    private interface Callback { void done(String out); }

    private void refresh() {
        final Tile t = getQsTile();
        if (t == null) return;
        su("/data/adb/fusionctl status", out -> {
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
        });
    }

    private void toggle() {
        final Tile t = getQsTile();
        if (t == null) return;
        boolean on = t.getState() == Tile.STATE_ACTIVE;
        subtitle(t, "switching\u2026");
        t.updateTile();
        su("/data/adb/fusionctl hide " + (on ? "off" : "on"), out -> refresh());
    }

    private static void subtitle(Tile t, String s) {
        if (Build.VERSION.SDK_INT >= 29) t.setSubtitle(s);
    }
}