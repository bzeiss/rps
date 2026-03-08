"""Plugin host session management — headless-first with on-demand GUI."""

import sys
import threading
import time

from rich.console import Console
from rich.table import Table

from rps_client.client import RpsClient
from rps_client.rps_audio import AudioRing, read_wav, write_wav, rms_db


console = Console()


class ParameterStore:
    """Stores and tracks plugin parameters, updated from gRPC stream events."""

    def __init__(self):
        self.params: dict[str, dict] = {}  # keyed by param id
        self._order: list[str] = []        # insertion order for display

    def load_list(self, parameter_list) -> None:
        """Initialize from a ParameterList gRPC event."""
        self.params.clear()
        self._order.clear()
        for p in parameter_list.parameters:
            self.params[p.id] = {
                "id": p.id,
                "index": p.index,
                "name": p.name,
                "module": p.module,
                "min": p.min_value,
                "max": p.max_value,
                "default": p.default_value,
                "value": p.current_value,
                "display": p.display_text,
                "flags": p.flags,
            }
            self._order.append(p.id)

    def apply_updates(self, updates) -> list[dict]:
        """Apply delta ParameterUpdates. Returns list of changed param dicts."""
        changed = []
        for u in updates.updates:
            if u.param_id in self.params:
                p = self.params[u.param_id]
                p["value"] = u.value
                p["display"] = u.display_text
                changed.append(p)
        return changed

    def print_table(self, plugin_name: str = "") -> None:
        """Print a Rich table showing all parameters."""
        title = f"{plugin_name} — {len(self.params)} Parameters"
        table = Table(title=title, show_lines=False)
        table.add_column("#", style="dim", width=4, justify="right")
        table.add_column("Name", style="bold", min_width=16, max_width=30)
        table.add_column("Value", justify="right", width=10)
        table.add_column("Display", style="cyan", min_width=10, max_width=20)
        table.add_column("Range", style="dim", width=18)
        table.add_column("Module", style="dim", max_width=30, no_wrap=True)

        for pid in self._order:
            p = self.params[pid]
            # Skip hidden parameters
            if p["flags"] & 2:  # kParamFlagHidden
                continue
            table.add_row(
                str(p["index"]),
                p["name"],
                f"{p['value']:.4f}",
                p["display"] or "",
                f"{p['min']:.2f} — {p['max']:.2f}",
                p["module"] or "",
            )

        console.print(table)


class PresetStore:
    """Stores available presets from gRPC stream events."""

    def __init__(self):
        self.presets: list[dict] = []

    def load_list(self, preset_list) -> None:
        """Initialize from a PresetList gRPC event."""
        self.presets.clear()
        for p in preset_list.presets:
            self.presets.append({
                "id": p.id,
                "name": p.name,
                "category": p.category,
                "creator": p.creator,
                "index": p.index,
                "flags": p.flags,
            })

    def find_by_query(self, query: str) -> dict | None:
        """Find a preset by numeric index, exact id, or fuzzy name match."""
        # Numeric index match (the # column)
        try:
            idx = int(query)
            for p in self.presets:
                if p["index"] == idx:
                    return p
        except ValueError:
            pass
        # Exact id match
        for p in self.presets:
            if p["id"] == query:
                return p
        # Case-insensitive name substring match
        q = query.lower()
        for p in self.presets:
            if q in p["name"].lower():
                return p
        return None

    def print_table(self) -> None:
        """Print a Rich table showing all presets."""
        if not self.presets:
            console.print("[dim]No presets available.[/dim]")
            return
        table = Table(title=f"{len(self.presets)} Presets", show_lines=False)
        table.add_column("#", style="dim", width=5, justify="right")
        table.add_column("Name", style="bold", min_width=20)
        table.add_column("Category", style="cyan", max_width=30)
        table.add_column("Creator", style="green", max_width=20)
        for p in self.presets:
            flags = []
            if p["flags"] & 1:
                flags.append("factory")
            if p["flags"] & 2:
                flags.append("user")
            table.add_row(
                str(p["index"]),
                p["name"],
                p["category"] or "",
                p["creator"] or "",
            )
        console.print(table)


def _read_line_nonblocking(stop_event: threading.Event) -> str | None:
    """Read a line from stdin without blocking indefinitely.

    Polls every 200ms so we detect gui_closed quickly.
    Returns the line string, or None if stop_event fires or EOF.
    """
    if sys.platform == "win32":
        import msvcrt
        buf: list[str] = []
        while not stop_event.is_set():
            if msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch in ("\r", "\n"):
                    line = "".join(buf).strip()
                    print()  # Echo newline
                    return line
                if ch == "\x03":  # Ctrl-C
                    raise KeyboardInterrupt
                if ch == "\x08":  # Backspace
                    if buf:
                        buf.pop()
                        # Erase character on screen
                        sys.stdout.write("\b \b")
                        sys.stdout.flush()
                    continue
                buf.append(ch)
                sys.stdout.write(ch)
                sys.stdout.flush()
            else:
                time.sleep(0.2)
        return None
    else:
        import select
        while not stop_event.is_set():
            ready, _, _ = select.select([sys.stdin], [], [], 0.2)
            if ready:
                line = sys.stdin.readline()
                if not line:
                    return None  # EOF
                return line.strip()
        return None

def _list_and_select_plugin(
    client: RpsClient, format_filter: str = "", last_selected: tuple | None = None,
) -> tuple[str, str] | None:
    """Interactive fuzzy-search plugin selector using arrow keys.

    Type to filter, arrow keys to navigate, Enter to select, Esc to exit.
    Page-Up/Page-Down jump 20 entries. Cursor starts at last_selected if given.
    """
    from InquirerPy import inquirer
    from InquirerPy.base.control import Choice

    plugins = client.list_plugins(format_filter=format_filter)

    if not plugins:
        console.print("[yellow]No plugins found in the database.[/yellow]")
        console.print("Run a scan first: [bold]python -m rps_client scan[/bold]")
        return None

    # Build choices for the fuzzy selector
    choices = []
    initial_index = 0
    for i, p in enumerate(plugins):
        fmt = p.format.upper()
        name = p.name or "(unknown)"
        vendor = p.vendor or ""
        label = f"[{fmt}]  {name}"
        if vendor:
            label += f"  ({vendor})"
        label += f"  — {p.path}"
        value = (p.path, p.format)
        choices.append(Choice(value=value, name=label))
        if last_selected and value == last_selected:
            initial_index = i

    page_jump = 20

    try:
        prompt = inquirer.fuzzy(
            message="Select a plugin (type to filter, ↑↓/PgUp/PgDn to navigate):",
            choices=choices,
            max_height="70%",
            mandatory=False,
        )

        # Pre-select the last used plugin
        if initial_index > 0:
            prompt.content_control.selected_choice_index = initial_index

        # Register page-up/page-down to jump multiple entries
        from prompt_toolkit.keys import Keys

        @prompt._kb.add(Keys.PageUp)
        def _page_up(_event):
            ctrl = prompt.content_control
            if ctrl.choice_count > 0:
                ctrl.selected_choice_index = max(
                    0, ctrl.selected_choice_index - page_jump,
                )

        @prompt._kb.add(Keys.PageDown)
        def _page_down(_event):
            ctrl = prompt.content_control
            if ctrl.choice_count > 0:
                ctrl.selected_choice_index = min(
                    ctrl.choice_count - 1, ctrl.selected_choice_index + page_jump,
                )

        @prompt._kb.add(Keys.Home)
        def _home(_event):
            ctrl = prompt.content_control
            if ctrl.choice_count > 0:
                ctrl.selected_choice_index = 0

        @prompt._kb.add(Keys.End)
        def _end(_event):
            ctrl = prompt.content_control
            if ctrl.choice_count > 0:
                ctrl.selected_choice_index = ctrl.choice_count - 1

        result = prompt.execute()
    except KeyboardInterrupt:
        return None

    return result


def _handle_send_audio(ring: AudioRing, filepath: str, con: Console) -> None:
    """Process a WAV file through the plugin via the shared memory ring buffer."""
    import os
    import array

    if not os.path.isfile(filepath):
        con.print(f"[red]✗ File not found:[/red] {filepath}")
        return

    # Read input WAV
    try:
        samples, wav_sr, wav_ch = read_wav(filepath)
    except Exception as e:
        con.print(f"[red]✗ Cannot read WAV:[/red] {e}")
        return

    n_samples = len(samples)
    con.print(
        f"  [dim]Read {n_samples // wav_ch} frames, {wav_ch}ch, {wav_sr}Hz[/dim]"
    )

    if wav_sr != ring.sample_rate:
        con.print(
            f"  [yellow]⚠ Sample rate mismatch:[/yellow] "
            f"WAV={wav_sr}Hz, ring={ring.sample_rate}Hz — proceeding anyway"
        )

    # Pad samples to full blocks
    block_floats = ring.block_size * ring.num_channels
    total_blocks = (n_samples + block_floats - 1) // block_floats
    pad_len = total_blocks * block_floats - n_samples
    if pad_len > 0:
        samples.extend([0.0] * pad_len)

    # Pre-convert samples to raw bytes for fast slicing
    samples_bytes = samples.tobytes()
    block_byte_size = block_floats * 4  # float32 = 4 bytes

    # Process blocks
    output_parts: list[bytes] = []
    blocks_sent = 0
    blocks_received = 0

    con.print(f"  Processing {total_blocks} blocks...")

    while blocks_received < total_blocks:
        # Send a batch (fill the ring as much as possible)
        while blocks_sent < total_blocks:
            start = blocks_sent * block_byte_size
            end = start + block_byte_size
            if ring.write_input_block(samples_bytes[start:end]):
                blocks_sent += 1
            else:
                break  # Ring full, go drain output

        # Drain all available output
        drained = 0
        while blocks_received < blocks_sent:
            if ring.wait_for_output(timeout_ms=2000):
                out = ring.read_output_block()
                if out is not None:
                    output_parts.append(out)
                    blocks_received += 1
                    drained += 1
                else:
                    break
            else:
                con.print(
                    f"  [yellow]Timeout at block {blocks_received}/{total_blocks}[/yellow]"
                )
                break

        # If we drained nothing and can't send more, we're stuck
        if drained == 0 and blocks_sent >= total_blocks:
            con.print(
                f"  [red]Plugin stalled after {blocks_received}/{total_blocks} blocks[/red]"
            )
            break

    if not output_parts:
        con.print("[red]✗ No output received from plugin.[/red]")
        return

    # Decode output
    output = array.array("f")
    for part in output_parts:
        output.frombytes(part)

    # Trim padding
    output = output[:n_samples]

    # Stats
    con.print(f"  Input  RMS: {rms_db(samples)}")
    con.print(f"  Output RMS: {rms_db(output)}")

    # Write output
    name, ext = os.path.splitext(filepath)
    out_path = f"{name}_processed{ext}"
    write_wav(out_path, output, wav_sr, wav_ch)
    con.print(
        f"  [green]✓ Written {len(output) // wav_ch} frames → {out_path}[/green]"
    )


class AudioPlayback:
    """Manages background audio playback through the shared memory ring buffer."""

    def __init__(self):
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()

    @property
    def is_playing(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def stop(self, con: Console | None = None) -> None:
        """Stop any active playback."""
        if not self.is_playing:
            if con:
                con.print("[dim]No audio playing.[/dim]")
            return
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=3)
        if con:
            con.print("[green]■ Audio stopped.[/green]")

    def start_play_audio(
        self, ring: AudioRing, filepath: str, con: Console,
    ) -> None:
        """Play a WAV file once through the audio device (non-blocking)."""
        self._start(ring, filepath, con, looped=False)

    def start_play_audio_looped(
        self, ring: AudioRing, filepath: str, con: Console,
    ) -> None:
        """Play a WAV file in a loop through the audio device (non-blocking)."""
        self._start(ring, filepath, con, looped=True)

    def _start(
        self, ring: AudioRing, filepath: str, con: Console, *, looped: bool,
    ) -> None:
        import os

        # Stop any existing playback first
        if self.is_playing:
            con.print("[dim]Stopping previous playback...[/dim]")
            self.stop()

        if not os.path.isfile(filepath):
            con.print(f"[red]✗ File not found:[/red] {filepath}")
            return

        # Read input WAV
        try:
            samples, wav_sr, wav_ch = read_wav(filepath)
        except Exception as e:
            con.print(f"[red]✗ Cannot read WAV:[/red] {e}")
            return

        n_samples = len(samples)
        duration_s = n_samples / (wav_sr * wav_ch)

        if wav_sr != ring.sample_rate:
            con.print(
                f"  [yellow]⚠ Sample rate mismatch:[/yellow] "
                f"WAV={wav_sr}Hz, ring={ring.sample_rate}Hz — proceeding anyway"
            )

        # Pad samples to full blocks
        block_floats = ring.block_size * ring.num_channels
        total_blocks = (n_samples + block_floats - 1) // block_floats
        pad_len = total_blocks * block_floats - n_samples
        if pad_len > 0:
            samples.extend([0.0] * pad_len)

        samples_bytes = samples.tobytes()
        block_byte_size = block_floats * 4  # float32 = 4 bytes
        block_duration_s = ring.block_size / ring.sample_rate

        mode = "Looping" if looped else "Playing"
        con.print(
            f"  [bold green]▶ {mode}[/bold green] {n_samples // wav_ch} frames "
            f"({duration_s:.1f}s) — type [bold]stop-audio[/bold] to stop"
        )

        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._playback_worker,
            args=(ring, samples_bytes, block_byte_size, total_blocks,
                  block_duration_s, looped, con, duration_s),
            daemon=True,
        )
        self._thread.start()

    def _playback_worker(
        self, ring: AudioRing, samples_bytes: bytes,
        block_byte_size: int, total_blocks: int,
        block_duration_s: float, looped: bool,
        con: Console, duration_s: float,
    ) -> None:
        loop_count = 0
        start_time = time.monotonic()

        try:
            while not self._stop_event.is_set():
                loop_count += 1
                blocks_sent = 0
                loop_start = time.monotonic()

                while blocks_sent < total_blocks and not self._stop_event.is_set():
                    offset = blocks_sent * block_byte_size
                    if ring.write_input_block(samples_bytes[offset:offset + block_byte_size]):
                        blocks_sent += 1
                    else:
                        time.sleep(block_duration_s * 0.25)
                        continue

                    # Drain output ring
                    while ring.read_output_block() is not None:
                        pass

                    # Pace: don't get too far ahead of real-time
                    target_time = loop_start + (blocks_sent - 4) * block_duration_s
                    now = time.monotonic()
                    if now < target_time:
                        time.sleep(target_time - now)

                if not looped:
                    # Single play: wait for drain then exit
                    remaining_s = 4 * block_duration_s + 0.1
                    drain_end = time.monotonic() + remaining_s
                    while time.monotonic() < drain_end and not self._stop_event.is_set():
                        while ring.read_output_block() is not None:
                            pass
                        time.sleep(block_duration_s * 0.5)
                    break

        except Exception:
            pass

        elapsed = time.monotonic() - start_time
        if looped:
            con.print(
                f"  [green]■ Stopped[/green] after {loop_count} loop(s), "
                f"{elapsed:.1f}s total"
            )
        else:
            con.print(
                f"  [green]✓ Played[/green] {duration_s:.1f}s "
                f"through audio device"
            )


def _handle_send_audio_grpc(
    client: RpsClient, plugin_path: str, filepath: str, con: Console,
    block_size: int, num_channels: int, sample_rate: int,
) -> None:
    """Process a WAV file through the plugin via gRPC bidirectional streaming."""
    import os
    import array
    import time

    if not os.path.isfile(filepath):
        con.print(f"[red]✗ File not found:[/red] {filepath}")
        return

    # Read input WAV
    try:
        samples, wav_sr, wav_ch = read_wav(filepath)
    except Exception as e:
        con.print(f"[red]✗ Cannot read WAV:[/red] {e}")
        return

    n_samples = len(samples)
    n_frames = n_samples // wav_ch
    con.print(f"  Read {n_frames} frames, {wav_ch}ch, {wav_sr}Hz")

    if wav_sr != sample_rate:
        con.print(
            f"  [yellow]⚠ Sample rate mismatch: WAV={wav_sr}Hz, "
            f"session={sample_rate}Hz — proceeding anyway[/yellow]"
        )

    block_floats = block_size * num_channels
    total_blocks = (n_samples + block_floats - 1) // block_floats
    pad_len = total_blocks * block_floats - n_samples
    if pad_len > 0:
        samples.extend([0.0] * pad_len)

    samples_bytes = samples.tobytes()
    block_byte_size = block_floats * 4

    con.print(f"  Processing {total_blocks} blocks via gRPC...")

    # Generator that yields raw block bytes
    def _block_iter():
        for i in range(total_blocks):
            start = i * block_byte_size
            end = start + block_byte_size
            yield samples_bytes[start:end]

    # Use the bidirectional stream
    t0 = time.perf_counter()
    output_parts: list[bytes] = []
    try:
        for out_block in client.stream_audio(plugin_path, _block_iter()):
            output_parts.append(out_block.audio_data)
    except Exception as e:
        con.print(f"[red]✗ gRPC stream error:[/red] {e}")
        return

    elapsed = time.perf_counter() - t0

    if not output_parts:
        con.print("[red]✗ No output received from plugin.[/red]")
        return

    # Reassemble output
    output = array.array("f")
    for part in output_parts:
        chunk = array.array("f")
        chunk.frombytes(part)
        output.extend(chunk)

    # Trim padding
    if pad_len > 0:
        output = output[: n_samples]

    # Compute RMS
    in_rms = rms_db(samples)
    out_rms = rms_db(output)

    con.print(
        f"  [green]✓ Processed {len(output_parts)} blocks in {elapsed:.2f}s[/green] [dim](gRPC)[/dim]"
    )
    con.print(f"  Input  RMS: {in_rms}")
    con.print(f"  Output RMS: {out_rms}")

    # Write output WAV
    from pathlib import Path
    stem = Path(filepath).stem
    out_path = str(Path(filepath).parent / f"{stem}_processed.wav")
    write_wav(out_path, output, wav_sr, wav_ch)
    con.print(
        f"  [green]✓ Written {len(output) // wav_ch} frames → {out_path}[/green]"
    )

def _open_gui_session(
    client: RpsClient,
    plugin_path: str,
    fmt: str,
    enable_audio: bool,
    sample_rate: int,
    num_channels: int,
    block_size: int,
    audio_device: str,
) -> bool:
    """Open a headless plugin session and manage its lifecycle.

    The plugin loads headlessly first. User can open/close GUI on demand.
    Returns True if user wants to continue (back), False to quit entirely.
    """
    store = ParameterStore()
    preset_store = PresetStore()
    plugin_name = ""
    session_done = threading.Event()  # Fires when session truly ends
    gui_open = threading.Event()  # Tracks whether GUI window is currently open
    plugin_ready = threading.Event()  # Fires after initial load messages are printed
    audio_ring: AudioRing | None = None
    audio_playback = AudioPlayback()

    def _commands_hint() -> str:
        """Build a compact one-line summary of available commands."""
        cmds = ["open-gui", "close-gui", "params", "presets", "load-preset",
                "save-state", "load-state"]
        if enable_audio:
            cmds.extend(["send-audio", "send-audio-grpc"])
            if audio_device:
                cmds.extend(["play-audio", "play-audio-looped", "stop-audio"])
        cmds.extend(["back", "quit", "help"])
        return ", ".join(cmds)

    def _stream_consumer():
        """Background thread: consumes gRPC event stream."""
        nonlocal plugin_name, audio_ring
        try:
            if enable_audio:
                event_stream = client.open_plugin_gui_with_audio(
                    plugin_path, fmt,
                    sample_rate=sample_rate,
                    block_size=block_size,
                    num_channels=num_channels,
                    audio_device=audio_device,
                )
            else:
                event_stream = client.open_plugin_gui(plugin_path, fmt)
            for event in event_stream:
                if session_done.is_set():
                    break
                if event.HasField("gui_opened"):
                    g = event.gui_opened
                    plugin_name = g.plugin_name
                    if g.width == 0 and g.height == 0:
                        # Headless load — plugin loaded but no GUI yet
                        console.print(
                            f"[green]✓ Plugin loaded (headless):[/green] {g.plugin_name}"
                        )
                    else:
                        # GUI actually opened
                        gui_open.set()
                        console.print(
                            f"[green]✓ GUI Opened:[/green] {g.plugin_name} "
                            f"({g.width}×{g.height})"
                        )
                    plugin_ready.set()
                elif event.HasField("parameter_list"):
                    store.load_list(event.parameter_list)
                    count = len(store.params)
                    console.print(
                        f"  [green]✓[/green] {count} parameter(s) available "
                        f"(type [bold]params[/bold] to list)"
                    )
                elif event.HasField("parameter_updates"):
                    changed = store.apply_updates(event.parameter_updates)
                    for p in changed:
                        val_str = p['display'] or f"{p['value']:.4f}"
                        console.print(
                            f"  [cyan]⟳[/cyan] {p['name']}: [bold]{val_str}[/bold]"
                        )
                elif event.HasField("gui_closed"):
                    reason = event.gui_closed.reason
                    gui_open.clear()
                    if reason in ("session_ended", "crash"):
                        console.print(
                            f"\n[yellow]✗ Session ended:[/yellow] {reason}"
                        )
                        session_done.set()
                        break
                    else:
                        # GUI window closed but session stays alive
                        console.print(
                            f"\n[dim]GUI window closed ({reason}). "
                            f"Type 'open-gui' to reopen, 'quit' to end session.[/dim]"
                        )
                elif event.HasField("gui_error"):
                    e = event.gui_error
                    console.print(f"[red]✗ Error:[/red] {e.error}")
                    if e.details:
                        console.print(f"  [dim]{e.details}[/dim]")
                elif event.HasField("preset_list"):
                    preset_store.load_list(event.preset_list)
                    count = len(preset_store.presets)
                    console.print(
                        f"  [green]✓[/green] {count} preset(s) available "
                        f"(type [bold]presets[/bold] to list)"
                    )
                elif event.HasField("preset_loaded"):
                    pl = event.preset_loaded
                    console.print(
                        f"  [green]✓ Preset loaded:[/green] {pl.preset_name}"
                    )
                elif event.HasField("audio_ready"):
                    ar = event.audio_ready
                    try:
                        audio_ring = AudioRing(ar.shm_name)
                        console.print(
                            f"  [green]✓ Audio ready:[/green] "
                            f"{ar.sample_rate}Hz, bs={ar.block_size}, "
                            f"ch={ar.num_channels}"
                        )
                    except Exception as e:
                        console.print(f"  [red]✗ Audio ring error:[/red] {e}")
        except Exception as e:
            if not session_done.is_set():
                console.print(f"[red]Stream error: {e}[/red]")
                session_done.set()

    # Start stream consumer thread
    stream_thread = threading.Thread(target=_stream_consumer, daemon=True)
    stream_thread.start()

    # Wait for GUI to close (block the caller)
    try:
        # Wait for plugin to finish loading before showing prompt
        plugin_ready.wait(timeout=30)
        # Brief pause to let remaining stream events (params, presets, audio) print
        time.sleep(0.5)
        # Print available commands once before the prompt loop
        console.print(f"\n[dim]Commands: {_commands_hint()}[/dim]\n")
        while not session_done.is_set():
            sys.stdout.write("> ")
            sys.stdout.flush()
            line = _read_line_nonblocking(session_done)
            if line is None:
                break
            if not line:
                continue

            parts = line.split(maxsplit=1)
            cmd = parts[0].lower()

            if cmd in ("quit", "exit", "q"):
                console.print("[yellow]Exiting...[/yellow]")
                audio_playback.stop()
                try:
                    client.close_plugin_session(plugin_path)
                except Exception:
                    pass
                session_done.set()
                stream_thread.join(timeout=3)
                return False

            elif cmd in ("back", "b"):
                console.print("[yellow]Returning to plugin selection...[/yellow]")
                audio_playback.stop()
                try:
                    client.close_plugin_session(plugin_path)
                except Exception:
                    pass
                session_done.set()
                stream_thread.join(timeout=3)
                return True

            elif cmd == "open-gui":
                if gui_open.is_set():
                    console.print("[dim]GUI is already open.[/dim]")
                else:
                    console.print("[bold]Opening GUI...[/bold]")
                    try:
                        client.show_plugin_gui(plugin_path)
                    except Exception as e:
                        console.print(f"[red]✗ show-gui error:[/red] {e}")

            elif cmd in ("close-gui", "close"):
                if not gui_open.is_set():
                    console.print("[dim]GUI is not open.[/dim]")
                else:
                    console.print("[yellow]Closing GUI window...[/yellow]")
                    try:
                        client.close_plugin_gui(plugin_path)
                    except Exception:
                        pass

            elif cmd == "save-state" and len(parts) == 2:
                filepath = parts[1]
                try:
                    resp = client.get_plugin_state(plugin_path)
                    if resp.success:
                        with open(filepath, "wb") as f:
                            f.write(resp.state_data)
                        console.print(
                            f"[green]✓ State saved:[/green] "
                            f"{len(resp.state_data)} bytes → {filepath}"
                        )
                    else:
                        console.print(f"[red]✗ Save failed:[/red] {resp.error}")
                except Exception as e:
                    console.print(f"[red]✗ Save error:[/red] {e}")

            elif cmd == "load-state" and len(parts) == 2:
                filepath = parts[1]
                try:
                    with open(filepath, "rb") as f:
                        state_data = f.read()
                    console.print(
                        f"[dim]Loading {len(state_data)} bytes from {filepath}...[/dim]"
                    )
                    resp = client.set_plugin_state(plugin_path, state_data)
                    if resp.success:
                        console.print("[green]✓ State restored[/green]")
                    else:
                        console.print(f"[red]✗ Load failed:[/red] {resp.error}")
                except FileNotFoundError:
                    console.print(f"[red]✗ File not found:[/red] {filepath}")
                except Exception as e:
                    console.print(f"[red]✗ Load error:[/red] {e}")

            elif cmd == "params":
                store.print_table(plugin_name)

            elif cmd == "presets":
                preset_store.print_table()

            elif cmd == "load-preset" and len(parts) == 2:
                query = parts[1]
                match = preset_store.find_by_query(query)
                if not match:
                    console.print(
                        f"[red]✗ No preset matching '{query}'.[/red] "
                        f"Type [bold]presets[/bold] to list all."
                    )
                else:
                    console.print(
                        f"[dim]Loading preset '{match['name']}'...[/dim]"
                    )
                    try:
                        resp = client.load_preset(plugin_path, match["id"])
                        if resp.success:
                            console.print(
                                f"[green]✓ Preset loaded:[/green] {match['name']}"
                            )
                        else:
                            console.print(
                                f"[red]✗ Load failed:[/red] {resp.error}"
                            )
                    except Exception as e:
                        console.print(f"[red]✗ Preset error:[/red] {e}")

            elif cmd == "send-audio" and len(parts) == 2:
                if not enable_audio or not audio_ring:
                    console.print(
                        "[red]✗ Audio not enabled.[/red] "
                        "Use [bold]open-gui --audio[/bold] to enable."
                    )
                else:
                    _handle_send_audio(audio_ring, parts[1], console)

            elif cmd == "send-audio-grpc" and len(parts) == 2:
                if not enable_audio:
                    console.print(
                        "[red]✗ Audio not enabled.[/red] "
                        "Use [bold]open-gui --audio[/bold] to enable."
                    )
                else:
                    _handle_send_audio_grpc(
                        client, plugin_path, parts[1], console,
                        block_size=block_size, num_channels=num_channels,
                        sample_rate=sample_rate,
                    )

            elif cmd == "play-audio" and len(parts) == 2:
                if not enable_audio or not audio_ring:
                    console.print(
                        "[red]✗ Audio not enabled.[/red] "
                        "Use [bold]open-gui --audio[/bold] to enable."
                    )
                elif not audio_device:
                    console.print(
                        "[red]✗ No audio device.[/red] "
                        "Use [bold]open-gui --audio --audio-device sdl3[/bold] for real-time playback."
                    )
                else:
                    audio_playback.start_play_audio(audio_ring, parts[1], console)

            elif cmd == "play-audio-looped" and len(parts) == 2:
                if not enable_audio or not audio_ring:
                    console.print(
                        "[red]✗ Audio not enabled.[/red] "
                        "Use [bold]open-gui --audio[/bold] to enable."
                    )
                elif not audio_device:
                    console.print(
                        "[red]✗ No audio device.[/red] "
                        "Use [bold]open-gui --audio --audio-device sdl3[/bold] for real-time playback."
                    )
                else:
                    audio_playback.start_play_audio_looped(audio_ring, parts[1], console)

            elif cmd == "stop-audio":
                audio_playback.stop(console)

            elif cmd == "help":
                console.print("[bold]Session commands:[/bold]")
                console.print("  open-gui              — Open the plugin's native GUI window")
                console.print("  close-gui             — Close the GUI window (session stays alive)")
                console.print("  save-state <file>     — Save plugin state to file")
                console.print("  load-state <file>     — Restore plugin state from file")
                console.print("  presets               — List available presets")
                console.print("  load-preset <#|name>  — Load a preset by index or name")
                console.print("  params                — Print all parameters")
                if enable_audio:
                    console.print("  send-audio <file.wav> — Process via shared memory (write output file)")
                    console.print("  send-audio-grpc <file.wav> — Process via gRPC stream (networked)")
                    if audio_device:
                        console.print("  play-audio <file.wav>  — Real-time playback (background)")
                        console.print("  play-audio-looped <file.wav> — Looped playback (background)")
                        console.print("  stop-audio            — Stop active playback")
                console.print("  back                  — End session and return to plugin selection")
                console.print("  quit                  — End session and exit")

            else:
                console.print(
                    f"[dim]Unknown command: '{cmd}'. Type 'help' for commands.[/dim]"
                )
                continue

            # After every successful command, show available commands
            if not session_done.is_set():
                console.print(f"\n[dim]Commands: {_commands_hint()}[/dim]\n")

    except KeyboardInterrupt:
        console.print("\n[yellow]Exiting...[/yellow]")
        audio_playback.stop()
        try:
            client.close_plugin_session(plugin_path)
        except Exception:
            pass
        session_done.set()

    stream_thread.join(timeout=3)
    return False


def run_open_gui(
    client: RpsClient, format_filter: str = "", enable_audio: bool = False,
    sample_rate: int = 48000, num_channels: int = 2, block_size: int = 128,
    audio_device: str = "",
) -> None:
    """Main logic for the open-gui command. Loops so user can open multiple plugins."""
    last_selected = None

    while True:
        selection = _list_and_select_plugin(client, format_filter, last_selected)
        if not selection:
            return

        last_selected = selection  # Remember (path, format) for cursor position

        plugin_path, fmt = selection
        console.print(f"\n[bold]Selected:[/bold] {plugin_path} ({fmt})")

        # Start a headless session — user can open-gui, send-audio, etc.
        should_continue = _open_gui_session(
            client, plugin_path, fmt,
            enable_audio=enable_audio,
            sample_rate=sample_rate,
            num_channels=num_channels,
            block_size=block_size,
            audio_device=audio_device,
        )

        if not should_continue:
            return

        console.print()  # Blank line before next selection

