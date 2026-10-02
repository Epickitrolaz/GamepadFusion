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
 * su round-trips run on a background executor; tile updates are posted back to
 * the main thread so SystemUI is never blocked. While the tile is listening
 * (QS shade open) it re-polls the daemon every second, so profile switches made
 * by the per-app monitor show up immediately instead of whenever SystemUI
 * next decides to rebind the service.
 */
public class FusionTile extends TileService {

    private static final long POLL_MS = 1000;
    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    private boolean listening = false;
    private boolean querying = false;

    private final Runnable tick = this::poll;

    @Override public void onStartListening() {
        listening = true;
        poll();          // immediate first state, then scheduled refreshes
    }

    @Override public void onStopListening() {
        listening = false;
        main.removeCallbacks(tick);
    }

    @Override public void onTileAdded() { queryOnce(); }

    @Override public void onClick() { toggle(); }

    /** One su round-trip, result applied on the main thread. */
    private void queryOnce() {
        final Tile t = getQsTile();
        if (t == null) return;
        if (querying) {
            // a query is already queued/running - try again on the next tick
            // (always reschedule, or the polling loop would die here)
            if (listening) main.postDelayed(tick, POLL_MS);
            return;
        }
        querying = true;
        EXEC.execute(() -> {
            final String out = Su.run("/data/adb/fusionctl status");
            main.post(() -> { querying = false; apply(out); });
        });
    }

    private void poll() {
        if (listening) queryOnce();
    }

    private void apply(String out) {
        Tile t = getQsTile();
        if (t != null) {
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
        if (listening) main.postDelayed(tick, POLL_MS);
    }

    private void toggle() {
        final Tile t = getQsTile();
        if (t == null) return;
        boolean on = t.getState() == Tile.STATE_ACTIVE;
        subtitle(t, "switching\u2026");
        t.updateTile();
        EXEC.execute(() -> Su.run("/data/adb/fusionctl hide " + (on ? "off" : "on")));
        queryOnce();
    }

    private static void subtitle(Tile t, String s) {
        if (Build.VERSION.SDK_INT >= 29) t.setSubtitle(s);
    }
}