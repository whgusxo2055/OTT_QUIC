const sessionId = checkLogin();
const urlParams = new URLSearchParams(window.location.search);
const videoId = parseInt(urlParams.get('id'), 10);
const startTimeParam = urlParams.get('t');  // URL에서 시작 시간 (초)

if (!videoId) {
  alert('잘못된 접근입니다.');
  window.location.href = 'index.html';
}

// ===== 설정 =====
const CONFIG = {
  BUFFER_AHEAD_MAX: 15,      // 최대 버퍼 길이 (초)
  BUFFER_CLEANUP_BEHIND: 10, // 현재 위치 이전 버퍼 유지 (초)
  BUFFER_CLEANUP_INTERVAL: 2000, // 버퍼 정리 주기 (ms)
  RECONNECT_DELAY: 2000,     // 재연결 대기 시간 (ms)
  MAX_RECONNECT_ATTEMPTS: 5, // 최대 재연결 시도 횟수
  SEGMENT_QUEUE_MAX: 5,      // 세그먼트 큐 최대 크기
  MAX_APPEND_RETRIES: 5,     // 버퍼 추가 재시도 횟수
  PROGRESS_SAVE_INTERVAL: 5000, // 진행률 저장 주기 (ms)
};

// ===== 상태 변수 =====
let socket = null;
let mediaSource = null;
let sourceBuffer = null;
let bufferValid = false;
let segmentQueue = [];
let appending = false;
let nextSegment = 0;
let maxSegments = 100;
let watchTimer = null;
let objectUrl = null;
let cleanupInterval = null;
let requesting = false;
let seeking = false;
let seekTargetTime = 0;
let segmentDuration = 2;
let segmentInfo = [];
let userPaused = false;
let pendingInitSegment = null;
let reconnectAttempts = 0; // 재연결 시도 횟수
let videoCodecString = null; // 서버에서 받은 코덱 정보 (null = 아직 모름)
let mediaSourceReady = false; // MediaSource가 열렸는지 여부

// 코덱 이름을 MIME 코덱 문자열로 변환
function getCodecMimeString(videoCodec) {
  const codecMap = {
    'hevc': 'hvc1.1.6.L120.90',
    'h265': 'hvc1.1.6.L120.90',
    'h264': 'avc1.64001f',
    'avc': 'avc1.64001f',
    'avc1': 'avc1.64001f',
    'av1': 'av01.0.12M.08',
    'av01': 'av01.0.12M.08'
  };
  return codecMap[videoCodec?.toLowerCase()] || 'avc1.64001f';
}

// SourceBuffer 생성 함수
function createSourceBuffer() {
  if (!mediaSource || mediaSource.readyState !== 'open') {
    console.log('[MSE] MediaSource not ready, cannot create SourceBuffer');
    return false;
  }
  if (sourceBuffer) {
    console.log('[MSE] SourceBuffer already exists');
    return true;
  }
  
  // 코덱 문자열 결정 (서버에서 받은 것 또는 기본값)
  const codecStr = videoCodecString || 'avc1.64001f';
  const mimeType = `video/mp4; codecs="${codecStr}, mp4a.40.2"`;
  console.log('[MSE] Creating SourceBuffer with codec:', mimeType);
  
  try {
    if (!MediaSource.isTypeSupported(mimeType)) {
      console.error('[MSE] Codec not supported:', mimeType);
      // 대안 코덱들 시도
      const fallbackCodecs = [
        'video/mp4; codecs="hvc1.1.6.L120.90, mp4a.40.2"', // HEVC
        'video/mp4; codecs="avc1.64001f, mp4a.40.2"',     // H.264 High
        'video/mp4; codecs="avc1.42E01E, mp4a.40.2"',     // H.264 Baseline
        'video/mp4; codecs="av01.0.08M.08, mp4a.40.2"'    // AV1
      ];
      
      for (const altMime of fallbackCodecs) {
        if (MediaSource.isTypeSupported(altMime)) {
          console.log('[MSE] Using fallback codec:', altMime);
          sourceBuffer = mediaSource.addSourceBuffer(altMime);
          setupSourceBufferEvents();
          bufferValid = true;
          return true;
        }
      }
      
      console.error('[MSE] No supported codec found');
      alert('지원되는 비디오 코덱이 없습니다.');
      return false;
    }
    
    sourceBuffer = mediaSource.addSourceBuffer(mimeType);
    setupSourceBufferEvents();
    bufferValid = true;
    console.log('[MSE] SourceBuffer created successfully');
    return true;
  } catch (e) {
    console.error('[MSE] Failed to create SourceBuffer:', e);
    return false;
  }
}

// SourceBuffer 이벤트 설정
function setupSourceBufferEvents() {
  sourceBuffer.addEventListener('updateend', () => {
    appending = false;
    
    // 버퍼가 추가되면 비디오가 멈춰있는 경우 재생 시도 (사용자가 일시정지한게 아닌 경우만)
    const video = document.getElementById('player');
    if (video && video.paused && !userPaused && sourceBuffer.buffered.length > 0) {
      const currentTime = video.currentTime;
      const buffered = sourceBuffer.buffered;
      
      // seek 중이었고 목표 시간이 버퍼 내에 있으면 목표 시간으로 이동
      if (seekTargetTime > 0 && Math.abs(currentTime - seekTargetTime) > 0.5) {
        for (let i = 0; i < buffered.length; i++) {
          if (seekTargetTime >= buffered.start(i) && seekTargetTime <= buffered.end(i)) {
            console.log('[MSE] Seek target', seekTargetTime.toFixed(2), 'now in buffer, jumping...');
            const target = seekTargetTime;
            seekTargetTime = 0;
            video.currentTime = target;
            if (!userPaused) {
              video.play().catch(e => console.log('[MSE] Play failed:', e));
            }
            return;
          }
        }
        console.log('[MSE] Seek target', seekTargetTime.toFixed(2), 'not yet in buffer, waiting...');
        return;
      }
      
      if (seekTargetTime > 0) {
        seekTargetTime = 0;
      }
      
      for (let i = 0; i < buffered.length; i++) {
        if (currentTime >= buffered.start(i) - 0.1 && currentTime <= buffered.end(i)) {
          console.log('[MSE] Buffer ready at current position, starting playback...');
          video.play().catch(e => console.log('[MSE] Play failed:', e));
          break;
        }
      }
    }
    
    // 큐에 있는 다음 세그먼트 먼저 처리
    if (segmentQueue.length > 0) {
      appendNext();
    } else if (nextSegment < maxSegments && !requesting) {
      requestSegment();
    }
  });
  
  sourceBuffer.addEventListener('error', (e) => {
    console.error('[MSE] SourceBuffer error:', e);
  });
}

// SourceBuffer 상태 확인 헬퍼
function isSourceBufferValid() {
  return sourceBuffer && 
         bufferValid && 
         mediaSource && 
         mediaSource.readyState === 'open';
}

// 주어진 시간에 해당하는 세그먼트 인덱스 찾기
function findSegmentForTime(time) {
  // segmentInfo 배열이 있으면 정확한 세그먼트 찾기
  if (segmentInfo && segmentInfo.length > 0) {
    for (let i = 0; i < segmentInfo.length; i++) {
      const seg = segmentInfo[i];
      if (time >= seg.start && time < seg.end) {
        console.log('[Seek] Found segment', seg.index, 'for time', time.toFixed(2), '(', seg.start.toFixed(2), '-', seg.end.toFixed(2), ')');
        return seg.index;
      }
    }
    // 마지막 세그먼트 이후면 마지막 세그먼트 반환
    if (time >= segmentInfo[segmentInfo.length - 1].end) {
      return segmentInfo.length - 1;
    }
    // 첫 세그먼트 이전이면 0 반환
    return 0;
  }
  
  // fallback: 평균 duration으로 계산
  const idx = Math.floor(time / segmentDuration);
  console.log('[Seek] Fallback segment calc:', idx, 'for time', time.toFixed(2), '(segDur:', segmentDuration.toFixed(3), ')');
  return Math.max(0, Math.min(idx, maxSegments - 1));
}

document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('back-btn').addEventListener('click', goBack);
  connectWebSocket();
});

function goBack() {
  saveProgress();
  window.location.href = 'index.html';
}

window.addEventListener('beforeunload', () => {
  saveProgress();
});

function connectWebSocket() {
  console.log('[WS] Connecting to:', wsUrl);
  socket = new WebSocket(wsUrl);
  socket.binaryType = 'arraybuffer';
  
  socket.onopen = () => {
    console.log('[WS] WebSocket connected');
    reconnectAttempts = 0; // 연결 성공 시 재연결 횟수 리셋
    // Get last position first
    console.log('[WS] Sending watch_get for video:', videoId);
    send({type: 'watch_get', video_id: videoId});
  };

  socket.onmessage = (ev) => {
    console.log('[WS] Message received, type:', typeof ev.data, 'size:', ev.data.length || ev.data.byteLength);
    if (typeof ev.data === 'string') {
      try {
        const data = JSON.parse(ev.data);
        handleMessage(data);
      } catch (e) {
        console.error('[WS] JSON Parse Error', e);
      }
    } else {
      handleBinary(ev.data);
    }
  };

  socket.onerror = (err) => {
    console.error('[WS] WebSocket error:', err);
  };

  socket.onclose = (ev) => {
    console.log('[WS] WebSocket closed, code:', ev.code, 'reason:', ev.reason);
    
    // 사용자가 일시정지한 경우에는 재연결하지 않음
    if (userPaused) {
      console.log('[WS] User paused, not reconnecting');
      return;
    }
    
    // 재연결 횟수 제한
    if (reconnectAttempts >= CONFIG.MAX_RECONNECT_ATTEMPTS) {
      console.error('[WS] Max reconnection attempts reached');
      return;
    }
    
    // 재연결 시도 (더 받을 세그먼트가 있을 때만)
    if (nextSegment < maxSegments) {
      reconnectAttempts++;
      console.log('[WS] Reconnecting in', CONFIG.RECONNECT_DELAY, 'ms... (attempt', reconnectAttempts + '/' + CONFIG.MAX_RECONNECT_ATTEMPTS + ')');
      setTimeout(() => {
        // 재연결 전 다시 확인 - 사용자가 일시정지 상태면 재연결 안함
        if (userPaused) {
          console.log('[WS] User still paused, cancelling reconnection');
          return;
        }
        
        // 재연결 전 MSE 상태 확인 및 리셋
        const video = document.getElementById('player');
        const currentTime = video ? video.currentTime : 0;
        
        // MSE가 ended 상태면 완전히 리셋
        if (mediaSource && mediaSource.readyState !== 'open') {
          console.log('[WS] MediaSource state:', mediaSource.readyState, '- resetting MSE');
          resetMediaSource();
          setupMediaSource();
        }
        
        connectWebSocket();
        // 재연결 후 현재 위치부터 다시 요청
        setTimeout(() => {
          if (socket && socket.readyState === WebSocket.OPEN) {
            // 현재 재생 위치에 해당하는 세그먼트부터 요청
            if (currentTime > 0) {
              nextSegment = findSegmentForTime(currentTime);
              console.log('[WS] Resuming from segment:', nextSegment, 'for time:', currentTime.toFixed(2));
            }
            seeking = true;
            pendingInitSegment = null;
            send({ type: 'ws_init', video_id: videoId });
          }
        }, 500);
      }, CONFIG.RECONNECT_DELAY);
    }
  };
}

function send(obj) {
  // 모든 요청에 세션 ID 포함
  obj.session_id = sessionId;
  console.log('[WS] Sending:', obj);
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(obj));
  } else {
    console.warn('[WS] Socket not ready, state:', socket?.readyState);
  }
}

function handleMessage(msg) {
  console.log('[WS] Message:', msg);
  switch (msg.type) {
    case 'watch_get':
      if (msg.status === 'ok') {
        // URL 파라미터가 있으면 우선 사용, 아니면 서버에서 받은 위치 사용
        let startPos = msg.position || 0;
        if (startTimeParam !== null) {
          const paramTime = parseInt(startTimeParam, 10);
          if (!isNaN(paramTime) && paramTime >= 0) {
            startPos = paramTime;
            console.log('[Watch] Using URL parameter start time:', startPos);
          }
        }
        startStream(startPos);
      } else {
        // URL 파라미터 있으면 사용
        let startPos = 0;
        if (startTimeParam !== null) {
          const paramTime = parseInt(startTimeParam, 10);
          if (!isNaN(paramTime) && paramTime >= 0) {
            startPos = paramTime;
          }
        }
        startStream(startPos);
      }
      break;
    case 'ws_init':
      if (msg.status === 'ok') {
        console.log('[WS] Init segment sent by server, duration:', msg.total_duration || msg.duration, 'segments:', msg.total_segments);
        
        // 코덱 정보 저장 (video_codec 필드에서 MIME 코덱 문자열 생성)
        if (msg.video_codec) {
          videoCodecString = getCodecMimeString(msg.video_codec);
          console.log('[WS] Video codec:', msg.video_codec, '-> MIME:', videoCodecString);
        }
        
        // MediaSource가 준비되었으면 SourceBuffer 생성
        if (mediaSourceReady && !sourceBuffer) {
          if (createSourceBuffer()) {
            // 저장된 INIT 세그먼트가 있으면 추가
            if (pendingInitSegment) {
              console.log('[MSE] Appending pending INIT segment after codec info received');
              appendSegment(pendingInitSegment);
              pendingInitSegment = null;
              setTimeout(() => {
                if (isSourceBufferValid()) {
                  requestSegment();
                }
              }, 100);
            }
          }
        }
        
        // Set total segments from server
        if (msg.total_segments) {
          maxSegments = msg.total_segments;
          console.log('[WS] Max segments set to:', maxSegments);
        }
        
        // 세그먼트 정보 저장 (서버에서 segments 배열이 제공된 경우)
        if (msg.segments && Array.isArray(msg.segments)) {
          segmentInfo = msg.segments;
          console.log('[WS] Segment info received:', segmentInfo.length, 'segments');
          // 첫 번째 세그먼트의 duration을 기본값으로 사용
          if (segmentInfo.length > 0 && segmentInfo[0].duration) {
            segmentDuration = segmentInfo[0].duration;
          }
        } else {
          // fallback: 세그먼트 길이 계산 (duration / total_segments)
          const duration = msg.total_duration || msg.duration;
          if (duration && msg.total_segments) {
            segmentDuration = duration / msg.total_segments;
            console.log('[WS] Segment duration calculated:', segmentDuration.toFixed(3), 'seconds');
          }
        }
        
        // Set duration on mediaSource when ready
        const durationSec = msg.total_duration || msg.duration;
        if (durationSec && durationSec > 0) {
          const waitForBuffer = () => {
            if (mediaSource && mediaSource.readyState === 'open' && sourceBuffer && !sourceBuffer.updating) {
              try {
                mediaSource.duration = durationSec;
                console.log('[MSE] Duration set to:', durationSec);
              } catch (e) {
                console.warn('[MSE] Could not set duration:', e);
              }
            } else {
              setTimeout(waitForBuffer, 100);
            }
          };
          setTimeout(waitForBuffer, 500);
        }
        
        // Start saving progress periodically
        if (watchTimer) clearInterval(watchTimer);
        watchTimer = setInterval(saveProgress, CONFIG.PROGRESS_SAVE_INTERVAL);
      } else {
        console.error('[WS] Init failed:', msg.message);
      }
      break;
    case 'ws_segment':
      if (msg.status === 'error') {
        // segment-missing means end of video, not an error
        if (msg.message === 'segment-missing') {
          console.log('[WS] End of video reached at segment:', msg.segment);
        } else {
          console.error('[WS] Segment Error:', msg);
        }
        // End of stream - no more segments
        if (mediaSource && mediaSource.readyState === 'open') {
          // Wait for buffer to finish before ending stream
          const checkAndEnd = () => {
            if (sourceBuffer && !sourceBuffer.updating) {
              try {
                mediaSource.endOfStream();
                console.log('[MSE] Stream ended successfully');
              } catch (e) {
                console.warn('endOfStream failed', e);
              }
            } else {
              setTimeout(checkAndEnd, 100);
            }
          };
          checkAndEnd();
        }
      }
      break;
  }
}

function startStream(position) {
  resetMediaSource();
  setupMediaSource();
  
  // position이 0보다 크면 해당 위치부터 시작
  if (position > 0) {
    seekTargetTime = position;
    seeking = true;
    const targetSegment = findSegmentForTime(position);
    nextSegment = targetSegment;
    console.log('[Stream] Starting with seek target:', seekTargetTime, 'from segment:', targetSegment);
  } else {
    nextSegment = 0;
    seekTargetTime = 0;
    seeking = false;
  }
  
  // INIT 세그먼트 요청
  send({type: 'ws_init', video_id: videoId});
}

function setupMediaSource() {
  const video = document.getElementById('player');
  mediaSource = new MediaSource();
  objectUrl = URL.createObjectURL(mediaSource);
  video.src = objectUrl;

  // Video element error handler
  video.onerror = (e) => {
    console.error('[Video] Error:', video.error);
  };

  video.onloadedmetadata = () => {
    console.log('[Video] Metadata loaded, duration:', video.duration);
  };

  video.oncanplay = () => {
    console.log('[Video] Can play, userPaused:', userPaused);
    // 사용자가 일시정지하지 않은 경우에만 자동재생 시도
    if (!userPaused) {
      video.play().then(() => {
        console.log('[Video] Autoplay started');
      }).catch(e => {
        console.log('[Video] Autoplay blocked, trying muted:', e.message);
        // 자동재생 차단 시 음소거로 재시도
        video.muted = true;
        video.play().then(() => {
          console.log('[Video] Autoplay started (muted fallback)');
          // 사용자가 클릭하면 음소거 해제
          document.addEventListener('click', () => {
            video.muted = false;
            console.log('[Video] Unmuted after user interaction');
          }, { once: true });
        }).catch(e2 => console.log('[Video] Autoplay failed:', e2.message));
      });
    } else {
      console.log('[Video] Autoplay skipped (user paused)');
    }
  };

  // 재생 멈춤 감지
  video.onwaiting = () => {
    // 버퍼 상태 상세 로그
    if (sourceBuffer && sourceBuffer.buffered.length > 0) {
      const currentTime = video.currentTime;
      const buffered = sourceBuffer.buffered;
      let bufferRanges = [];
      for (let i = 0; i < buffered.length; i++) {
        bufferRanges.push(`[${buffered.start(i).toFixed(2)}-${buffered.end(i).toFixed(2)}]`);
      }
      console.log('[Video] Waiting at', currentTime.toFixed(2), 'buffers:', bufferRanges.join(', '));
      
      // 현재 위치가 버퍼 안에 있는지 확인
      let inBuffer = false;
      let nextBufferStart = null;
      for (let i = 0; i < buffered.length; i++) {
        if (currentTime >= buffered.start(i) - 0.5 && currentTime <= buffered.end(i)) {
          inBuffer = true;
          break;
        }
        // 현재 위치보다 뒤에 있는 버퍼 찾기
        if (buffered.start(i) > currentTime && nextBufferStart === null) {
          nextBufferStart = buffered.start(i);
        }
      }
      
      if (!inBuffer && nextBufferStart !== null) {
        console.log('[Video] Gap detected, jumping to', nextBufferStart.toFixed(2));
        video.currentTime = nextBufferStart;
      } else if (inBuffer && !userPaused) {
        // 버퍼 안에 있는데 멈춤 - play() 재시도 (사용자가 일시정지한게 아닌 경우만)
        console.log('[Video] In buffer but waiting, trying play()');
        video.play().catch(e => console.log('[Video] Play retry failed:', e));
      }
    } else {
      console.log('[Video] Waiting for data... no buffer');
    }
    
    // 더 많은 세그먼트 요청
    if (!requesting && nextSegment < maxSegments) {
      requestSegment();
    }
  };

  video.onstalled = () => {
    console.log('[Video] Stalled - network issue');
  };

  video.onplaying = () => {
    console.log('[Video] Playing');
    userPaused = false; // 재생 시작하면 플래그 해제
  };

  video.onpause = () => {
    console.log('[Video] Paused');
    // seeking 중이 아니고 버퍼가 충분하면 사용자가 일시정지한 것으로 간주
    // (버퍼 부족으로 인한 자동 pause 구분)
    if (!seeking && sourceBuffer && sourceBuffer.buffered.length > 0) {
      const buffered = sourceBuffer.buffered;
      const currentTime = video.currentTime;
      for (let i = 0; i < buffered.length; i++) {
        if (currentTime >= buffered.start(i) && currentTime < buffered.end(i) - 0.5) {
          userPaused = true;
          console.log('[Video] User paused (buffer available)');
          return;
        }
      }
    }
    console.log('[Video] Paused (buffer issue or seeking)');
  };

  // Seek 이벤트 - 사용자가 타임라인 클릭
  video.onseeking = () => {
    const seekTime = video.currentTime;
    console.log('[Video] Seeking to', seekTime.toFixed(2));
    
    // Seek 시 userPaused 해제 (사용자가 seek하면 재생 의도로 간주)
    userPaused = false;
    
    // seek 목표 시간 저장
    seekTargetTime = seekTime;
    
    // 해당 위치가 버퍼에 있는지 확인하고, readyState도 체크
    if (sourceBuffer && sourceBuffer.buffered.length > 0) {
      const buffered = sourceBuffer.buffered;
      let bufferRanges = [];
      let inBuffer = false;
      for (let i = 0; i < buffered.length; i++) {
        bufferRanges.push(`[${buffered.start(i).toFixed(2)}-${buffered.end(i).toFixed(2)}]`);
        if (seekTime >= buffered.start(i) && seekTime <= buffered.end(i)) {
          inBuffer = true;
        }
      }
      
      // 버퍼에 있고 readyState가 충분하면 그냥 재생
      if (inBuffer && video.readyState >= 3) {
        console.log('[Video] Seek target in buffer and ready, ranges:', bufferRanges.join(', '));
        seekTargetTime = 0; // 리셋
        return;
      }
      
      // 버퍼에 있지만 readyState가 낮으면 로그만 출력 (짧은 대기 후 재확인)
      if (inBuffer) {
        console.log('[Video] Seek target in buffer but readyState:', video.readyState, '- waiting...');
        // 잠시 후 재확인
        setTimeout(() => {
          if (video.readyState >= 3 && !userPaused) {
            console.log('[Video] Now ready, playing...');
            video.play().catch(e => console.log('[Video] Play failed:', e));
          } else if (userPaused) {
            console.log('[Video] Ready but user paused, not playing');
          } else {
            console.log('[Video] Still not ready, reloading from segment...');
            // 버퍼 비우고 다시 로드
            reloadFromSegment(seekTime);
          }
        }, 500);
        return;
      }
      
      console.log('[Video] Buffer ranges:', bufferRanges.join(', '));
    } else {
      console.log('[Video] No buffer available');
    }
    
    // 버퍼에 없으면 해당 세그먼트부터 로드
    reloadFromSegment(seekTime);
  };
  
  // 세그먼트부터 다시 로드하는 헬퍼 함수
  function reloadFromSegment(seekTime) {
    const targetSegment = findSegmentForTime(seekTime);
    console.log('[Video] Loading from segment:', targetSegment, 'for time:', seekTime.toFixed(2));
    
    seeking = true;
    segmentQueue = []; // 기존 큐 비우기
    nextSegment = targetSegment;
    requesting = false;
    
    // SourceBuffer 비우기
    if (sourceBuffer && !sourceBuffer.updating && sourceBuffer.buffered.length > 0) {
      try {
        const start = sourceBuffer.buffered.start(0);
        const end = sourceBuffer.buffered.end(sourceBuffer.buffered.length - 1);
        console.log('[MSE] Clearing buffer for seek:', start.toFixed(2), '-', end.toFixed(2));
        sourceBuffer.remove(start, end);
        sourceBuffer.addEventListener('updateend', function onClear() {
          sourceBuffer.removeEventListener('updateend', onClear);
          console.log('[MSE] Buffer cleared, requesting init + segment');
          send({ type: 'ws_init', video_id: videoId });
        }, { once: true });
        return;
      } catch (e) {
        console.warn('[MSE] Buffer clear failed:', e);
      }
    }
    
    // 버퍼가 없거나 비우기 실패 시 init부터 다시 요청
    send({ type: 'ws_init', video_id: videoId });
  }

  video.onseeked = () => {
    const currentTime = video.currentTime;
    console.log('[Video] Seeked to', currentTime.toFixed(2), 'target was:', seekTargetTime.toFixed(2), 'readyState:', video.readyState, 'paused:', video.paused);
    
    // 버퍼 상태 확인
    if (sourceBuffer && sourceBuffer.buffered && sourceBuffer.buffered.length > 0) {
      const buffered = sourceBuffer.buffered;
      let bufferRanges = [];
      for (let i = 0; i < buffered.length; i++) {
        bufferRanges.push(`[${buffered.start(i).toFixed(2)}-${buffered.end(i).toFixed(2)}]`);
      }
      console.log('[Video] Post-seek buffer:', bufferRanges.join(', '));
      
      // 목표 시간이 설정되어 있고, 현재 위치와 다른 경우에만 이동
      if (seekTargetTime > 0 && Math.abs(currentTime - seekTargetTime) > 0.5) {
        for (let i = 0; i < buffered.length; i++) {
          if (seekTargetTime >= buffered.start(i) && seekTargetTime <= buffered.end(i)) {
            console.log('[Video] Seek target in buffer, jumping to', seekTargetTime.toFixed(2));
            const target = seekTargetTime;
            seekTargetTime = 0; // 먼저 리셋하여 재귀 방지
            video.currentTime = target;
            if (!userPaused) {
              video.play().catch(e => console.log('[Video] Play after seek failed:', e));
            }
            return;
          }
        }
        // 목표 시간이 버퍼에 없으면 더 기다림 (세그먼트 로딩 중)
        console.log('[Video] Seek target not in buffer yet, waiting for segments...');
        return;
      }
      
      // seekTargetTime이 현재 위치와 같거나 거의 같으면 리셋하고 재생
      if (seekTargetTime > 0) {
        seekTargetTime = 0;
      }
      
      // 현재 위치에 버퍼가 있으면 바로 재생 (사용자가 일시정지하지 않은 경우만)
      if (!userPaused) {
        for (let i = 0; i < buffered.length; i++) {
          if (currentTime >= buffered.start(i) && currentTime <= buffered.end(i)) {
            console.log('[Video] Seek position in buffer, playing...');
            video.play().catch(e => console.log('[Video] Play after seek failed:', e));
            return;
          }
        }
      }
    } else {
      console.log('[Video] No buffer after seek, waiting for data...');
    }
    
    // 버퍼가 없으면 canplay 이벤트에서 재생 시도
  };

  mediaSource.addEventListener('sourceopen', () => {
    console.log('[MSE] Source opened');
    mediaSourceReady = true;
    
    // 코덱 정보가 이미 있으면 바로 SourceBuffer 생성
    if (videoCodecString) {
      if (createSourceBuffer()) {
        // 저장된 INIT 세그먼트가 있으면 추가
        if (pendingInitSegment) {
          console.log('[MSE] Appending pending INIT segment, size:', pendingInitSegment.length);
          appendSegment(pendingInitSegment);
          pendingInitSegment = null;
          setTimeout(() => {
            if (isSourceBufferValid()) {
              console.log('[MSE] Requesting segments after pending INIT');
              requestSegment();
            }
          }, 100);
        }
      }
    } else {
      console.log('[MSE] Waiting for codec info from server before creating SourceBuffer...');
    }
  });
  
  // 주기적으로 버퍼 정리
  if (cleanupInterval) {
    clearInterval(cleanupInterval);
  }
  cleanupInterval = setInterval(() => {
    if (sourceBuffer && !sourceBuffer.updating && !cleaningBuffer) {
      cleanupBuffer();
    }
  }, CONFIG.BUFFER_CLEANUP_INTERVAL);

  mediaSource.addEventListener('sourceended', () => {
    console.log('[MSE] Source ended');
  });

  mediaSource.addEventListener('error', (e) => {
    console.error('[MSE] MediaSource error:', e);
  });
  
  segmentQueue = [];
  appending = false;
}

function handleBinary(data) {
  const arr = new Uint8Array(data);
  if (arr.length < 8) return;
  
  const magic = String.fromCharCode(arr[0], arr[1], arr[2], arr[3]);
  const idx = (arr[4] << 24) | (arr[5] << 16) | (arr[6] << 8) | arr[7];
  const payload = arr.slice(8);
  
  console.log('[WS] Binary received:', magic, 'idx:', idx, 'size:', payload.length);
  
  if (magic === 'INIT') {
    // Init segment - SourceBuffer가 준비되었는지 확인
    if (isSourceBufferValid()) {
      // SourceBuffer가 준비됨 - 바로 추가
      appendSegment(payload);
      pendingInitSegment = null;
    } else {
      // SourceBuffer가 아직 준비 안됨 - 저장해두고 sourceopen에서 처리
      console.log('[WS] SourceBuffer not ready, saving INIT segment for later');
      pendingInitSegment = payload;
    }
    
    requesting = false; // 요청 플래그 리셋
    
    // nextSegment가 설정되지 않았으면 0으로 초기화
    if (nextSegment === undefined || nextSegment === null) {
      nextSegment = 0;
    }
    console.log('[WS] INIT received, next segment:', nextSegment, 'seekTargetTime:', seekTargetTime);
    
    // seeking 플래그는 유지 (seekTargetTime이 있을 때)
    const hasSeekTarget = seekTargetTime > 0;
    
    // SourceBuffer가 유효할 때만 다음 세그먼트 요청
    if (isSourceBufferValid()) {
      requestSegment();
      
      // seek 목표가 있고 사용자가 일시정지하지 않았으면 재생 시도
      if (hasSeekTarget && !userPaused) {
        const video = document.getElementById('player');
        if (video) {
          video.play().catch(e => console.log('[Video] Play after seek failed:', e));
        }
      }
    } else {
      console.log('[WS] Waiting for SourceBuffer to be ready before requesting segments');
    }
  } else if (magic === 'SEGM') {
    // 세그먼트 큐에 추가 - 다음 요청은 appendNext 성공 후에
    nextSegment = idx + 1;
    requesting = false; // 세그먼트 수신 완료, 다음 요청 가능
    appendSegment(payload);
  }
}

function appendSegment(buffer) {
  if (!sourceBuffer || !bufferValid) return;
  
  // 큐가 너무 크면 세그먼트 버림
  if (segmentQueue.length >= CONFIG.SEGMENT_QUEUE_MAX) {
    console.warn('[MSE] Queue too large, dropping oldest segment');
    segmentQueue.shift();
  }
  
  segmentQueue.push(buffer);
  appendNext();
}

// 버퍼 정리 진행 중 플래그
let cleaningBuffer = false;

// 오래된 버퍼 삭제 (현재 재생 위치 기준) - 재귀 없이
function cleanupBuffer() {
  if (cleaningBuffer) return;
  if (!isSourceBufferValid()) return;
  
  const video = document.getElementById('player');
  if (!video || sourceBuffer.updating) {
    return;
  }
  
  try {
    const currentTime = video.currentTime;
    const buffered = sourceBuffer.buffered;
  
    if (buffered.length > 0) {
      const start = buffered.start(0);
      // 현재 위치에서 CONFIG.BUFFER_CLEANUP_BEHIND 초 전까지만 유지
      const removeEnd = Math.max(start, currentTime - CONFIG.BUFFER_CLEANUP_BEHIND);
      
      if (removeEnd > start + 1) {
        try {
          cleaningBuffer = true;
          console.log('[MSE] Removing buffer from', start.toFixed(2), 'to', removeEnd.toFixed(2));
          sourceBuffer.remove(start, removeEnd);
          sourceBuffer.addEventListener('updateend', function onEnd() {
            sourceBuffer.removeEventListener('updateend', onEnd);
            cleaningBuffer = false;
            appendNext(); // 버퍼 정리 후 다음 세그먼트 추가 시도
          }, { once: true });
        } catch (e) {
          cleaningBuffer = false;
          console.warn('[MSE] Buffer remove failed:', e);
        }
      }
    }
  } catch (e) {
    console.warn('[MSE] cleanupBuffer error:', e);
  }
}

let retryCount = 0;

function appendNext() {
  if (!sourceBuffer || !bufferValid || !mediaSource || mediaSource.readyState !== 'open' || sourceBuffer.updating || appending || segmentQueue.length === 0) {
    return;
  }

  // 버퍼 정리는 주기적으로 별도로 수행 (재귀 호출 방지)
  appending = true;
  const seg = segmentQueue.shift();
  console.log('[MSE] Appending segment, size:', (seg.length / 1024 / 1024).toFixed(2) + 'MB', 'queue:', segmentQueue.length);
  
  try {
    sourceBuffer.appendBuffer(seg);
    retryCount = 0; // 성공하면 리셋
  } catch (e) {
    console.error('[MSE] SourceBuffer Append Error:', e.name);
    appending = false;
    
    // QuotaExceededError 처리
    if (e.name === 'QuotaExceededError') {
      if (retryCount < CONFIG.MAX_APPEND_RETRIES) {
        retryCount++;
        segmentQueue.unshift(seg); // 세그먼트 다시 큐에 넣기
        console.log('[MSE] Quota exceeded, cleaning buffer (retry', retryCount + '/' + CONFIG.MAX_APPEND_RETRIES + ')');
        cleanupBuffer();
        // 버퍼 정리 후 재시도
        setTimeout(appendNext, 500);
      } else {
        console.error('[MSE] Max retries reached, skipping segment');
        retryCount = 0;
        // 다음 세그먼트 시도
        setTimeout(appendNext, 500);
      }
    }
  }
}

function requestSegment() {
  // 이미 요청 중이면 무시
  if (requesting) {
    return;
  }
  
  // SourceBuffer 상태 확인
  if (!isSourceBufferValid()) {
    console.warn('[WS] SourceBuffer not valid, skipping request');
    return;
  }
  
  // 모든 세그먼트를 다 받았으면 종료
  if (nextSegment >= maxSegments) {
    console.log('[WS] All segments received, ending stream');
    if (mediaSource && mediaSource.readyState === 'open' && !sourceBuffer.updating) {
      try {
        mediaSource.endOfStream();
      } catch (e) {
        console.warn('endOfStream failed', e);
      }
    }
    return;
  }
  
  // 버퍼가 이미 충분하면 요청 지연
  const video = document.getElementById('player');
  try {
    if (video && sourceBuffer && sourceBuffer.buffered && sourceBuffer.buffered.length > 0) {
      const buffered = sourceBuffer.buffered;
      const bufferEnd = buffered.end(buffered.length - 1);
      const bufferStart = buffered.start(0);
      const currentTime = video.currentTime;
      const bufferedAhead = bufferEnd - currentTime;
      
      // 현재 시간이 버퍼 범위 밖에 있으면 버퍼 시작점으로 이동
      // (세그먼트 duration이 균일하지 않아 seek 시 정확한 위치를 찾지 못할 수 있음)
      if (currentTime < bufferStart - 0.5) {
        console.log('[WS] Current time', currentTime.toFixed(2), 'before buffer start', bufferStart.toFixed(2), ', jumping to buffer start');
        seekTargetTime = 0; // seek 목표 리셋
        video.currentTime = bufferStart;
        return;
      }
    
      // 앞으로 CONFIG.BUFFER_AHEAD_MAX 이상 버퍼되어 있으면 대기
      if (bufferedAhead > CONFIG.BUFFER_AHEAD_MAX) {
        console.log('[WS] Buffer full, waiting...', bufferedAhead.toFixed(1) + 's ahead, current:', currentTime.toFixed(2), 'buffer:', bufferStart.toFixed(2) + '-' + bufferEnd.toFixed(2));
        
        // 비디오가 멈춰있고 사용자가 일시정지한게 아니면 재생 시도
        if ((video.paused || video.readyState < 3) && !userPaused) {
          console.log('[WS] Video not playing, readyState:', video.readyState, 'paused:', video.paused);
          video.play().catch(e => console.log('[Video] Play failed:', e));
        }
        
        // 사용자가 일시정지한 상태면 추가 요청 안함
        if (userPaused) {
          console.log('[WS] User paused with full buffer, stopping requests');
          requesting = false;
          return;
        }
        
        requesting = true; // 대기 중에도 플래그 설정
        setTimeout(() => {
          requesting = false;
          requestSegment();
        }, 1000);
        return;
      }
    
      // 비디오가 paused 상태이고 버퍼가 있고 사용자가 일시정지한게 아니면 재생 시도
      if (video.paused && bufferedAhead > 2 && !userPaused) {
        console.log('[WS] Video paused with buffer, trying to play...');
        video.play().catch(e => console.log('[Video] Play failed:', e));
      }
    }
  } catch (e) {
    console.warn('[WS] Buffer check error:', e);
  }
  
  requesting = true; // 요청 시작 플래그 설정
  console.log('[WS] Requesting segment:', nextSegment);
  send({
    type: 'ws_segment',
    video_id: videoId,
    segment: nextSegment
  });
}

function saveProgress() {
  const video = document.getElementById('player');
  if (video && !video.paused) {
    const pos = Math.floor(video.currentTime); // 초 단위로 저장
    send({
      type: 'watch_update',
      video_id: videoId,
      position: pos
    });
  }
}

function resetMediaSource() {
  const video = document.getElementById('player');
  segmentQueue = [];
  appending = false;
  nextSegment = 0;
  pendingInitSegment = null;
  requesting = false;
  seeking = false;
  seekTargetTime = 0;
  retryCount = 0;
  
  // interval 정리
  if (cleanupInterval) {
    clearInterval(cleanupInterval);
    cleanupInterval = null;
  }
  if (watchTimer) {
    clearInterval(watchTimer);
    watchTimer = null;
  }
  if (sourceBuffer) {
    try {
      sourceBuffer.onupdateend = null;
    } catch (e) {
      /* ignore */
    }
  }
  if (mediaSource && mediaSource.readyState === 'open' && sourceBuffer) {
    try {
      mediaSource.removeSourceBuffer(sourceBuffer);
    } catch (e) {
      console.warn('removeSourceBuffer failed', e);
    }
  }
  sourceBuffer = null;
  bufferValid = false;
  if (mediaSource && mediaSource.readyState === 'open') {
    try {
      mediaSource.endOfStream();
    } catch (e) {
      /* ignore */
    }
  }
  mediaSource = null;
  if (objectUrl) {
    URL.revokeObjectURL(objectUrl);
    objectUrl = null;
  }
  if (video) {
    video.removeAttribute('src');
    video.load();
  }
}
