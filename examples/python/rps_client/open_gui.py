"""Open a plugin's native GUI via gRPC and display lifecycle events with parameter dump."""

import sys

import click
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


def _list_and_select_plugin(client: RpsClient, format_filter: str = "") -> tuple[str, str] | None:
    """List plugins and let the user select one. Returns (path, format) or None."""
    plugins = client.list_plugins(format_filter=format_filter)

    if not plugins:
        console.print("[yellow]No plugins found in the database.[/yellow]")
        console.print("Run a scan first: [bold]python -m rps_client scan[/bold]")
        return None

    # Build a rich table
    table = Table(title="Scanned Plugins", show_lines=False)
    table.add_column("#", style="dim", width=4)
    table.add_column("Format", style="cyan", width=6)
    table.add_column("Name", style="bold")
    table.add_column("Vendor", style="green")
    table.add_column("Version", style="dim")
    table.add_column("Path", style="dim", max_width=60, no_wrap=True)

    for i, p in enumerate(plugins, start=1):
        table.add_row(
            str(i),
            p.format.upper(),
            p.name or "(unknown)",
            p.vendor or "",
            p.version or "",
            p.path,
        )

    console.print(table)
    console.print()

    # Prompt for selection
    while True:
        try:
            choice = console.input("[bold]Enter plugin number (0 to cancel): [/bold]")
            num = int(choice.strip())
            if num == 0:
                return None
            if 1 <= num <= len(plugins):
                selected = plugins[num - 1]
                return (selected.path, selected.format)
            console.print(f"[red]Please enter a number between 1 and {len(plugins)}.[/red]")
        except (ValueError, EOFError):
            return None


def run_open_gui(client: RpsClient, format_filter: str = "") -> None:
    """Main logic for the open-gui command."""
    selection = _list_and_select_plugin(client, format_filter)
    if not selection:
        return

    plugin_path, fmt = selection
    console.print(f"\n[bold]Opening GUI:[/bold] {plugin_path} ({fmt})")
    console.print("[dim]Press Ctrl+C to close...[/dim]\n")

    store = ParameterStore()
    plugin_name = ""

    try:
        event_stream = client.open_plugin_gui(plugin_path, fmt)
        for event in event_stream:
            if event.HasField("gui_opened"):
                g = event.gui_opened
                plugin_name = g.plugin_name
                console.print(
                    f"[green]✓ GUI Opened:[/green] {g.plugin_name} "
                    f"({g.width}×{g.height})"
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
                break
            elif event.HasField("gui_error"):
                e = event.gui_error
                console.print(f"[red]✗ Error:[/red] {e.error}")
                if e.details:
                    console.print(f"  [dim]{e.details}[/dim]")

    except KeyboardInterrupt:
        console.print("\n[yellow]Closing GUI...[/yellow]")
        try:
            client.close_plugin_gui(plugin_path)
        except Exception:
            pass
    except Exception as e:
        console.print(f"[red]Error: {e}[/red]")
        sys.exit(1)
