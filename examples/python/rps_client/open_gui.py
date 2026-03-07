"""Open a plugin's native GUI via gRPC and display lifecycle events with parameter dump."""

import sys
import threading
import time

from rich.console import Console
from rich.table import Table

from rps_client.client import RpsClient


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

        result = prompt.execute()
    except KeyboardInterrupt:
        return None

    return result


def run_open_gui(client: RpsClient, format_filter: str = "") -> None:
    """Main logic for the open-gui command. Loops so user can open multiple plugins."""
    last_selected = None

    while True:
        selection = _list_and_select_plugin(client, format_filter, last_selected)
        if not selection:
            return

        last_selected = selection  # Remember (path, format) for cursor position

        plugin_path, fmt = selection
        console.print(f"\n[bold]Opening GUI:[/bold] {plugin_path} ({fmt})")

        store = ParameterStore()
        preset_store = PresetStore()
        plugin_name = ""
        gui_closed = threading.Event()

        def _stream_consumer():
            """Background thread: consumes gRPC event stream."""
            nonlocal plugin_name
            try:
                event_stream = client.open_plugin_gui(plugin_path, fmt)
                for event in event_stream:
                    if gui_closed.is_set():
                        break
                    if event.HasField("gui_opened"):
                        g = event.gui_opened
                        plugin_name = g.plugin_name
                        console.print(
                            f"[green]✓ GUI Opened:[/green] {g.plugin_name} "
                            f"({g.width}×{g.height})"
                        )
                        console.print(
                            "[dim]Commands: save-state <file>, load-state <file>, "
                            "presets, load-preset <#|name>, params, help, quit[/dim]\n"
                        )
                    elif event.HasField("parameter_list"):
                        store.load_list(event.parameter_list)
                        store.print_table(plugin_name)
                        console.print()
                    elif event.HasField("parameter_updates"):
                        changed = store.apply_updates(event.parameter_updates)
                        for p in changed:
                            val_str = p['display'] or f"{p['value']:.4f}"
                            console.print(
                                f"  [cyan]⟳[/cyan] {p['name']}: [bold]{val_str}[/bold]"
                            )
                    elif event.HasField("gui_closed"):
                        console.print(
                            f"\n[yellow]✗ GUI Closed:[/yellow] {event.gui_closed.reason}"
                        )
                        gui_closed.set()
                        break
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
            except Exception as e:
                if not gui_closed.is_set():
                    console.print(f"[red]Stream error: {e}[/red]")
                    gui_closed.set()

        # Start stream consumer thread
        stream_thread = threading.Thread(target=_stream_consumer, daemon=True)
        stream_thread.start()

        # Main thread: accept commands
        try:
            while not gui_closed.is_set():
                line = _read_line_nonblocking(gui_closed)
                if line is None:
                    break  # GUI closed or EOF
                if not line:
                    continue

                parts = line.split(maxsplit=1)
                cmd = parts[0].lower()

                if cmd in ("quit", "exit", "close", "q"):
                    console.print("[yellow]Closing GUI...[/yellow]")
                    try:
                        client.close_plugin_gui(plugin_path)
                    except Exception:
                        pass
                    gui_closed.set()
                    break

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

                elif cmd == "help":
                    console.print("[bold]Available commands:[/bold]")
                    console.print("  save-state <file>     — Save plugin state to file")
                    console.print("  load-state <file>     — Restore plugin state from file")
                    console.print("  presets               — List available presets")
                    console.print("  load-preset <#|name>  — Load a preset by index or name")
                    console.print("  params                — Print all parameters")
                    console.print("  quit                  — Close the GUI and exit")

                else:
                    console.print(
                        f"[dim]Unknown command: '{cmd}'. Type 'help' for commands.[/dim]"
                    )

        except KeyboardInterrupt:
            console.print("\n[yellow]Closing GUI...[/yellow]")
            try:
                client.close_plugin_gui(plugin_path)
            except Exception:
                pass
            gui_closed.set()

        stream_thread.join(timeout=3)
        console.print()  # Blank line before next selection

