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

    def list_plugins(self, format_filter: str = "") -> list:
        """List all successfully scanned plugins from the database."""
        response = self._stub.ListPlugins(
            rps_pb2.ListPluginsRequest(format_filter=format_filter)
        )
        return list(response.plugins)

    def open_plugin_gui(self, plugin_path: str, fmt: str) -> Iterator:
        """Open a plugin's native GUI. Returns an iterator of PluginEvent."""
        request = rps_pb2.OpenPluginGuiRequest(
            plugin_path=plugin_path,
            format=fmt,
        )
        return self._stub.OpenPluginGui(request)

    def open_plugin_gui_with_audio(
        self, plugin_path: str, fmt: str,
        sample_rate: int = 48000, block_size: int = 128, num_channels: int = 2,
        audio_device: str = "",
    ) -> Iterator:
        """Open a plugin's native GUI with audio processing enabled.

        Returns an iterator of PluginEvent, including an AudioReady event
        with shared memory details for sending audio.
        """
        request = rps_pb2.OpenPluginGuiRequest(
            plugin_path=plugin_path,
            format=fmt,
            enable_audio=True,
            sample_rate=sample_rate,
            block_size=block_size,
            num_channels=num_channels,
            audio_device=audio_device,
        )
        return self._stub.OpenPluginGui(request)

    def close_plugin_gui(self, plugin_path: str) -> rps_pb2.ClosePluginGuiResponse:
        """Close a currently open plugin GUI window (session stays alive)."""
        return self._stub.ClosePluginGui(
            rps_pb2.ClosePluginGuiRequest(plugin_path=plugin_path)
        )

    def show_plugin_gui(self, plugin_path: str) -> rps_pb2.ShowPluginGuiResponse:
        """Show the GUI window for a headless plugin session."""
        return self._stub.ShowPluginGui(
            rps_pb2.ShowPluginGuiRequest(plugin_path=plugin_path)
        )

    def close_plugin_session(self, plugin_path: str) -> rps_pb2.ClosePluginSessionResponse:
        """Close the entire plugin session (terminates the host process)."""
        return self._stub.ClosePluginSession(
            rps_pb2.ClosePluginSessionRequest(plugin_path=plugin_path)
        )

    def get_plugin_state(self, plugin_path: str) -> rps_pb2.GetPluginStateResponse:
        """Get the complete state of a running plugin as an opaque binary blob."""
        return self._stub.GetPluginState(
            rps_pb2.GetPluginStateRequest(plugin_path=plugin_path)
        )

    def set_plugin_state(self, plugin_path: str, state_data: bytes) -> rps_pb2.SetPluginStateResponse:
        """Restore plugin state from a previously saved binary blob."""
        return self._stub.SetPluginState(
            rps_pb2.SetPluginStateRequest(plugin_path=plugin_path, state_data=state_data)
        )

    def load_preset(self, plugin_path: str, preset_id: str) -> rps_pb2.LoadPresetResponse:
        """Load a preset by its id on a running plugin GUI."""
        return self._stub.LoadPreset(
            rps_pb2.LoadPresetRequest(plugin_path=plugin_path, preset_id=preset_id)
        )

    def stream_audio(self, plugin_path: str, input_blocks: Iterator):
        """Open a bidirectional audio stream through a hosted plugin.

        Args:
            plugin_path: The plugin path (must have an active audio GUI session).
            input_blocks: Iterator of raw audio block bytes (interleaved float32).

        Returns:
            Iterator of AudioOutputBlock messages from the server.
        """
        def _generate():
            seq = 0
            for block_bytes in input_blocks:
                yield rps_pb2.AudioInputBlock(
                    plugin_path=plugin_path if seq == 0 else "",
                    audio_data=block_bytes,
                    sequence=seq,
                )
                seq += 1

        return self._stub.StreamAudio(_generate())

    def list_audio_devices(self, backend: str = ""):
        """List available audio device backends and their devices."""
        resp = self._stub.ListAudioDevices(
            rps_pb2.ListAudioDevicesRequest(backend=backend)
        )
        return resp.devices

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()
