const wsUrl = `ws://${location.hostname}:8080`;
let socket = null;
let sessionId = null;
let currentVideo = null;
let connectionId = 0;
let streamId = 1;
let totalBytes = 0;
let chunkSize = 1024 * 1024;
let durationMs = 0;
let mediaSource = null;
let sourceBuffer = null;
let segmentQueue = [];
let appending = false;
let nextSegment = 0;
const maxSegments = 100;
let watchTimer = null;
let watchTimer = null;

function log(msg) {
  const el = document.getElementById('log');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
}

function connect() {
  socket = new WebSocket(wsUrl);
  socket.onopen = () => log('WebSocket connected');
  socket.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      try {
        const data = JSON.parse(ev.data);
        handleMessage(data);
      } catch (e) {
        log(`recv: ${ev.data}`);
    showToast("서버 응답 오류");
      }
    } else {
      handleBinary(ev.data);
    }
  };
  socket.onerror = (err) => { log('ws error ' + err); showToast('WebSocket 오류');};
  socket.onclose = () => { log('WebSocket closed'); showToast('연결이 종료되었습니다');};
}

function send(obj) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    log('socket not open');
    return;
  }
  socket.send(JSON.stringify(obj));
}

async function login() {
  const user = document.getElementById('username').value;
  const pass = document.getElementById('password').value;
  const res = await fetch('http://' + location.hostname + ':8080/login', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    credentials: 'include',
    body: JSON.stringify({username: user, password: pass}),
  });
  if (!res.ok) {
    log('login failed');
    return;
  }
  const data = await res.json();
  sessionId = data.session_id;
  document.getElementById('session-info').textContent = 'SID: ' + sessionId;
  log('login success');
}

async function logout() {
  await fetch('http://' + location.hostname + ':8080/logout', {
    method: 'GET',
    credentials: 'include',
    headers: sessionId ? {'Authorization': 'Bearer ' + sessionId} : {},
  });
  sessionId = null;
  document.getElementById('session-info').textContent = '';
  log('logout');
}

function refreshVideos() {
  send({type: 'list_videos'});
}

function selectVideo(id) {
  currentVideo = id;
  send({type: 'video_detail', video_id: id});
}

function startStream() {
  if (!currentVideo) {
    log('no video selected');
    return;
  }
  connectionId = Math.floor(Math.random() * 1e6) + 1;
  streamId = 1;
  nextSegment = 0;
  setupMediaSource();
  send({type: 'ws_init', video_id: currentVideo});
}

function requestChunk() {
  if (!currentVideo) {
    log('no video selected');
    return;
  }
  const offset = parseInt(document.getElementById('seek-offset').value, 10) || 0;
  const length = 1024 * 1024;
  send({
    type: 'stream_chunk',
    video_id: currentVideo,
    connection_id: connectionId,
    stream_id: streamId,
    offset,
    length,
  });
}

function seekStream() {
  const offset = parseInt(document.getElementById('seek-offset').value, 10) || 0;
  send({
    type: 'stream_seek',
    video_id: currentVideo,
    connection_id: connectionId,
    stream_id: streamId,
    offset,
  });
  requestChunk();
}

function stopStream() {
  send({type: 'stream_stop', connection_id: connectionId});
  if (watchTimer) {
    clearInterval(watchTimer);
    watchTimer = null;
  }
}

function watchGet() {
  if (!currentVideo) return;
  send({
    type: 'watch_get',
    video_id: currentVideo,
  });
}

function watchSet() {
  if (!currentVideo) return;
  const pos = parseInt(document.getElementById('watch-pos').value, 10) || 0;
  send({
    type: 'watch_update',
    video_id: currentVideo,
    position: pos,
  });
}

document.getElementById('login-btn').onclick = login;
document.getElementById('logout-btn').onclick = logout;
document.getElementById('refresh-btn').onclick = refreshVideos;
document.getElementById('start-btn').onclick = startStream;
document.getElementById('seek-btn').onclick = seekStream;
document.getElementById('stop-btn').onclick = stopStream;
document.getElementById('watch-get-btn').onclick = watchGet;
document.getElementById('watch-set-btn').onclick = watchSet;

connect();

function handleMessage(msg) {
  if (!msg || !msg.type) {
    log('unknown message');
    return;
  }
  switch (msg.type) {
    case 'videos':
      renderVideoList(msg.items || []);
      break;
    case 'video_detail':
      renderDetail(msg);
      break;
    case 'watch_get':
      if (msg.status === 'ok' && typeof msg.position === 'number') {
        document.getElementById('watch-pos').value = msg.position;
      }
      log(JSON.stringify(msg));
      break;
    case 'watch_update':
    case 'stream_chunk':
    case 'stream_stop':
    case 'stream_seek':
      log(JSON.stringify(msg));
      break;
    case 'stream_start':
      if (msg.status === 'ok') {
        totalBytes = msg.total_bytes || 0;
        chunkSize = msg.chunk_size || chunkSize;
        connectionId = msg.connection_id || connectionId;
        streamId = msg.stream_id || streamId;
        durationMs = (msg.duration || 0) * 1000;
        updateMeta();
        if (watchTimer) clearInterval(watchTimer);
        watchTimer = setInterval(saveWatch, 10000);
      }
      log(JSON.stringify(msg));
      break;
    case 'ws_segment':
      if (msg.status === 'error') {
        showToast(msg.message || 'segment error');
      }
      log(JSON.stringify(msg));
      break;
    default:
      log(JSON.stringify(msg));
      break;
  }
}

function renderVideoList(items) {
  const ul = document.getElementById('video-list');
  ul.innerHTML = '';
  items.forEach((item) => {
    const li = document.createElement('li');
    li.textContent = `${item.id} - ${item.title}`;
    li.onclick = () => selectVideo(item.id);
    ul.appendChild(li);
  });
}

function renderDetail(d) {
  document.getElementById('detail-title').textContent = d.title || '-';
  document.getElementById('detail-desc').textContent = d.description || '-';
  document.getElementById('detail-duration').textContent = d.duration || '-';
  document.getElementById('detail-file').textContent = d.file_path || '-';
  document.getElementById('detail-thumb').textContent = d.thumbnail_path || '-';
}

function updateMeta() {
  document.getElementById('meta-conn').textContent = connectionId || '-';
  document.getElementById('meta-stream').textContent = streamId || '-';
  document.getElementById('meta-bytes').textContent = totalBytes || '-';
  document.getElementById('meta-chunk').textContent = chunkSize || '-';
  document.getElementById('seek-offset').value = 0;
}

function setupMediaSource() {
  const video = document.getElementById('player');
  mediaSource = new MediaSource();
  video.src = URL.createObjectURL(mediaSource);
  mediaSource.addEventListener('sourceopen', () => {
    sourceBuffer = mediaSource.addSourceBuffer('video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\"');
    sourceBuffer.addEventListener('updateend', () => {
      appending = false;
      appendNext();
    });
  });
  segmentQueue = [];
  appending = false;
}

function handleBinary(data) {
  const reader = new FileReader();
  reader.onload = () => {
    const arr = new Uint8Array(reader.result);
    if (arr.length < 8) {
      log('binary too small');
      return;
    }
    const magic = String.fromCharCode(arr[0], arr[1], arr[2], arr[3]);
    const idx = (arr[4] << 24) | (arr[5] << 16) | (arr[6] << 8) | arr[7];
    const payload = arr.slice(8);
    if (magic === 'INIT') {
      appendSegment(payload.buffer);
      nextSegment = 0;
      requestSegment();
    } else if (magic === 'SEGM') {
      appendSegment(payload.buffer);
      nextSegment = idx + 1;
      if (nextSegment < maxSegments) {
        requestSegment();
      }
    } else {
      log('unknown binary magic ' + magic);
    }
  };
  reader.readAsArrayBuffer(data);
}

function appendSegment(buffer) {
  if (!sourceBuffer) {
    log('no sourceBuffer');
    return;
  }
  segmentQueue.push(buffer);
  appendNext();
}

function appendNext() {
  if (!sourceBuffer || sourceBuffer.updating || appending || segmentQueue.length === 0) {
    return;
  }
  appending = true;
  const seg = segmentQueue.shift();
  try {
    sourceBuffer.appendBuffer(seg);
  } catch (e) {
    log('append error ' + e);
    appending = false;
  }
}

function requestSegment() {
  send({
    type: 'ws_segment',
    video_id: currentVideo,
    segment: nextSegment,
  });
}

function saveWatch() {
  const pos = parseInt(document.getElementById('seek-offset').value, 10) || 0;
  if (!currentVideo) return;
  send({type: 'watch_update', video_id: currentVideo, position: pos});
}

function saveWatch() {
  const pos = parseInt(document.getElementById('seek-offset').value, 10) || 0;
  if (!currentVideo) return;
  send({type: 'watch_update', video_id: currentVideo, position: pos});
}


function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 2500);
}
