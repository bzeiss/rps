from rps_client.client import RpsClient

with RpsClient("localhost:50051") as client:
    # Graph API — full control
    resp = client.create_graph(sample_rate=48000, block_size=128)
    gid = resp.graph_id
    client.add_node(gid, "in", "input", io_layout={"format": "stereo", "channel_count": 2})
    client.add_node(gid, "fx", "plugin", plugin_path="C:/Program Files/Common Files/VST3/AIR Chorus.vst3", plugin_format="vst3")
    client.add_node(gid, "out", "output", io_layout={"format": "stereo", "channel_count": 2})
    client.connect_nodes(gid, "in", 0, "fx", 0)
    client.connect_nodes(gid, "fx", 0, "out", 0)
    client.activate_graph(gid)
    info = client.get_graph_info(gid)
    print(f"Graph {gid}: {info.state}, {info.node_count} nodes, {info.slice_count} slices")

    # Chain API — one-liner
    resp = client.create_chain([
        {"plugin_path": "/path/to/eq.vst3", "format": "vst3"},
        {"plugin_path": "/path/to/comp.clap", "format": "clap"},
    ])
    print(f"Chain created: {resp.graph_id}, success={resp.success}")
