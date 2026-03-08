"""Chain (multi-plugin graph) management — interactive REPL."""

import os

from rich.console import Console
from rich.table import Table

from rps_client.client import RpsClient

console = Console()


def run_chain(
    client: RpsClient,
    format_filter: str = "",
    sample_rate: int = 48000,
    block_size: int = 128,
    num_channels: int = 2,
) -> None:
    """Interactive chain management REPL."""
    console.print("\n[bold]Chain Manager[/bold] — multi-plugin graph processing")
    console.print("[dim]Type 'help' for commands, 'quit' to exit.[/dim]\n")

    try:
        while True:
            try:
                line = input("chain> ").strip()
            except EOFError:
                break

            if not line:
                continue

            parts = line.split(None, 1)
            cmd = parts[0].lower()

            if cmd in ("quit", "exit", "q"):
                break

            elif cmd == "create":
                paths = _parse_paths(parts[1]) if len(parts) > 1 else []
                _do_create(
                    client, paths, format_filter,
                    sample_rate, block_size, num_channels,
                )

            elif cmd == "info" and len(parts) == 2:
                _do_info(client, parts[1].strip())

            elif cmd == "destroy" and len(parts) == 2:
                _do_destroy(client, parts[1].strip())

            elif cmd == "help":
                _print_help()

            else:
                console.print(f"[dim]Unknown: '{line}'. Type 'help' for commands.[/dim]")

    except KeyboardInterrupt:
        console.print("\n[yellow]Interrupted.[/yellow]")


def _print_help() -> None:
    console.print("[bold]Chain commands:[/bold]")
    console.print('  create <path1> <path2> ...  — Create chain from plugin paths')
    console.print("  create                     — Interactive plugin selection")
    console.print("  info <graph_id>            — Show graph info")
    console.print("  destroy <graph_id>         — Destroy a chain")
    console.print("  quit                       — Exit")


def _parse_paths(arg_string: str) -> list[str]:
    """Parse plugin paths from command line, handling quoted paths."""
    import shlex
    try:
        return shlex.split(arg_string)
    except ValueError:
        return arg_string.split()


def _do_create(
    client: RpsClient,
    plugin_paths: list[str],
    format_filter: str,
    sample_rate: int,
    block_size: int,
    num_channels: int,
) -> None:
    if not plugin_paths:
        plugin_paths = _interactive_select_plugins(client, format_filter)
        if not plugin_paths:
            return

    plugins = []
    for path in plugin_paths:
        fmt = _detect_format(path)
        if not fmt:
            console.print(f"[red]✗ Unknown format:[/red] {path}")
            return
        plugins.append({"plugin_path": path, "format": fmt})

    console.print(f"\n[bold]Creating chain with {len(plugins)} plugin(s):[/bold]")
    for i, p in enumerate(plugins, 1):
        name = os.path.basename(p["plugin_path"])
        console.print(f"  {i}. {name} [dim]({p['format']})[/dim]")

    try:
        resp = client.create_chain(
            plugins=plugins,
            sample_rate=sample_rate,
            block_size=block_size,
            num_channels=num_channels,
        )
    except Exception as e:
        console.print(f"\n[red]✗ CreateChain failed:[/red] {e}")
        return

    if resp.success:
        console.print(
            f"\n[green]✓ Chain created:[/green] [bold]{resp.graph_id}[/bold]"
        )
        try:
            _do_info(client, resp.graph_id)
        except Exception as e:
            console.print(f"[dim]Info fetch failed: {type(e).__name__}: {e}[/dim]")
    else:
        console.print(f"\n[red]✗ Failed:[/red] {resp.error}")


def _do_info(client: RpsClient, graph_id: str) -> None:
    try:
        resp = client.get_graph_info(graph_id)

        table = Table(title=f"Graph: {graph_id}", show_lines=False)
        table.add_column("Property", style="bold", width=14)
        table.add_column("Value", style="cyan")
        table.add_row("State", resp.state or "unknown")
        table.add_row("Nodes", str(resp.node_count))
        table.add_row("Edges", str(resp.edge_count))
        table.add_row("Slices", str(resp.slice_count))
        table.add_row("Strategy", resp.strategy or "n/a")
        console.print(table)
    except Exception as e:
        console.print(f"[red]\u2717 GetGraphInfo failed:[/red] {type(e).__name__}: {e}")


def _do_destroy(client: RpsClient, graph_id: str) -> None:
    try:
        client.destroy_chain(graph_id)
        console.print(f"[green]✓ Chain {graph_id} destroyed.[/green]")
    except Exception as e:
        console.print(f"[red]✗ DestroyChain failed:[/red] {e}")


def _detect_format(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    return {".vst3": "vst3", ".clap": "clap", ".component": "au"}.get(ext, "")


def _interactive_select_plugins(
    client: RpsClient, format_filter: str,
) -> list[str]:
    """Multi-select plugins from the scanned database."""
    from InquirerPy import inquirer
    from InquirerPy.base.control import Choice

    plugins = client.list_plugins(format_filter=format_filter)
    if not plugins:
        console.print("[yellow]No plugins found. Run a scan first.[/yellow]")
        return []

    choices = []
    for p in plugins:
        fmt = p.format.upper()
        name = p.name or "(unknown)"
        vendor = p.vendor or ""
        label = f"[{fmt}]  {name}"
        if vendor:
            label += f"  ({vendor})"
        choices.append(Choice(value=p.path, name=label))

    try:
        selected = inquirer.checkbox(
            message="Select plugins (Space=toggle, Enter=confirm):",
            choices=choices, max_height="70%",
        ).execute()
    except KeyboardInterrupt:
        return []

    if not selected:
        console.print("[dim]No plugins selected.[/dim]")
    return selected or []
