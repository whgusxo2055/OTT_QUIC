#!/usr/bin/env bash
# ffmpeg fMP4 세그먼트 생성 스크립트
# 사용법: tools/segment_video.sh <video_id> <input_file> [output_dir]
# 결과: data/segments/<video_id>/init-stream0.m4s, chunk-stream0-00000.m4s ...

set -euo pipefail

VIDEO_ID="$1"
INPUT="$2"
OUT_BASE="${3:-data/segments}"

OUT_DIR="$OUT_BASE/$VIDEO_ID"
mkdir -p "$OUT_DIR"

ffmpeg -y -i "$INPUT" \
  -map 0:v -map 0:a? \
  -c:v copy -c:a copy \
  -f segment \
  -segment_time 2 \
  -segment_format mp4 \
  -movflags +frag_keyframe+empty_moov+default_base_moof \
  "$OUT_DIR/chunk-stream0-%05d.m4s"

# init segment 생성
ffmpeg -y -i "$INPUT" -map 0:v -map 0:a? -c:v copy -c:a copy \
  -f mp4 -movflags +frag_keyframe+empty_moov+default_base_moof \
  "$OUT_DIR/init-stream0.m4s"

echo "Segments written to $OUT_DIR"
