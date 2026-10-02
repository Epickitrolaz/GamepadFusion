package dev.fusion.control;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.concurrent.TimeUnit;

/** Runs shell commands as root via Magisk su. All calls spawn background threads at the call site. */
public final class Su {
    private Su() {}

    public static String run(String cmd) {
        try {
            final Process p = new ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start();
            // watchdog: never block forever (e.g. un-granted root)
            new Thread(() -> {
                try { p.waitFor(12, TimeUnit.SECONDS); } catch (InterruptedException ignored) {}
                if (p.isAlive()) p.destroyForcibly();
            }).start();
            StringBuilder sb = new StringBuilder();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            p.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return "ERR " + e;
        }
    }
}
