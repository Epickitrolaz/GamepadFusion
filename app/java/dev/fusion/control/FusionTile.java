package dev.fusion.control;

import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings tile: tap to toggle hide mode (grab physical pads / Fusion-only). */
public class FusionTile extends TileService {

    @Override public void onStartListening() { refresh(); }
    @Override public void onTileAdded()      { refresh(); }
    @Override public void onClick()          { toggle(); }

    private void refresh() {
        Tile t = getQsTile();
        if (t == null) return;
        String out = Su.run("/data/adb/fusionctl status");
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
        Tile t = getQsTile();
        boolean on = t != null && t.getState() == Tile.STATE_ACTIVE;
        Su.run("/data/adb/fusionctl hide " + (on ? "off" : "on"));
        refresh();
    }

    private static void subtitle(Tile t, String s) {
        if (Build.VERSION.SDK_INT >= 29) t.setSubtitle(s);
    }
}
