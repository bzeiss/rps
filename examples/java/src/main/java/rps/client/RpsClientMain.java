package rps.client;

import org.jline.terminal.Terminal;
import org.jline.terminal.TerminalBuilder;
import rps.v1.Rps.ScanEvent;
import rps.v1.Rps.GetStatusResponse;

import java.util.Collections;
import java.util.Iterator;
import java.util.Map;
import java.util.TreeMap;

public class RpsClientMain {
    private static Terminal terminal;
    private static int totalPlugins = 0;
    private static int completedPlugins = 0;
    private static int successCount = 0;
    private static int failCount = 0;
    private static int crashCount = 0;
    private static int timeoutCount = 0;
    private static int skippedCount = 0;
    
    private static class WorkerInfo {
        String filename = "";
        int percentage = 0;
        String stage = "";
        boolean active = false;
    }
    
    private static final Map<Integer, WorkerInfo> workers = new TreeMap<>();
    private static int lastRenderedLines = 0;

    public static void main(String[] args) {
        String host = "127.0.0.1";
        int port = 50051;
        String dbPath = "rps-plugins.db";

        try {
            // Attempt to build a system terminal, fallback to dumb if needed
            terminal = TerminalBuilder.builder()
                    .system(true)
                    .build();
        } catch (Exception e) {
            try {
                terminal = TerminalBuilder.builder().dumb(true).build();
            } catch (Exception e2) {
                System.err.println("Critical error: Could not even initialize dumb terminal.");
                return;
            }
        }

        terminal.writer().println("RPS Java Client starting...");

        String serverBin = ServerManager.findServerBinary();
        try (ServerManager server = (serverBin != null) ? new ServerManager(serverBin, port, dbPath, "info") : null) {
            if (server != null) {
                server.start();
            }

            try (RpsClient client = new RpsClient(host, port)) {
                GetStatusResponse status = client.getStatus();
                terminal.writer().println("Server status: " + status.getState() + ", Uptime: " + status.getUptimeMs() + "ms");

                Iterator<ScanEvent> events = client.startScan(
                        Collections.emptyList(), "", "incremental", "all", "", 
                        0, 6, 3, 120000, false
                );

                while (events.hasNext()) {
                    handleEvent(events.next());
                    render();
                }

                terminal.writer().println("\nScan complete.");
            }
        } catch (io.grpc.StatusRuntimeException e) {
            terminal.writer().println("gRPC Error: " + e.getStatus().getCode());
        } catch (Exception e) {
            terminal.writer().println("Error: " + e.getMessage());
        }
    }

    private static void handleEvent(ScanEvent event) {
        if (event.hasScanStarted()) {
            totalPlugins = event.getScanStarted().getTotalPlugins();
        } 
        else if (event.hasPluginStarted()) {
            var p = event.getPluginStarted();
            workers.putIfAbsent(p.getWorkerId(), new WorkerInfo());
            WorkerInfo wi = workers.get(p.getWorkerId());
            wi.filename = p.getPluginFilename();
            wi.percentage = 0;
            wi.stage = "Starting";
            wi.active = true;
        } 
        else if (event.hasPluginProgress()) {
            var pr = event.getPluginProgress();
            WorkerInfo wi = workers.get(pr.getWorkerId());
            if (wi != null) {
                wi.percentage = pr.getPercentage();
                wi.stage = pr.getStage();
            }
        }
        else if (event.hasPluginCompleted()) {
            var c = event.getPluginCompleted();
            completedPlugins++;
            switch (c.getOutcome()) {
                case OUTCOME_SUCCESS -> successCount++;
                case OUTCOME_FAIL -> failCount++;
                case OUTCOME_CRASH -> crashCount++;
                case OUTCOME_TIMEOUT -> timeoutCount++;
                case OUTCOME_SKIPPED -> skippedCount++;
                case OUTCOME_UNKNOWN -> {}
                case UNRECOGNIZED -> {}
            }
            WorkerInfo wi = workers.get(c.getWorkerId());
            if (wi != null) {
                wi.active = false;
                wi.percentage = 100;
                wi.stage = c.getOutcome().toString();
            }
        }
    }

    private static void render() {
        var out = terminal.writer();
        
        // Move up to overwrite
        if (lastRenderedLines > 0) {
            out.print("\033[" + lastRenderedLines + "A");
        }

        int lines = 0;
        
        // Header
        out.print("\033[2K"); // Clear line
        out.println("\033[1;34mRPS Java Scanner\033[0m");
        lines++;

        // Overall progress
        int pct = totalPlugins > 0 ? (completedPlugins * 100 / totalPlugins) : 0;
        int barWidth = 40;
        int filled = (pct * barWidth) / 100;
        String bar = "━".repeat(filled) + "╌".repeat(barWidth - filled);
        
        out.print("\033[2K");
        out.println(String.format(" %s %3d%% (%d/%d)  \033[32m✓%d\033[0m \033[31m✗%d\033[0m \033[31m💥%d\033[0m \033[33m⏱%d\033[0m \033[2m⊘%d\033[0m", 
                bar, pct, completedPlugins, totalPlugins, successCount, failCount, crashCount, timeoutCount, skippedCount));
        lines++;

        out.print("\033[2K\n");
        lines++;

        // Worker lines
        for (var entry : workers.entrySet()) {
            out.print("\033[2K");
            WorkerInfo wi = entry.getValue();
            if (wi.active) {
                int wBarWidth = 10;
                int wFilled = (wi.percentage * wBarWidth) / 100;
                String wBar = "━".repeat(wFilled) + "╌".repeat(wBarWidth - wFilled);
                
                out.println(String.format("  \033[1m#%d\033[0m [%s] %3d%%  %-25s \033[2m(%s)\033[0m", 
                        entry.getKey(), wBar, wi.percentage, 
                        truncate(wi.filename, 25), wi.stage));
            } else {
                out.println(String.format("  \033[2m#%d  idle\033[0m", entry.getKey()));
            }
            lines++;
        }

        out.flush();
        lastRenderedLines = lines;
    }

    private static String truncate(String s, int n) {
        if (s.length() <= n) return s;
        return s.substring(0, n-3) + "...";
    }
}
