#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Convenience launcher: stream the public Angkor Wat 3D Tiles sample over HTTP.
# A quick smoke test for the streaming / LOD / frustum-cull path with no local
# dataset. Extra args are forwarded to the viewer, e.g.:
#   scripts/run-angkor.sh --no-msaa
#   scripts/run-angkor.sh --gles --sse-pixels 24
set -euo pipefail

ANGKOR_URL="https://storage.googleapis.com/open-cogs/planet-stac/angkor-wat/3d-geofox.ai/3DTiles/tileset.json"
VIEWER="${VIEWER:-./build/3dtiles-viewer}"

exec "$VIEWER" --mode http --source "$ANGKOR_URL" "$@"
