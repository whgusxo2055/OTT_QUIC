const wsUrl = `ws://${location.hostname}:8080`;
let socket = null;
let sessionId = null;
let currentVideo = null;
let connectionId = 0;
let streamId = 1;

function log(msg) {
  const el = document.getElementById('log');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
}

function connect() {
  socket = new WebSocket(wsUrl);
  socket.onopen = () => log('WebSocket connected');
  socket.onmessage = (ev) => {
    log(`recv: ${ev.data}`);
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
document.getElementById('seek-btn').onclick = seekStream;
document.getElementById('stop-btn').onclick = stopStream;
document.getElementById('watch-get-btn').onclick = watchGet;
document.getElementById('watch-set-btn').onclick = watchSet;

connect();
