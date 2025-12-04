const apiHost = location.hostname || 'localhost';
const apiPort = '8080';
const apiBase = `${location.protocol}//${apiHost}:${apiPort}`;
const wsUrl = `ws://${apiHost}:${apiPort}`;

function checkLogin() {
  const sid = localStorage.getItem('ott_session_id');
  if (!sid) {
    window.location.href = 'login.html';
    return null;
  }
  return sid;
}

function logout() {
  localStorage.removeItem('ott_session_id');
  localStorage.removeItem('ott_username');
  localStorage.removeItem('ott_is_admin');
  window.location.href = 'login.html';
}

function formatDuration(seconds) {
  if (!seconds) return '';
  const m = Math.floor(seconds / 60);
  const s = seconds % 60;
  return `${m}분 ${s}초`;
}
