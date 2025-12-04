const sessionId = checkLogin();
let socket = null;
let videoCache = [];
let continueCache = [];

document.addEventListener('DOMContentLoaded', () => {
  connectWebSocket();
  document.getElementById('logout-btn').addEventListener('click', logout);
  
  if (localStorage.getItem('ott_is_admin') === 'true') {
    document.getElementById('admin-link').style.display = 'inline-block';
  }
});

function connectWebSocket() {
  socket = new WebSocket(wsUrl);
  
  socket.onopen = () => {
    console.log('WebSocket connected');
    send({type: 'list_videos'});
    send({type: 'list_continue'});
  };

  socket.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      try {
        const data = JSON.parse(ev.data);
        handleMessage(data);
      } catch (e) {
        console.error('JSON Parse Error', e);
      }
    }
  };
}

function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(obj));
  }
}

function handleMessage(msg) {
  if (msg.type === 'videos') {
    videoCache = msg.items || [];
    renderMainLists();
  } else if (msg.type === 'continue_videos') {
    continueCache = msg.items || [];
    renderContinueList();
  }
}

function renderMainLists() {
  if (videoCache.length === 0) return;

  const heroVideo = videoCache[0];
  updateHero(heroVideo);

  renderCarousel('video-list', videoCache);
  renderCarousel('new-list', videoCache.slice().reverse().slice(0, 5));
}

function renderContinueList() {
  const container = document.getElementById('continue-list');
  if (!container) return;
  
  if (continueCache.length === 0) {
    container.innerHTML = '<p style="color: #666; padding: 20px;">시청 중인 콘텐츠가 없습니다.</p>';
    return;
  }
  renderCarousel('continue-list', continueCache);
}

function updateHero(video) {
  document.getElementById('hero-title').textContent = video.title;
  document.getElementById('hero-desc').textContent = video.description || 'No description available.';
  
  const heroSection = document.getElementById('hero-section');
  if (video.thumbnail_path) {
    heroSection.style.backgroundImage = `url('${video.thumbnail_path}')`;
  }
  
  document.getElementById('hero-play-btn').onclick = () => {
    window.location.href = `watch.html?id=${video.id}`;
  };
}

function renderCarousel(elementId, videos) {
  const container = document.getElementById(elementId);
  if (!container) return;
  
  container.innerHTML = '';
  
  videos.forEach(video => {
    const card = document.createElement('div');
    card.className = 'video-card';
    card.onclick = () => {
      window.location.href = `watch.html?id=${video.id}`;
    };
    
    const img = document.createElement('img');
    img.src = video.thumbnail_path || 'https://via.placeholder.com/320x180?text=No+Image';
    img.alt = video.title;
    
    const info = document.createElement('div');
    info.className = 'video-info';
    
    const title = document.createElement('div');
    title.className = 'video-title';
    title.textContent = video.title;
    
    const meta = document.createElement('div');
    meta.className = 'video-meta';
    meta.textContent = formatDuration(video.duration);
    
    info.appendChild(title);
    info.appendChild(meta);
    
    card.appendChild(img);
    card.appendChild(info);
    
    container.appendChild(card);
  });
}
