# OTT QUIC 스트리밍 디버그 로그

## 개요
AV1 코덱 + fMP4 세그먼트를 WebSocket으로 스트리밍하는 OTT 플레이어 개발 중 발생한 문제와 해결 과정을 기록합니다.

---

## 1. AV1 코덱 지원 문제

### 증상
- 비디오가 재생되지 않음
- MSE에서 코덱 지원 오류 발생

### 원인
- 기존 코덱 문자열이 실제 비디오 스펙과 불일치

### 해결
```javascript
// 수정 전
'video/mp4; codecs="av01.0.08M.08"'

// 수정 후 (실제 비디오 분석 결과 반영)
'video/mp4; codecs="av01.0.12M.08, mp4a.40.2"'
```
- `ffprobe`로 실제 비디오 분석: Main Profile, Level 12, 8-bit, yuv420p, 3840x2160

---

## 2. Duration 표시 문제

### 증상
- 비디오 duration이 `Infinity`로 표시됨

### 원인
- `mediaSource.duration`이 설정되지 않음

### 해결
- 서버에서 `total_duration` 전송
- 클라이언트에서 `mediaSource.duration = durationSec` 설정

---

## 3. Seek 시 잘못된 위치로 이동

### 증상
- 20초로 seek 시 24초로 이동
- seek 위치가 정확하지 않음

### 원인
- 세그먼트 duration이 **균일하지 않음** (7초, 4.3초, 0.8초 등 가변)
- 단순 계산 `Math.floor(time / segmentDuration)`으로는 정확한 세그먼트를 찾을 수 없음

### 해결
1. **segment_info.json 생성** (`tools/segment_video.sh`)
   ```bash
   # playlist.m3u8에서 각 세그먼트의 정확한 시작/종료 시간 추출
   # JSON 형식으로 저장
   ```
   
2. **서버에서 세그먼트 정보 전송** (`src/server/websocket.c`)
   ```c
   // ws_init 응답에 segments 배열 포함
   {"type":"ws_init","segments":[{"index":0,"start":0.0,"end":7.007,"duration":7.007},...]}
   ```
   
3. **클라이언트에서 정확한 세그먼트 찾기**
   ```javascript
   function findSegmentForTime(time) {
     for (let i = 0; i < segmentInfo.length; i++) {
       if (time >= seg.start && time < seg.end) {
         return seg.index;
       }
     }
   }
   ```

---

## 4. 일시정지 후 자동 재생 문제

### 증상
- 사용자가 일시정지했는데 자동으로 다시 재생됨

### 원인
- `video.onpause` 이벤트에서 버퍼 부족으로 인한 자동 pause와 사용자 pause를 구분하지 못함
- `updateend` 이벤트에서 무조건 `play()` 호출

### 해결
```javascript
let userPaused = false;

video.onpause = () => {
  // 버퍼가 충분할 때만 사용자 pause로 인식
  if (!seeking && sourceBuffer && sourceBuffer.buffered.length > 0) {
    const buffered = sourceBuffer.buffered;
    const currentTime = video.currentTime;
    for (let i = 0; i < buffered.length; i++) {
      if (currentTime >= buffered.start(i) && currentTime < buffered.end(i) - 0.5) {
        userPaused = true;
        return;
      }
    }
  }
};

video.onplaying = () => {
  userPaused = false;
};
```

---

## 5. WebSocket 재연결 후 CHUNK_DEMUXER_ERROR_APPEND_FAILED

### 증상
- 오래 일시정지 후 WebSocket 끊김
- 재연결 시 `CHUNK_DEMUXER_ERROR_APPEND_FAILED` 에러
- SourceBuffer error 발생

### 원인
1. WebSocket 재연결 시 INIT 세그먼트가 SourceBuffer 생성 전에 도착
2. INIT 세그먼트가 무시됨
3. 새 SourceBuffer 생성 후 일반 세그먼트가 INIT 없이 추가 → 디멀티플렉서 에러

### 해결
```javascript
let pendingInitSegment = null;

function handleBinary(data) {
  if (magic === 'INIT') {
    if (isSourceBufferValid()) {
      appendSegment(payload);
    } else {
      // SourceBuffer 준비 전이면 저장
      pendingInitSegment = payload;
    }
  }
}

// sourceopen 이벤트에서
mediaSource.addEventListener('sourceopen', () => {
  // ... SourceBuffer 생성 후
  if (pendingInitSegment) {
    appendSegment(pendingInitSegment);
    pendingInitSegment = null;
  }
});
```

---

## 6. 일시정지 상태에서 불필요한 세그먼트 요청

### 증상
- 버퍼가 충분한데도 계속 세그먼트 요청
- 일시정지 상태에서도 요청 발생

### 원인
- `requestSegment()`에서 버퍼 충분 시 1초 후 재귀 호출
- `userPaused` 상태를 확인하지 않음

### 해결
```javascript
if (bufferedAhead > CONFIG.BUFFER_AHEAD_MAX) {
  if (userPaused) {
    console.log('[WS] User paused with full buffer, stopping requests');
    requesting = false;
    return; // 추가 요청 안함
  }
  // ... 재생 중이면 1초 후 재시도
}
```

---

## 7. 일시정지 상태에서 WebSocket 자동 재연결

### 증상
- 사용자가 일시정지했는데 WebSocket 끊기면 자동 재연결

### 원인
- `socket.onclose`에서 `userPaused` 상태를 확인하지 않음

### 해결
```javascript
socket.onclose = (ev) => {
  if (userPaused) {
    console.log('[WS] User paused, not reconnecting');
    return;
  }
  // ... 재연결 로직
};
```

---

## 8. 메모리 누수

### 증상
- 장시간 재생 시 메모리 증가

### 원인
1. `setInterval`이 MSE 리셋 후에도 계속 실행
2. 미사용 변수/함수 존재

### 해결
```javascript
let cleanupInterval = null;

// sourceopen에서
if (cleanupInterval) clearInterval(cleanupInterval);
cleanupInterval = setInterval(cleanupBuffer, CONFIG.BUFFER_CLEANUP_INTERVAL);

// resetMediaSource에서
if (cleanupInterval) {
  clearInterval(cleanupInterval);
  cleanupInterval = null;
}
if (watchTimer) {
  clearInterval(watchTimer);
  watchTimer = null;
}
```

**제거된 미사용 코드:**
- 변수: `connectionId`, `streamId`, `resetting`
- 함수: `handleSourceError()`

---

## 최종 개선사항

### CONFIG 객체 도입
```javascript
const CONFIG = {
  BUFFER_AHEAD_MAX: 15,        // 최대 버퍼 길이 (초)
  BUFFER_CLEANUP_BEHIND: 10,   // 현재 위치 이전 버퍼 유지 (초)
  BUFFER_CLEANUP_INTERVAL: 2000, // 버퍼 정리 주기 (ms)
  RECONNECT_DELAY: 2000,       // 재연결 대기 시간 (ms)
  MAX_RECONNECT_ATTEMPTS: 5,   // 최대 재연결 시도 횟수
  SEGMENT_QUEUE_MAX: 5,        // 세그먼트 큐 최대 크기
  MAX_APPEND_RETRIES: 5,       // 버퍼 추가 재시도 횟수
  PROGRESS_SAVE_INTERVAL: 5000, // 진행률 저장 주기 (ms)
};
```

### 재연결 횟수 제한
- 무한 재연결 방지: 최대 5회까지만 시도

---

## 파일 변경 목록

| 파일 | 변경 내용 |
|------|----------|
| `tools/segment_video.sh` | segment_info.json 생성 로직 추가 |
| `src/server/websocket.c` | ws_init 응답에 segments 배열 포함 |
| `web/js/watch.js` | 전체 리팩토링 (CONFIG, 상태 관리, 에러 처리) |

---

## 교훈

1. **가변 길이 세그먼트 처리**: HLS/fMP4 세그먼트는 균일한 길이를 보장하지 않음
2. **MSE 상태 관리**: SourceBuffer 생성/소멸 타이밍 중요
3. **사용자 의도 구분**: 자동 pause vs 수동 pause 구분 필요
4. **리소스 정리**: interval, timer 등 정리 필수
5. **설정값 분리**: 하드코딩 대신 CONFIG 객체로 관리

---

*마지막 업데이트: 2025년 12월 4일*
