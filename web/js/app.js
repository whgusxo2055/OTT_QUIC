const wsUrl = `ws://${location.hostname}:8080`;
let socket = null;
let sessionId = null;
let currentVideo = null;
let connectionId = 0;
let streamId = 1;
let totalBytes = 0;
let chunkSize = 1024 * 1024;
let durationMs = 0;

function log(msg) {
  const el = document.getElementById('log');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
}

function connect() {
  socket = new WebSocket(wsUrl);
  socket.onopen = () => log('WebSocket connected');
  socket.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data);
      handleMessage(data);
    } catch (e) {
      log(`recv: ${ev.data}`);
    }
  };
  socket.onerror = (err) => log('ws error ' + err);
  socket.onclose = () => log('WebSocket closed');
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
  const chunkLen = 1024 * 1024;
  send({
    type: 'stream_start',
    video_id: currentVideo,
    connection_id: connectionId,
    stream_id: streamId,
    chunk_length: chunkLen,
  });
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
}

function watchGet() {
  if (!currentVideo) return;
  send({
    type: 'watch_get',
    user_id: 1,
    video_id: currentVideo,
  });
}

function watchSet() {
  if (!currentVideo) return;
  const pos = parseInt(document.getElementById('watch-pos').value, 10) || 0;
  send({
    type: 'watch_update',
    user_id: 1,
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
