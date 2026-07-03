#!/usr/bin/env bash
set -euo pipefail

ENV_CONTENT=$(cat <<EOF
UID=$(id -u)
GID=$(id -g)
USERNAME=user
HOME=/home/user
EOF
)

for dir in docker/*/; do
  [ -d "$dir" ] || continue
  printf '%s\n' "$ENV_CONTENT" > "${dir}.env"
done
