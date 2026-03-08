"""Manages the rps-server subprocess lifecycle."""

import os
import signal
import socket
import subprocess
import sys
import time
from typing import TYPE_CHECKING
from pathlib import Path

import grpc

if TYPE_CHECKING:
    from rps_client.windows_job import WindowsJobObject


class ServerManager:
    """Spawns and kills the rps-server binary as a subprocess."""

    def __init__(
        self,
        server_bin: str = "rps-server",
        port: int = 50051,
        db: str = "rps-plugins.db",
    ):
        self.server_bin = server_bin
        self.port = port
        self.db = db
        self._process: subprocess.Popen | None = None
        self._windows_job: WindowsJobObject | None = None
        self._old_sigint_handler = None
        self._old_sigterm_handler = None
        self._debug = os.getenv("RPS_DEBUG_PROCESS_LIFECYCLE") == "1"

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
        server_path = Path(self.server_bin)
        if not server_path.exists() or not server_path.is_file():
            raise FileNotFoundError(f"Cannot find rps-server binary at: {self.server_bin}")

        scanner_name = "rps-pluginscanner.exe" if sys.platform == "win32" else "rps-pluginscanner"
        scanner_path = server_path.with_name(scanner_name)
        if not scanner_path.exists() or not scanner_path.is_file():
            raise FileNotFoundError(f"Cannot find {scanner_name} alongside rps-server at: {scanner_path}")

        self._old_sigint_handler = signal.getsignal(signal.SIGINT)
        self._old_sigterm_handler = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGINT, self._handle_signal)
        signal.signal(signal.SIGTERM, self._handle_signal)

        cmd = [
            self.server_bin,
            "--port", str(self.port),
            "--db", self.db,
        ]
        self._process = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=(sys.platform != "win32"),
        )
        if self._debug:
            print(f"[rps] spawned server pid {self._process.pid}")

        if sys.platform == "win32":
            # Windows process ownership model:
            # parent/child does not imply lifetime ownership. Attach rps-server
            # to a Job Object so parent exit tears down the process tree.
            from rps_client.windows_job import WindowsJobObject

            try:
                self._windows_job = WindowsJobObject.create_and_assign(self._process.pid)
                if self._debug:
                    print(f"[rps] attached Windows Job Object to pid {self._process.pid}")
            except Exception:
                self._process.terminate()
                self._process.wait(timeout=2.0)
                self._process = None
                raise

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._process.poll() is not None:
                self._close_windows_job()
                raise RuntimeError(
                    f"rps-server exited immediately with code {self._process.returncode}"
                )
            try:
                with socket.create_connection(("localhost", self.port), timeout=0.5):
                    return
            except (ConnectionRefusedError, OSError, socket.timeout):
                time.sleep(0.2)

        self.stop(in_hurry=True)
        raise TimeoutError(
            f"rps-server did not start within {timeout}s on port {self.port}"
        )

    def stop(self, in_hurry: bool = False) -> None:
        """Stop the server subprocess gracefully, then forcefully if needed."""
        if self._old_sigint_handler:
            signal.signal(signal.SIGINT, self._old_sigint_handler)
            self._old_sigint_handler = None
        if self._old_sigterm_handler:
            signal.signal(signal.SIGTERM, self._old_sigterm_handler)
            self._old_sigterm_handler = None

        if self._process is None:
            self._close_windows_job()
            return

        try:
            if in_hurry:
                if self._debug:
                    print("[rps] stop(in_hurry=True)")
                self._process.terminate()
                try:
                    self._process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait()
                return

            try:
                from rps_client.proto import rps_pb2, rps_pb2_grpc

                channel = grpc.insecure_channel(f"localhost:{self.port}")
                stub = rps_pb2_grpc.RpsServiceStub(channel)
                stub.Shutdown(rps_pb2.ShutdownRequest(), timeout=2)
                channel.close()
            except Exception:
                pass

            try:
                self._process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self._process.terminate()
                try:
                    self._process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait()
        finally:
            if self._debug:
                print("[rps] server process stopped")
            self._process = None
            self._close_windows_job()

    def _close_windows_job(self) -> None:
        if self._windows_job is not None:
            try:
                self._windows_job.close()
                if self._debug:
                    print("[rps] closed Windows Job Object")
            finally:
                self._windows_job = None

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
