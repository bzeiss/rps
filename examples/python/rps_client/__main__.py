"""Entry point: python -m rps_client"""

import sys
import os
import click

from rps_client.client import RpsClient
from rps_client.server_manager import ServerManager
from rps_client.tui import run_tui


@click.group()
@click.option("--server", default=None, help="Connect to an already-running server (e.g. localhost:50051)")
@click.option("--server-bin", default=None, help="Path to rps-server binary (auto-detected if not set)")
@click.option("--port", default=50051, type=int, help="Server port (default: 50051)")
@click.option("--db", default="rps-plugins.db", help="Database file path")
@click.pass_context
def cli(ctx, server, server_bin, port, db):
    """RPS Plugin Scanner - Python TUI Client"""
    ctx.ensure_object(dict)
    ctx.obj["server"] = server
    ctx.obj["server_bin"] = server_bin
    ctx.obj["port"] = port
    ctx.obj["db"] = db


@cli.command()
@click.option("--formats", "-f", default="all", help="Comma-separated formats or 'all'")
@click.option("--mode", "-m", default="incremental", type=click.Choice(["full", "incremental"]))
@click.option("--jobs", "-j", default=6, type=int, help="Parallel workers")
@click.option("--retries", "-r", default=3, type=int, help="Max retries per plugin")
@click.option("--timeout", "-t", default=120000, type=int, help="Per-plugin timeout in ms")
@click.option("--limit", "-l", default=0, type=int, help="Max plugins to scan (0 = unlimited)")
@click.option("--filter", "filter_str", default="", help="Filename substring filter")
@click.option("--scan", "single_plugin", default="", help="Single plugin file to scan")
@click.option("--scan-dir", "scan_dirs", multiple=True, help="Directories to scan")
@click.option("--verbose", "-v", is_flag=True, help="Enable verbose logging")
@click.pass_context
def scan(ctx, formats, mode, jobs, retries, timeout, limit, filter_str, single_plugin, scan_dirs, verbose):
    """Start a plugin scan with TUI progress display."""
    server_addr = ctx.obj["server"]
    managed = server_addr is None

    if managed:
        server_bin = ctx.obj["server_bin"] or _find_server_bin()
        if not server_bin:
            click.echo("Error: Cannot find rps-server binary. Use --server-bin or --server.", err=True)
            sys.exit(1)

        mgr = ServerManager(
            server_bin=server_bin,
            port=ctx.obj["port"],
            db=ctx.obj["db"],
            log_level="debug" if verbose else "info",
        )
        click.echo(f"Starting rps-server ({server_bin})...")
        try:
            mgr.start()
        except Exception as e:
            click.echo(f"Error starting server: {e}", err=True)
            sys.exit(1)
        server_addr = mgr.address
    else:
        mgr = None

    try:
        with RpsClient(server_addr) as client:
            event_stream = client.start_scan(
                scan_dirs=list(scan_dirs),
                single_plugin=single_plugin,
                mode=mode,
                formats=formats,
                filter_str=filter_str,
                limit=limit,
                jobs=jobs,
                retries=retries,
                timeout_ms=timeout,
                verbose=verbose,
            )
            state = run_tui(event_stream, mode=mode, formats=formats)

        # Print final summary
        click.echo()
        if state.failures:
            click.echo(f"Failed plugins ({len(state.failures)}):")
            for path, reason in state.failures:
                click.echo(f"  {path}")
                click.echo(f"    -> {reason}")
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)
    finally:
        if mgr:
            mgr.stop()


@cli.command()
@click.pass_context
def status(ctx):
    """Query server status."""
    server_addr = ctx.obj["server"]
    if not server_addr:
        server_addr = f"localhost:{ctx.obj['port']}"

    try:
        with RpsClient(server_addr) as client:
            resp = client.get_status()
            state_str = "SCANNING" if resp.state == 1 else "IDLE"
            click.echo(f"Server: {server_addr}")
            click.echo(f"State:  {state_str}")
            click.echo(f"Uptime: {resp.uptime_ms}ms")
            click.echo(f"DB:     {resp.db_path}")
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)


@cli.command()
@click.pass_context
def shutdown(ctx):
    """Shut down the server."""
    server_addr = ctx.obj["server"]
    if not server_addr:
        server_addr = f"localhost:{ctx.obj['port']}"

    try:
        with RpsClient(server_addr) as client:
            client.shutdown()
            click.echo("Shutdown request sent.")
    except Exception as e:
        click.echo(f"Error: {e}", err=True)
        sys.exit(1)


def _find_server_bin() -> str | None:
    """Try to locate rps-server binary relative to this script or on PATH."""
    # Check common build output locations relative to project root
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, "..", "..", ".."))

    candidates = [
        os.path.join(project_root, "build", "apps", "rps-server", "rps-server.exe"),
        os.path.join(project_root, "build", "apps", "rps-server", "rps-server"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c

    # Check PATH
    import shutil
    found = shutil.which("rps-server") or shutil.which("rps-server.exe")
    return found


def main():
    cli()


if __name__ == "__main__":
    main()
