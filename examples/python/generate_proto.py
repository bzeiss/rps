#!/usr/bin/env python3
"""Generate Python gRPC stubs from rps.proto and fix imports."""

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
PROTO_DIR = SCRIPT_DIR / ".." / ".." / "proto"
OUT_DIR = SCRIPT_DIR / "rps_client" / "proto"

# Generate stubs
subprocess.check_call([
    sys.executable, "-m", "grpc_tools.protoc",
    f"-I{PROTO_DIR}",
    f"--python_out={OUT_DIR}",
    f"--grpc_python_out={OUT_DIR}",
    str(PROTO_DIR / "rps.proto"),
])

# Fix grpc_tools' broken absolute import -> relative import
grpc_file = OUT_DIR / "rps_pb2_grpc.py"
text = grpc_file.read_text()
text = text.replace("import rps_pb2 as rps__pb2", "from . import rps_pb2 as rps__pb2")
grpc_file.write_text(text)

print(f"Generated and patched proto stubs in {OUT_DIR}")
