const apiHost = location.hostname || 'localhost';
const apiPort = '8080';
const apiBase = `${location.protocol}//${apiHost}:${apiPort}`;
const wsUrl = `ws://${apiHost}:${apiPort}`;
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
let segmentRetries = {};
let lastRequestedSegment = 0;
const maxSegmentRetries = 3;
const segmentRetryDelayMs = 500;
let videoCache = [];
let currentFilter = '';

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
        showToast('서버 응답 오류');
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
  const res = await fetch(apiBase + '/login', {
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
  await fetch(apiBase + '/logout', {
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
  segmentRetries = {};
  send({type: 'video_detail', video_id: id});
  const v = videoCache.find((x) => x.id === id);
  if (v) updateHero(v);
}

function startStream() {
  if (!currentVideo) {
    log('no video selected');
    return;
  }
  connectionId = Math.floor(Math.random() * 1e6) + 1;
  streamId = 1;
  nextSegment = 0;
  segmentRetries = {};
  lastRequestedSegment = 0;
  setupMediaSource();
  send({type: 'ws_init', video_id: currentVideo});
  openDrawer();
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
const watchInline = document.getElementById('watch-get-inline');
if (watchInline) watchInline.onclick = watchGet;
const drawerClose = document.getElementById('drawer-close');
if (drawerClose) drawerClose.onclick = closeDrawer;
const searchInput = document.getElementById('search-input');
if (searchInput) {
  searchInput.oninput = () => {
    currentFilter = searchInput.value.trim().toLowerCase();
    renderVideoList(videoCache);
  };
}
const uploadBtn = document.getElementById('upload-btn');
if (uploadBtn) uploadBtn.onclick = uploadVideo;
const uploadFile = document.getElementById('upload-file');
const uploadTitle = document.getElementById('upload-title');
const uploadDesc = document.getElementById('upload-desc');

connect();

function handleMessage(msg) {
  if (!msg || !msg.type) {
    log('unknown message');
    return;
  }
  switch (msg.type) {
    case 'videos':
      videoCache = msg.items || [];
      renderVideoList(videoCache);
      if (videoCache.length > 0) {
        updateHero(videoCache[0]);
      }
      break;
    case 'video_detail':
      renderDetail(msg);
      break;
    case 'watch_get':
      if (msg.status === 'ok' && typeof msg.position === 'number') {
        document.getElementById('watch-pos').value = msg.position;
      } else if (msg.status === 'error' && msg.message === 'login-required') {
        handleSessionExpired();
      }
      log(JSON.stringify(msg));
      break;
    case 'watch_update':
    case 'stream_chunk':
    case 'stream_stop':
    case 'stream_seek':
      if (msg.status === 'error' && msg.message === 'login-required') {
        handleSessionExpired();
      }
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
        segmentRetries = {};
      } else if (msg.status === 'error' && msg.message) {
        showToast(msg.message);
        if (msg.message === 'login-required') {
          handleSessionExpired();
        }
      }
      log(JSON.stringify(msg));
      break;
    case 'ws_segment':
      if (msg.status === 'error') {
        const seg = typeof msg.segment === 'number' ? msg.segment : lastRequestedSegment;
        handleSegmentError(seg, msg.message || 'segment error');
        if (msg.message === 'login-required') {
          handleSessionExpired();
        }
      } else {
        clearSegmentRetry(msg.segment);
      }
      log(JSON.stringify(msg));
      break;
    default:
      if (msg.status === 'unauthorized' || msg.message === 'login-required') {
        handleSessionExpired();
      }
      log(JSON.stringify(msg));
      break;
  }
}

function renderVideoList(items) {
  const list = document.getElementById('video-list');
  const cont = document.getElementById('continue-list');
  if (!list || !cont) return;
  list.innerHTML = '';
  cont.innerHTML = '';

  const filtered = currentFilter
    ? items.filter((i) => (i.title || '').toLowerCase().includes(currentFilter))
    : items;

  filtered.forEach((item, idx) => {
    const card = createCard(item);
    list.appendChild(card);
    if (idx < 6) {
      cont.appendChild(createCard(item));
    }
  });
}

function createCard(item) {
  const card = document.createElement('div');
  card.className = 'card';
  card.onclick = () => selectVideo(item.id);
  const thumb = document.createElement('div');
  thumb.className = 'thumb';
  thumb.style.backgroundImage = item.thumbnail_path ? `url(${item.thumbnail_path})` : '';
  thumb.style.backgroundSize = 'cover';
  thumb.style.backgroundPosition = 'center';
  thumb.textContent = item.thumbnail_path ? '' : '썸네일 없음';
  const body = document.createElement('div');
  body.className = 'body';
  const title = document.createElement('div');
  title.className = 'title';
  title.textContent = item.title || `영상 ${item.id}`;
  const meta = document.createElement('div');
  meta.className = 'meta';
  meta.textContent = `${item.duration || 0}s`;
  const actions = document.createElement('div');
  actions.className = 'actions';
  const playBtn = document.createElement('button');
  playBtn.textContent = '재생';
  playBtn.onclick = (e) => {
    e.stopPropagation();
    selectVideo(item.id);
    startStream();
  };
  const infoBtn = document.createElement('button');
  infoBtn.className = 'ghost';
  infoBtn.textContent = '정보';
  infoBtn.onclick = (e) => {
    e.stopPropagation();
    selectVideo(item.id);
  };
  actions.appendChild(playBtn);
  actions.appendChild(infoBtn);
  body.appendChild(title);
  body.appendChild(meta);
  card.appendChild(thumb);
  card.appendChild(body);
  card.appendChild(actions);
  return card;
}

function renderDetail(d) {
  document.getElementById('detail-title').textContent = d.title || '-';
  document.getElementById('detail-desc').textContent = d.description || '-';
  document.getElementById('detail-duration').textContent = d.duration || '-';
  document.getElementById('detail-file').textContent = d.file_path || '-';
  if (d.thumbnail_path) {
    document.getElementById('hero-bg').style.backgroundImage = `url(${d.thumbnail_path})`;
  }
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
      clearSegmentRetry(idx);
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
  lastRequestedSegment = nextSegment;
  send({
    type: 'ws_segment',
    video_id: currentVideo,
    segment: nextSegment,
  });
}

function handleSegmentError(segIndex, reason) {
  const count = (segmentRetries[segIndex] || 0) + 1;
  segmentRetries[segIndex] = count;
  if (count > maxSegmentRetries) {
    showToast(`세그먼트 ${segIndex} 요청 실패: ${reason || 'unknown'}`);
    log(`segment ${segIndex} failed after ${count} attempts`);
    return;
  }
  showToast(`세그먼트 ${segIndex} 재시도 (${count})`);
  setTimeout(() => {
    lastRequestedSegment = segIndex;
    send({
      type: 'ws_segment',
      video_id: currentVideo,
      segment: segIndex,
    });
  }, segmentRetryDelayMs * count);
}

function clearSegmentRetry(segIndex) {
  if (typeof segIndex === 'number') {
    segmentRetries[segIndex] = 0;
  }
}

function saveWatch() {
  const pos = parseInt(document.getElementById('seek-offset').value, 10) || 0;
  if (!currentVideo) return;
  send({type: 'watch_update', video_id: currentVideo, position: pos});
}

function handleSessionExpired() {
  sessionId = null;
  document.getElementById('session-info').textContent = '';
  if (watchTimer) {
    clearInterval(watchTimer);
    watchTimer = null;
  }
  showToast('세션이 만료되었습니다. 다시 로그인하세요.');
}

function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  setTimeout(() => t.classList.add('hidden'), 2500);
}

function updateHero(item) {
  const title = document.getElementById('hero-title');
  const desc = document.getElementById('hero-desc');
  const bg = document.getElementById('hero-bg');
  title.textContent = item.title || '영상';
  desc.textContent = item.description || '';
  if (item.thumbnail_path) {
    bg.style.backgroundImage = `url(${item.thumbnail_path})`;
  }
}

function openDrawer() {
  const d = document.getElementById('player-drawer');
  if (d) d.classList.remove('hidden');
}

function closeDrawer() {
  const d = document.getElementById('player-drawer');
  if (d) d.classList.add('hidden');
}

async function uploadVideo() {
  const fileInput = document.getElementById('upload-file');
  const title = document.getElementById('upload-title').value;
  const desc = document.getElementById('upload-desc').value;
  const msgEl = document.getElementById('upload-msg');
  const prog = document.getElementById('upload-progress');
  if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
    showToast('업로드할 파일을 선택하세요');
    return;
  }
  const file = fileInput.files[0];
  const form = new FormData();
  form.append('file', file);
  if (title) form.append('title', title);
  if (desc) form.append('description', desc);

  prog.style.width = '30%';
  msgEl.textContent = '업로드 중...';
  try {
    const res = await fetch(apiBase + '/upload', {
      method: 'POST',
      body: form,
    });
    prog.style.width = '70%';
    if (!res.ok) {
      const txt = await res.text();
      throw new Error(txt || 'upload failed');
    }
    prog.style.width = '100%';
    msgEl.textContent = '업로드 완료';
    showToast('업로드 성공');
    refreshVideos();
  } catch (e) {
    prog.style.width = '0%';
    msgEl.textContent = '업로드 실패';
    showToast('업로드 실패');
    log(e.toString());
  }
}
