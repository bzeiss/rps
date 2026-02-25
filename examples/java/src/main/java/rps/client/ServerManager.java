package rps.client;

import java.io.File;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class ServerManager implements AutoCloseable {
    private final String binPath;
    private final int port;
    private final String dbPath;
    private final String logLevel;
    private Process process;

    public ServerManager(String binPath, int port, String dbPath, String logLevel) {
        this.binPath = binPath;
        this.port = port;
        this.dbPath = dbPath;
        this.logLevel = logLevel;
    }

    public void start() throws IOException, InterruptedException {
        List<String> cmd = new ArrayList<>();
        cmd.add(binPath);
        cmd.add("--port");
        cmd.add(String.valueOf(port));
        cmd.add("--db");
        cmd.add(dbPath);
        cmd.add("--log-level");
        cmd.add(logLevel);

        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.redirectError(ProcessBuilder.Redirect.DISCARD);
        pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);

        System.out.println("Spawning server: " + String.join(" ", cmd));
        this.process = pb.start();

        // Wait for server to be ready
        long deadline = System.currentTimeMillis() + 10000;
        while (System.currentTimeMillis() < deadline) {
            if (!process.isAlive()) {
                throw new IOException("rps-server exited immediately with code " + process.exitValue() + 
                                     ". Check rps-server.log for details.");
            }
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress("127.0.0.1", port), 500);
                return; // Connected!
            } catch (IOException e) {
                Thread.sleep(200);
            }
        }
        throw new IOException("rps-server did not start within 10s on port " + port);
    }

    public static String findServerBinary() {
        // Try standard build locations relative to CWD
        String[] candidates = {
            "../../build/apps/rps-server/rps-server",
            "../../build/apps/rps-server/rps-server.exe",
            "build/apps/rps-server/rps-server",
            "../apps/rps-server/rps-server"
        };

        for (String c : candidates) {
            File f = new File(c);
            if (f.exists() && f.canExecute()) {
                return f.getAbsolutePath();
            }
        }

        // Try PATH
        String path = System.getenv("PATH");
        String sep = System.getProperty("path.separator");
        for (String p : path.split(sep)) {
            File f = new File(p, "rps-server");
            if (f.exists() && f.canExecute()) return f.getAbsolutePath();
            f = new File(p, "rps-server.exe");
            if (f.exists() && f.canExecute()) return f.getAbsolutePath();
        }

        return null;
    }

    @Override
    public void close() {
        if (process != null && process.isAlive()) {
            // Kill the entire process tree (server + any scanner workers)
            process.descendants().forEach(ProcessHandle::destroyForcibly);
            process.destroyForcibly();
            try {
                process.waitFor(5, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                // already force-killed
            }
        }
    }
}
