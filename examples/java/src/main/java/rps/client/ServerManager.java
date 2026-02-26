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
        // Only check CWD and the directory of the running class/JAR
        String[] names = {"rps-server", "rps-server.exe"};
        String[] locations = {
            System.getProperty("user.dir"),
            new File(ServerManager.class.getProtectionDomain().getCodeSource().getLocation().getPath()).getParent()
        };

        for (String loc : locations) {
            if (loc == null) continue;
            for (String name : names) {
                File f = new File(loc, name);
                if (f.exists() && f.canExecute() && !f.isDirectory()) {
                    return f.getAbsolutePath();
                }
            }
        }

        // Try PATH
        String pathEnv = System.getenv("PATH");
        if (pathEnv != null) {
            String sep = System.getProperty("path.separator");
            for (String p : pathEnv.split(sep)) {
                for (String name : names) {
                    File f = new File(p, name);
                    if (f.exists() && f.canExecute() && !f.isDirectory()) {
                        return f.getAbsolutePath();
                    }
                }
            }
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
