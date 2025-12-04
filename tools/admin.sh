#!/usr/bin/env bash
set -euo pipefail

API_HOST="${API_HOST:-localhost}"
API_PORT="${API_PORT:-8443}"

upload() {
  local file="$1"
  local title="${2:-$(basename "$file")}"
  local desc="${3:-uploaded via admin.sh}"
  curl -v -X POST "http://$API_HOST:$API_PORT/upload" \
    -H "Content-Type: multipart/form-data" \
    -F "file=@${file}" \
    -F "title=${title}" \
    -F "description=${desc}"
}

case "${1:-}" in
  upload)
    if [ -z "${2:-}" ]; then
      echo "Usage: $0 upload <file> [title] [description]"
      exit 1
    fi
    upload "$2" "${3:-}" "${4:-}"
    ;;
  *)
    echo "Usage: $0 upload <file> [title] [description]"
    ;;
esac
