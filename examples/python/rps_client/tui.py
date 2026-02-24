"""Rich TUI display for scan progress."""

import threading
import time
from dataclasses import dataclass, field

from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.progress import (
    Progress,
    BarColumn,
    TextColumn,
    TaskProgressColumn,
    TimeElapsedColumn,
    SpinnerColumn,
    TaskID,
)
from rich.table import Table
from rich.text import Text

from rps_client.proto import rps_pb2


@dataclass
class WorkerState:
    plugin_filename: str = ""
    plugin_path: str = ""
    percentage: int = 0
    stage: str = ""
    idle: bool = True
    last_result: str = ""  # e.g. "✓ Serum v1.35" or "✗ CRASH"


@dataclass
class ScanState:
    """Shared state updated by the event consumer, read by the renderer."""

    total_plugins: int = 0
    worker_count: int = 0
    skipped_unchanged: int = 0
    skipped_blocked: int = 0
    completed: int = 0
    success: int = 0
    fail: int = 0
    crash: int = 0
    timeout: int = 0
    skipped: int = 0
    workers: dict[int, WorkerState] = field(default_factory=dict)
    recent: list[str] = field(default_factory=list)
    finished: bool = False
    total_ms: int = 0
    failures: list[tuple[str, str]] = field(default_factory=list)
    mode: str = ""
    formats: str = ""

    MAX_RECENT = 12

    def add_recent(self, line: str) -> None:
        self.recent.append(line)
        if len(self.recent) > self.MAX_RECENT:
            self.recent = self.recent[-self.MAX_RECENT :]


def process_event(event: rps_pb2.ScanEvent, state: ScanState) -> None:
    """Update ScanState from a single ScanEvent."""
    which = event.WhichOneof("event")

    if which == "scan_started":
        e = event.scan_started
        state.total_plugins = e.total_plugins
        state.worker_count = e.worker_count
        state.skipped_unchanged = e.skipped_unchanged
        state.skipped_blocked = e.skipped_blocked
        for i in range(1, e.worker_count + 1):
            state.workers.setdefault(i, WorkerState())

    elif which == "plugin_started":
        e = event.plugin_started
        ws = state.workers.setdefault(e.worker_id, WorkerState())
        ws.plugin_filename = e.plugin_filename
        ws.plugin_path = e.plugin_path
        ws.percentage = 0
        ws.stage = "Starting..."
        ws.idle = False

    elif which == "plugin_progress":
        e = event.plugin_progress
        ws = state.workers.get(e.worker_id)
        if ws:
            ws.percentage = e.percentage
            ws.stage = e.stage

    elif which == "plugin_completed":
        e = event.plugin_completed
        state.completed += 1
        ws = state.workers.get(e.worker_id)

        outcome = e.outcome
        if outcome == rps_pb2.OUTCOME_SUCCESS:
            state.success += 1
            line = f"[green]✓[/green] {e.plugin_filename} → {e.plugin_name} v{e.plugin_version} ({e.elapsed_ms}ms)"
            if ws:
                ws.last_result = f"✓ {e.plugin_name}"
        elif outcome == rps_pb2.OUTCOME_FAIL:
            state.fail += 1
            line = f"[red]✗[/red] {e.plugin_filename} → {e.error_message} ({e.elapsed_ms}ms)"
            if ws:
                ws.last_result = "✗ FAIL"
        elif outcome == rps_pb2.OUTCOME_CRASH:
            state.crash += 1
            line = f"[red]💥[/red] {e.plugin_filename} → {e.error_message} ({e.elapsed_ms}ms)"
            if ws:
                ws.last_result = "💥 CRASH"
        elif outcome == rps_pb2.OUTCOME_TIMEOUT:
            state.timeout += 1
            line = f"[yellow]⏱[/yellow] {e.plugin_filename} → TIMEOUT ({e.elapsed_ms}ms)"
            if ws:
                ws.last_result = "⏱ TIMEOUT"
        elif outcome == rps_pb2.OUTCOME_SKIPPED:
            state.skipped += 1
            line = f"[dim]⊘[/dim] {e.plugin_filename} → {e.error_message}"
            if ws:
                ws.last_result = "⊘ SKIP"
        else:
            line = f"? {e.plugin_filename}"

        state.add_recent(line)
        if ws:
            ws.idle = True
            ws.percentage = 0
            ws.stage = ""
            ws.plugin_filename = ""

    elif which == "plugin_retry":
        e = event.plugin_retry
        filename = e.plugin_path.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
        state.add_recent(
            f"[yellow]↻[/yellow] {filename} retry {e.attempt}/{e.max_retries}: {e.reason}"
        )

    elif which == "scan_completed":
        e = event.scan_completed
        state.success = e.success
        state.fail = e.fail
        state.crash = e.crash
        state.timeout = e.timeout
        state.skipped = e.skipped
        state.total_ms = e.total_ms
        state.failures = [(f.path, f.reason) for f in e.failures]
        state.finished = True


class ScanDisplay:
    """Manages persistent Rich renderables to avoid flicker.

    Instead of rebuilding a Layout from scratch every frame (which causes
    Rich to clear and redraw the entire terminal), we keep a single
    Progress bar for the overall scan and update worker/recent tables
    in-place via a Group renderable.
    """

    def __init__(self, state: ScanState):
        self.state = state
        self.overall = Progress(
            SpinnerColumn(),
            TextColumn("[bold blue]{task.description}"),
            BarColumn(bar_width=40),
            TaskProgressColumn(),
            TextColumn("{task.fields[stats]}"),
            TimeElapsedColumn(),
        )
        self.overall_task = self.overall.add_task(
            "Scanning", total=1, stats="", start=True
        )

    def _build_worker_table(self) -> Table:
        t = Table(show_header=False, box=None, padding=(0, 1), expand=True)
        t.add_column("id", width=5, no_wrap=True)
        t.add_column("bar", width=20, no_wrap=True)
        t.add_column("info", ratio=1, no_wrap=True, overflow="ellipsis")

        for wid in sorted(self.state.workers.keys()):
            ws = self.state.workers[wid]
            if ws.idle:
                t.add_row(
                    f"[dim]#{wid}[/dim]",
                    "[dim]idle[/dim]",
                    f"[dim]{ws.last_result}[/dim]" if ws.last_result else "",
                )
            else:
                p = ws.percentage
                w = 15
                filled = int(w * p / 100)
                bar_str = f"[cyan]{'━' * filled}[/cyan]{'╌' * (w - filled)}"
                t.add_row(
                    f"[bold]#{wid}[/bold]",
                    f"{bar_str} {p:3d}%",
                    f"[bold]{ws.plugin_filename}[/bold] [dim]{ws.stage}[/dim]"
                    if ws.stage
                    else f"[bold]{ws.plugin_filename}[/bold]",
                )
        return t

    def _build_recent(self) -> Text:
        text = Text()
        for line in self.state.recent:
            text.append_text(Text.from_markup(line + "\n"))
        return text

    def update(self) -> None:
        """Sync Progress bar state with scan state."""
        s = self.state
        total = max(s.total_plugins, 1)
        self.overall.update(
            self.overall_task,
            total=total,
            completed=s.completed,
            description=f"{s.mode.title()} scan ({s.formats})",
            stats=f"✓{s.success} ✗{s.fail} 💥{s.crash} ⏱{s.timeout} ⊘{s.skipped}",
        )

    def __rich_console__(self, console, options):
        """Implement Rich renderable protocol — called by Live on each refresh."""
        self.update()
        group = Group(
            self.overall,
            Text(),
            Panel(self._build_worker_table(), title="Workers", border_style="cyan"),
            Panel(self._build_recent(), title="Recent", border_style="green"),
        )
        yield from group.__rich_console__(console, options)


def run_tui(event_stream, mode: str = "", formats: str = "") -> ScanState:
    """Run the TUI, consuming events from the gRPC stream. Returns final state."""
    state = ScanState(mode=mode, formats=formats)
    console = Console()
    display = ScanDisplay(state)

    # Consumer thread: reads gRPC events and updates state
    def consume():
        try:
            for event in event_stream:
                process_event(event, state)
        except Exception:
            pass
        finally:
            state.finished = True

    consumer = threading.Thread(target=consume, daemon=True)
    consumer.start()

    # Let Rich's Live handle the render cadence — no manual sleep loop.
    # The display object implements __rich_console__ so Live calls it
    # on each refresh tick without rebuilding anything from scratch.
    with Live(display, console=console, refresh_per_second=8, screen=False) as live:
        while not state.finished:
            time.sleep(0.15)
        # Final refresh
        display.update()
        live.refresh()

    consumer.join(timeout=5)
    return state
