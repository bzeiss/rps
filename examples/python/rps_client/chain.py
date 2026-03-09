"""Chain (multi-plugin graph) management — interactive REPL."""

import os

from rich.console import Console
from rich.table import Table

from rps_client.client import RpsClient

console = Console()

# Session-local name → graph_id map, populated by create --name
_name_map: dict[str, str] = {}


def _resolve(id_or_name: str) -> str:
    """Resolve a graph ID or user-assigned name to a graph ID."""
    return _name_map.get(id_or_name, id_or_name)


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
            args = parts[1].strip() if len(parts) > 1 else ""

            if cmd in ("quit", "exit", "q"):
                break
            elif cmd == "create":
                _do_create(
                    client, args, format_filter,
                    sample_rate, block_size, num_channels,
                )
            elif cmd == "info":
                if args:
                    _do_info(client, _resolve(args))
                else:
                    console.print("[dim]Usage: info <graph_id|name>[/dim]")
            elif cmd == "detail":
                if args:
                    _do_detail(client, _resolve(args))
                else:
                    console.print("[dim]Usage: detail <graph_id|name>[/dim]")
            elif cmd == "destroy":
                if args:
                    _do_destroy(client, _resolve(args))
                else:
                    console.print("[dim]Usage: destroy <graph_id|name>[/dim]")
            elif cmd == "activate":
                if args:
                    _do_activate(client, _resolve(args))
                else:
                    console.print("[dim]Usage: activate <graph_id|name>[/dim]")
            elif cmd == "deactivate":
                if args:
                    _do_deactivate(client, _resolve(args))
                else:
                    console.print("[dim]Usage: deactivate <graph_id|name>[/dim]")
            elif cmd == "connect":
                _do_connect(client, args)
            elif cmd == "disconnect":
                _do_disconnect(client, args)
            elif cmd == "remove-node":
                _do_remove_node(client, args)
            elif cmd == "help":
                _print_help()
            else:
                console.print(f"[dim]Unknown: '{line}'. Type 'help' for commands.[/dim]")

    except KeyboardInterrupt:
        console.print("\n[yellow]Interrupted.[/yellow]")


def _print_help() -> None:
    console.print("[bold]Chain commands:[/bold]")
    console.print("[bold dim]  Lifecycle:[/bold dim]")
    console.print('  create [--name "Name"] <path1> <path2> ...')
    console.print("  create                         — Interactive plugin selection")
    console.print("  destroy <id|name>              — Destroy a chain")
    console.print("  activate <id|name>             — Activate (start processing)")
    console.print("  deactivate <id|name>           — Deactivate (stop processing)")
    console.print("[bold dim]  Inspect:[/bold dim]")
    console.print("  info <id|name>                 — Show graph summary")
    console.print("  detail <id|name>               — Show nodes + edges")
    console.print("[bold dim]  Edit (requires deactivate first):[/bold dim]")
    console.print("  connect <id> <src>:<port> <dst>:<port>")
    console.print("  disconnect <id> <edge_id>")
    console.print("  remove-node <id> <node_id>")
    console.print("[bold dim]  Session:[/bold dim]")
    console.print("  quit                           — Exit")


def _parse_paths(arg_string: str) -> list[str]:
    """Parse plugin paths from command line, handling quoted paths."""
    import shlex
    try:
        return shlex.split(arg_string)
    except ValueError:
        return arg_string.split()


def _do_create(
    client: RpsClient,
    args: str,
    format_filter: str,
    sample_rate: int,
    block_size: int,
    num_channels: int,
) -> None:
    # Parse --name option
    name = ""
    remaining = args
    if remaining.startswith("--name ") or remaining.startswith('--name "'):
        import shlex
        tokens = shlex.split(remaining)
        if len(tokens) >= 2 and tokens[0] == "--name":
            name = tokens[1]
            remaining = " ".join(f'"{t}"' if " " in t else t for t in tokens[2:])

    plugin_paths = _parse_paths(remaining) if remaining else []
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
        basename = os.path.basename(p["plugin_path"])
        console.print(f"  {i}. {basename} [dim]({p['format']})[/dim]")

    try:
        resp = client.create_chain(
            plugins=plugins,
            sample_rate=sample_rate,
            block_size=block_size,
            num_channels=num_channels,
            name=name,
        )
    except Exception as e:
        console.print(f"\n[red]✗ CreateChain failed:[/red] {e}")
        return

    if resp.success:
        # Register name → graph_id mapping
        if name:
            _name_map[name] = resp.graph_id

        label = f"[bold]{resp.graph_id}[/bold]"
        if name:
            label += f' ("{name}")'
        console.print(f"\n[green]✓ Chain created:[/green] {label}")
        try:
            _do_info(client, resp.graph_id)
        except Exception as e:
            console.print(f"[dim]Info fetch failed: {type(e).__name__}: {e}[/dim]")
    else:
        console.print(f"\n[red]✗ Failed:[/red] {resp.error}")


def _do_info(client: RpsClient, graph_id: str) -> None:
    try:
        resp = client.get_graph_info(graph_id)
        title = f"Graph: {graph_id}"
        if resp.name:
            title += f' ("{resp.name}")'

        table = Table(title=title, show_lines=False)
        table.add_column("Property", style="bold", width=14)
        table.add_column("Value", style="cyan")
        table.add_row("State", resp.state or "unknown")
        table.add_row("Nodes", str(resp.node_count))
        table.add_row("Edges", str(resp.edge_count))
        table.add_row("Slices", str(resp.slice_count))
        table.add_row("Strategy", resp.strategy or "n/a")
        console.print(table)
    except Exception as e:
        console.print(f"[red]✗ GetGraphInfo failed:[/red] {e}")


def _do_detail(client: RpsClient, graph_id: str) -> None:
    """Show full graph content: nodes and edges."""
    try:
        resp = client.get_graph_detail(graph_id)
    except Exception as e:
        console.print(f"[red]✗ GetGraphDetail failed:[/red] {e}")
        return

    title = f"Graph: {graph_id}"
    if resp.name:
        title += f' ("{resp.name}")'
    console.print(f"\n[bold]{title}[/bold]")
    console.print(f"[dim]State: {resp.state} | Strategy: {resp.strategy}[/dim]\n")

    # Nodes table
    node_table = Table(title="Nodes", show_lines=False, expand=False)
    node_table.add_column("#", style="dim", width=3)
    node_table.add_column("ID", style="bold", min_width=12)
    node_table.add_column("Type", style="cyan", width=10)
    node_table.add_column("Ports", style="magenta", min_width=16)
    node_table.add_column("Plugin / Config", style="green")

    for i, n in enumerate(resp.nodes, 1):
        detail = ""
        if n.plugin_path:
            detail = f"{os.path.basename(n.plugin_path)} ({n.format})"

        # Build port description: "in[0..N]:Xch  out[0..M]:Ych"
        port_parts = []
        if n.input_port_count > 0:
            idx = "0" if n.input_port_count == 1 else f"0..{n.input_port_count - 1}"
            port_parts.append(f"in[{idx}]:{n.input_channels}ch")
        if n.output_port_count > 0:
            idx = "0" if n.output_port_count == 1 else f"0..{n.output_port_count - 1}"
            port_parts.append(f"out[{idx}]:{n.output_channels}ch")
        ports = "  ".join(port_parts) if port_parts else "—"

        node_table.add_row(str(i), n.node_id, n.type, ports, detail)

    console.print(node_table)

    # Edges table
    if resp.edges:
        edge_table = Table(title="Edges", show_lines=False, expand=False)
        edge_table.add_column("#", style="dim", width=3)
        edge_table.add_column("ID", style="bold", width=8)
        edge_table.add_column("Connection", style="cyan")

        for i, e in enumerate(resp.edges, 1):
            conn = f"{e.source_node_id}:{e.source_port} → {e.dest_node_id}:{e.dest_port}"
            edge_table.add_row(str(i), e.edge_id, conn)

        console.print(edge_table)
    else:
        console.print("[dim]No edges.[/dim]")


def _do_destroy(client: RpsClient, graph_id: str) -> None:
    try:
        client.destroy_chain(graph_id)
        # Remove from name map
        _name_map.pop(next((k for k, v in _name_map.items() if v == graph_id), ""), None)
        console.print(f"[green]✓ Chain {graph_id} destroyed.[/green]")
    except Exception as e:
        console.print(f"[red]✗ DestroyChain failed:[/red] {e}")


def _do_activate(client: RpsClient, graph_id: str) -> None:
    try:
        resp = client.activate_graph(graph_id)
        if resp.success:
            console.print(
                f"[green]✓ Graph {graph_id} activated[/green] "
                f"[dim]({resp.slice_count} slice(s))[/dim]"
            )
        else:
            console.print(f"[red]✗ Activate failed:[/red] {resp.error}")
    except Exception as e:
        console.print(f"[red]✗ ActivateGraph failed:[/red] {e}")


def _do_deactivate(client: RpsClient, graph_id: str) -> None:
    try:
        client.deactivate_graph(graph_id)
        console.print(f"[green]✓ Graph {graph_id} deactivated.[/green]")
    except Exception as e:
        console.print(f"[red]✗ DeactivateGraph failed:[/red] {e}")


def _do_connect(client: RpsClient, args: str) -> None:
    """connect <graph_id|name> <src_node>:<port> <dst_node>:<port>"""
    parts = args.split()
    if len(parts) != 3 or ":" not in parts[1] or ":" not in parts[2]:
        console.print("[dim]Usage: connect <graph_id|name> <src>:<port> <dst>:<port>[/dim]")
        return

    graph_id = _resolve(parts[0])
    src_node, src_port = parts[1].rsplit(":", 1)
    dst_node, dst_port = parts[2].rsplit(":", 1)

    try:
        resp = client.connect_nodes(
            graph_id, src_node, int(src_port), dst_node, int(dst_port),
        )
        console.print(
            f"[green]✓ Connected:[/green] {src_node}:{src_port} → {dst_node}:{dst_port} "
            f"[dim](edge {resp.edge_id})[/dim]"
        )
    except Exception as e:
        console.print(f"[red]✗ Connect failed:[/red] {e}")


def _do_disconnect(client: RpsClient, args: str) -> None:
    """disconnect <graph_id|name> <edge_id>"""
    parts = args.split()
    if len(parts) != 2:
        console.print("[dim]Usage: disconnect <graph_id|name> <edge_id>[/dim]")
        return

    try:
        client.disconnect_nodes(_resolve(parts[0]), parts[1])
        console.print(f"[green]✓ Edge {parts[1]} removed.[/green]")
    except Exception as e:
        console.print(f"[red]✗ Disconnect failed:[/red] {e}")


def _do_remove_node(client: RpsClient, args: str) -> None:
    """remove-node <graph_id|name> <node_id>"""
    parts = args.split()
    if len(parts) != 2:
        console.print("[dim]Usage: remove-node <graph_id|name> <node_id>[/dim]")
        return

    try:
        client.remove_node(_resolve(parts[0]), parts[1])
        console.print(f"[green]✓ Node {parts[1]} removed.[/green]")
    except Exception as e:
        console.print(f"[red]✗ RemoveNode failed:[/red] {e}")


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
