"""Open a plugin's native GUI via gRPC and display lifecycle events."""

import sys

import click
from rich.console import Console
from rich.table import Table

from rps_client.client import RpsClient


console = Console()


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

    try:
        event_stream = client.open_plugin_gui(plugin_path, fmt)
        for event in event_stream:
            if event.HasField("gui_opened"):
                g = event.gui_opened
                console.print(
                    f"[green]✓ GUI Opened:[/green] {g.plugin_name} "
                    f"({g.width}×{g.height})"
                )
            elif event.HasField("gui_closed"):
                console.print(
                    f"[yellow]✗ GUI Closed:[/yellow] {event.gui_closed.reason}"
                )
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
