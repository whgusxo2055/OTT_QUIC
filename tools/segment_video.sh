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

# playlist.m3u8 파싱하여 segment_info.json 생성
python3 - "$OUT_DIR/playlist.m3u8" "$OUT_DIR/segment_info.json" "$TOTAL_DURATION" << 'PYTHON_SCRIPT'
import sys
import json
import re

playlist_path = sys.argv[1]
output_path = sys.argv[2]
total_duration = float(sys.argv[3])

segments = []
current_time = 0.0

with open(playlist_path, 'r') as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    line = line.strip()
    if line.startswith('#EXTINF:'):
        # #EXTINF:2.002000, 형식에서 duration 추출
        match = re.match(r'#EXTINF:([\d.]+)', line)
        if match:
            duration = float(match.group(1))
            # 다음 줄이 세그먼트 파일명
            if i + 1 < len(lines):
                segment_file = lines[i + 1].strip()
                if segment_file and not segment_file.startswith('#'):
                    # chunk-stream0-00000.m4s에서 인덱스 추출
                    idx_match = re.search(r'(\d+)\.m4s$', segment_file)
                    if idx_match:
                        idx = int(idx_match.group(1))
                        segments.append({
                            'index': idx,
                            'start': round(current_time, 3),
                            'end': round(current_time + duration, 3),
                            'duration': round(duration, 3)
                        })
                        current_time += duration

result = {
    'total_duration': round(total_duration, 3),
    'total_segments': len(segments),
    'segments': segments
}

with open(output_path, 'w') as f:
    json.dump(result, f, indent=2)

print(f"Generated {output_path} with {len(segments)} segments")
PYTHON_SCRIPT

# playlist.m3u8은 이제 필요없으므로 삭제 (segment_info.json으로 대체)
rm -f "$OUT_DIR/playlist.m3u8"

echo "Segments written to $OUT_DIR"
