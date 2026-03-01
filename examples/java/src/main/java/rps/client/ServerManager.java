package rps.client;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

public class ServerManager implements AutoCloseable {
    private static final boolean PROCESS_DEBUG =
            "1".equals(System.getenv("RPS_DEBUG_PROCESS_LIFECYCLE"));

    private final String binPath;
    private final int port;
    private final String dbPath;
    private final String logLevel;
    private Process process;
    private WindowsJobObject windowsJob;

    public ServerManager(String binPath, int port, String dbPath, String logLevel) {
        this.binPath = binPath;
        this.port = port;
        this.dbPath = dbPath;
        this.logLevel = logLevel;
    }

    public void start() throws IOException, InterruptedException {
        File serverFile = new File(binPath);
        if (!serverFile.exists() || !serverFile.isFile()) {
            throw new FileNotFoundException("Cannot find rps-server binary at: " + binPath);
        }

        String os = System.getProperty("os.name").toLowerCase();
        String scannerName = os.contains("win") ? "rps-pluginscanner.exe" : "rps-pluginscanner";
        File scannerFile = new File(serverFile.getParentFile(), scannerName);
        if (!scannerFile.exists() || !scannerFile.isFile()) {
            throw new FileNotFoundException("Cannot find " + scannerName + " alongside rps-server at: " + scannerFile.getAbsolutePath());
        }

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
        if (isWindows()) {
            // Windows process ownership model:
            // parent/child does not imply lifetime ownership. We explicitly attach
            // rps-server to a Job Object so parent exit tears down the process tree.
            this.windowsJob = WindowsJobObject.createAndAssign(process.pid());
            if (PROCESS_DEBUG) {
                System.out.println("[rps] attached Windows Job Object to pid " + process.pid());
            }
        }

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
        try {
            if (process != null) {
                if (PROCESS_DEBUG) {
                    System.out.println("[rps] stopping server pid " + process.pid());
                }
                process.destroy();
                try {
                    process.waitFor(2, TimeUnit.SECONDS);
                } catch (InterruptedException ignored) {
                }

                // If graceful stop didn't finish quickly, force-kill child tree.
                if (process.isAlive()) {
                    process.descendants().forEach(ProcessHandle::destroyForcibly);
                    process.destroyForcibly();
                    try {
                        process.waitFor(5, TimeUnit.SECONDS);
                    } catch (InterruptedException ignored) {
                    }
                }
            }
        } finally {
            if (windowsJob != null) {
                windowsJob.close();
                windowsJob = null;
                if (PROCESS_DEBUG) {
                    System.out.println("[rps] closed Windows Job Object");
                }
            }
            process = null;
        }
    }

    private static boolean isWindows() {
        return System.getProperty("os.name").toLowerCase().contains("win");
    }
}
