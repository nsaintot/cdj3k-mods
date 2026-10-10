#!/bin/sh
# for the stems feature, adds a stem sidecar.
set -e

cat > /run/systemd/system/cdj3k-mods-stemd.service <<UNIT
[Unit]
Description=stemd client (STEMS sidecar for cdj3k-mods)

[Service]
ExecStart=$MOD_DIR/stemd_client
Restart=on-failure
RestartSec=2s
UNIT
systemctl daemon-reload
systemctl --no-block start cdj3k-mods-stemd.service
