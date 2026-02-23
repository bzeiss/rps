"""gRPC client wrapper for the RPS service."""

from typing import Iterator

import grpc

from rps_client.proto import rps_pb2, rps_pb2_grpc


class RpsClient:
    """Thin wrapper around the generated gRPC stub."""

    def __init__(self, address: str = "localhost:50051"):
        self.address = address
        self._channel: grpc.Channel | None = None
        self._stub: rps_pb2_grpc.RpsServiceStub | None = None

    def connect(self) -> None:
        self._channel = grpc.insecure_channel(self.address)
        self._stub = rps_pb2_grpc.RpsServiceStub(self._channel)

    def close(self) -> None:
        if self._channel:
            self._channel.close()
            self._channel = None
            self._stub = None

    def start_scan(
        self,
        scan_dirs: list[str] | None = None,
        single_plugin: str = "",
        mode: str = "incremental",
        formats: str = "all",
        filter_str: str = "",
        limit: int = 0,
        jobs: int = 6,
        retries: int = 3,
        timeout_ms: int = 120000,
        verbose: bool = False,
    ) -> Iterator[rps_pb2.ScanEvent]:
        """Start a scan and return an iterator of ScanEvent messages."""
        request = rps_pb2.StartScanRequest(
            scan_dirs=scan_dirs or [],
            single_plugin=single_plugin,
            mode=mode,
            formats=formats,
            filter=filter_str,
            limit=limit,
            jobs=jobs,
            retries=retries,
            timeout_ms=timeout_ms,
            verbose=verbose,
        )
        return self._stub.StartScan(request)

    def get_status(self) -> rps_pb2.GetStatusResponse:
        return self._stub.GetStatus(rps_pb2.GetStatusRequest())

    def stop_scan(self) -> rps_pb2.StopScanResponse:
        return self._stub.StopScan(rps_pb2.StopScanRequest())

    def shutdown(self) -> None:
        try:
            self._stub.Shutdown(rps_pb2.ShutdownRequest(), timeout=3)
        except grpc.RpcError:
            pass  # Server may close connection before responding

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()
