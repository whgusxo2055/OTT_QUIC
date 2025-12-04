#!/usr/bin/env bash
# ffmpeg fMP4 세그먼트 생성 스크립트
# 사용법: tools/segment_video.sh <video_id> <input_file> [output_dir]
# 결과: data/segments/<video_id>/init-stream0.m4s, chunk-stream0-00000.m4s ...

set -euo pipefail

VIDEO_ID="$1"
INPUT="$2"
OUT_BASE="${3:-data/segments}"

OUT_DIR="$OUT_BASE/$VIDEO_ID"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# AV1 코덱 그대로 사용 - 트랜스코딩 없이 fMP4 세그먼트 생성
# 오디오만 AAC로 변환 (Opus는 MP4 컨테이너에서 호환성 문제 있을 수 있음)
ffmpeg -y -i "$INPUT" \
  -c:v copy \
  -c:a aac -b:a 128k \
  -f hls \
  -hls_time 2 \
  -hls_playlist_type vod \
  -hls_segment_type fmp4 \
  -hls_fmp4_init_filename 'init-stream0.m4s' \
  -hls_segment_filename "$OUT_DIR/chunk-stream0-%05d.m4s" \
  "$OUT_DIR/playlist.m3u8"

# playlist.m3u8에서 segment_info.json 생성
# 각 세그먼트의 시작/종료 시간 정보 추출
echo "Generating segment_info.json..."

# 총 duration 가져오기
TOTAL_DURATION=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$INPUT")

# 비디오 코덱 감지
VIDEO_CODEC=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$INPUT")
echo "Detected video codec: $VIDEO_CODEC"

# 코덱에 따른 MIME 타입 결정
case "$VIDEO_CODEC" in
  av1)
    CODEC_STRING="av01.0.12M.08"
    ;;
  hevc|h265)
    CODEC_STRING="hvc1.1.6.L120.90"
    ;;
  h264|avc)
    CODEC_STRING="avc1.640028"
    ;;
  vp9)
    CODEC_STRING="vp09.00.10.08"
    ;;
  *)
    CODEC_STRING="avc1.640028"
    echo "Warning: Unknown codec $VIDEO_CODEC, defaulting to H.264"
    ;;
esac

# playlist.m3u8 파싱하여 segment_info.json 생성 (awk 사용 - python 불필요)
awk -v total_dur="$TOTAL_DURATION" -v video_codec="$VIDEO_CODEC" -v codec_string="$CODEC_STRING" '
BEGIN {
    current_time = 0
    seg_count = 0
}
/^#EXTINF:/ {
    gsub(/^#EXTINF:/, "")
    gsub(/,.*$/, "")
    duration = $0 + 0
    getline segment_file
    if (segment_file !~ /^#/) {
        match(segment_file, /[0-9]+\.m4s$/)
        seg_idx = substr(segment_file, RSTART, RLENGTH)
        gsub(/\.m4s$/, "", seg_idx)
        seg_idx = seg_idx + 0
        
        segments[seg_count] = sprintf("    {\"index\": %d, \"start\": %.3f, \"end\": %.3f, \"duration\": %.3f}", seg_idx, current_time, current_time + duration, duration)
        current_time += duration
        seg_count++
    }
}
END {
    print "{"
    printf "  \"total_duration\": %.3f,\n", total_dur
    printf "  \"total_segments\": %d,\n", seg_count
    printf "  \"video_codec\": \"%s\",\n", video_codec
    printf "  \"codec_string\": \"%s\",\n", codec_string
    print "  \"segments\": ["
    for (i = 0; i < seg_count; i++) {
        if (i < seg_count - 1)
            print segments[i] ","
        else
            print segments[i]
    }
    print "  ]"
    print "}"
}
' "$OUT_DIR/playlist.m3u8" > "$OUT_DIR/segment_info.json"

echo "Generated $OUT_DIR/segment_info.json with $(grep -c '"index"' "$OUT_DIR/segment_info.json") segments"

# playlist.m3u8은 이제 필요없으므로 삭제 (segment_info.json으로 대체)
rm -f "$OUT_DIR/playlist.m3u8"

echo "Segments written to $OUT_DIR"
