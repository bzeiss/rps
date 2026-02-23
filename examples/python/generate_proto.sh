#!/bin/bash
# Generate Python gRPC stubs from rps.proto
# Run from the examples/python/ directory

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROTO_DIR="$SCRIPT_DIR/../../proto"
OUT_DIR="$SCRIPT_DIR/rps_client/proto"

python -m grpc_tools.protoc \
    -I "$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/rps.proto"

echo "Generated Python proto stubs in $OUT_DIR"
