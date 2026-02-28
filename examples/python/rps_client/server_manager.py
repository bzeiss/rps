"""Manages the rps-server subprocess lifecycle."""

import subprocess
import time
import socket
import os
import sys
import signal
from pathlib import Path

import grpc


class ServerManager:
    """Spawns and kills the rps-server binary as a subprocess."""

    def __init__(
        self,
        server_bin: str = "rps-server",
        port: int = 50051,
        db: str = "rps-plugins.db",
        log_file: str = "rps-server.log",
        log_level: str = "info",
    ):
        self.server_bin = server_bin
        self.port = port
        self.db = db
        self.log_file = log_file
        self.log_level = log_level
        self._process: subprocess.Popen | None = None
        self._old_sigint_handler = None
        self._old_sigterm_handler = None

    def _handle_signal(self, signum, frame):
        """Signal handler to ensure cleanup on SIGINT/SIGTERM."""
        self.stop(in_hurry=True)
        if signum == signal.SIGINT and self._old_sigint_handler:
            self._old_sigint_handler(signum, frame)
        elif signum == signal.SIGTERM and self._old_sigterm_handler:
            self._old_sigterm_handler(signum, frame)
        else:
            sys.exit(1)

    def start(self, timeout: float = 10.0) -> None:
        """Start the server subprocess and wait for it to accept connections."""
        # Precondition checks for binaries
        server_path = Path(self.server_bin)
        if not server_path.exists() or not server_path.is_file():
            raise FileNotFoundError(f"Cannot find rps-server binary at: {self.server_bin}")

        scanner_name = "rps-pluginscanner.exe" if sys.platform == "win32" else "rps-pluginscanner"
        scanner_path = server_path.with_name(scanner_name)
        if not scanner_path.exists() or not scanner_path.is_file():
            raise FileNotFoundError(f"Cannot find {scanner_name} alongside rps-server at: {scanner_path}")

        # Register signal handlers for cleanup
        self._old_sigint_handler = signal.getsignal(signal.SIGINT)
        self._old_sigterm_handler = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGINT, self._handle_signal)
        signal.signal(signal.SIGTERM, self._handle_signal)

        cmd = [
            self.server_bin,
            "--port", str(self.port),
            "--db", self.db,
            "--log", self.log_file,
            "--log-level", self.log_level,
        ]
        env = self._build_env()
        # On Unix, put the process in its own group so Ctrl+C to parent doesn't 
        # kill the child immediately before we can call Shutdown() or cleanup.
        # Wait, actually if we WANT it killed, we should leave it.
        # But for reliability, we want the PARENT to control the death.
        self._process = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
            start_new_session=(sys.platform != "win32")
        )

        # Wait for the server to start accepting connections
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._process.poll() is not None:
                raise RuntimeError(
                    f"rps-server exited immediately with code {self._process.returncode}"
                )
            try:
                with socket.create_connection(("localhost", self.port), timeout=0.5):
                    return  # Server is ready
            except (ConnectionRefusedError, OSError, socket.timeout):
                time.sleep(0.2)

        raise TimeoutError(
            f"rps-server did not start within {timeout}s on port {self.port}"
        )

    def stop(self, in_hurry: bool = False) -> None:
        """Stop the server subprocess gracefully, then forcefully if needed."""
        # Restore signal handlers
        if self._old_sigint_handler:
            signal.signal(signal.SIGINT, self._old_sigint_handler)
            self._old_sigint_handler = None
        if self._old_sigterm_handler:
            signal.signal(signal.SIGTERM, self._old_sigterm_handler)
            self._old_sigterm_handler = None

        if self._process is None:
            return

        if in_hurry:
            # Kill immediately — server's signal handler will clean up children
            self._process.terminate()  # SIGTERM
            try:
                self._process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self._process.kill()  # SIGKILL
                self._process.wait()
            self._process = None
            return

        # Try graceful shutdown via gRPC first
        try:
            from rps_client.proto import rps_pb2, rps_pb2_grpc

            channel = grpc.insecure_channel(f"localhost:{self.port}")
            stub = rps_pb2_grpc.RpsServiceStub(channel)
            stub.Shutdown(rps_pb2.ShutdownRequest(), timeout=2)
            channel.close()
        except Exception:
            pass

        # Wait for graceful exit
        try:
            self._process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            # Force kill
            self._process.terminate()
            try:
                self._process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait()

        self._process = None

    @staticmethod
    def _build_env() -> dict[str, str]:
        """Build subprocess environment with MSYS2 DLL directories on PATH.

        On Windows, rps-server links shared gRPC/protobuf/spdlog DLLs from
        MSYS2. If the user runs from PowerShell or VS Code (rather than the
        MSYS2 shell), those DLLs won't be on PATH. We auto-detect and add
        the MSYS2 bin directory so the server can find them.
        """
        env = os.environ.copy()
        if sys.platform == "win32":
            msys2_dirs = [
                r"C:\msys64\clang64\bin",
                r"C:\msys64\mingw64\bin",
                r"C:\msys64\ucrt64\bin",
            ]
            path = env.get("PATH", "")
            additions = [d for d in msys2_dirs if os.path.isdir(d) and d not in path]
            if additions:
                env["PATH"] = ";".join(additions) + ";" + path
        return env

    @property
    def address(self) -> str:
        return f"localhost:{self.port}"

    @property
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
