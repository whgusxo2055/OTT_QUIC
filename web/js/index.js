const sessionId = checkLogin();
let socket = null;
let videoCache = [];
let continueCache = [];
let currentSortOrder = 'latest';
let pendingVideo = null;  // 이어보기 모달에서 선택된 비디오

document.addEventListener('DOMContentLoaded', () => {
  connectWebSocket();
  document.getElementById('logout-btn').addEventListener('click', logout);
  
  if (localStorage.getItem('ott_is_admin') === 'true') {
    document.getElementById('admin-link').style.display = 'inline-block';
  }
  
  // 검색 기능 초기화
  initSearch();
  
  // 정렬 기능 초기화
  initSort();
  
  // 모달 초기화
  initModals();
});

// ===== 검색 기능 =====
function initSearch() {
  const searchContainer = document.getElementById('search-container');
  const searchInput = document.getElementById('search-input');
  const searchBtn = document.getElementById('search-btn');
  
  // 검색 결과 컨테이너 생성
  const resultsContainer = document.createElement('div');
  resultsContainer.className = 'search-results';
  resultsContainer.id = 'search-results';
  searchContainer.appendChild(resultsContainer);
  
  // 검색 버튼 클릭 - 검색창 토글
  searchBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    searchContainer.classList.toggle('active');
    if (searchContainer.classList.contains('active')) {
      searchInput.focus();
    } else {
      searchInput.value = '';
      resultsContainer.classList.remove('show');
    }
  });
  
  // 검색 입력
  let searchTimeout = null;
  searchInput.addEventListener('input', (e) => {
    const query = e.target.value.trim();
    
    // 디바운싱
    if (searchTimeout) clearTimeout(searchTimeout);
    
    if (query.length === 0) {
      resultsContainer.classList.remove('show');
      return;
    }
    
    searchTimeout = setTimeout(() => {
      performSearch(query);
    }, 300);
  });
  
  // 검색창 외부 클릭 시 닫기
  document.addEventListener('click', (e) => {
    if (!searchContainer.contains(e.target)) {
      searchContainer.classList.remove('active');
      searchInput.value = '';
      resultsContainer.classList.remove('show');
    }
  });
  
  // Enter 키로 검색
  searchInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      const query = searchInput.value.trim();
      if (query.length > 0) {
        performSearch(query);
      }
    }
    if (e.key === 'Escape') {
      searchContainer.classList.remove('active');
      searchInput.value = '';
      resultsContainer.classList.remove('show');
    }
  });
}

function performSearch(query) {
  const resultsContainer = document.getElementById('search-results');
  const lowerQuery = query.toLowerCase();
  
  // 비디오 캐시에서 검색
  const results = videoCache.filter(video => {
    const titleMatch = video.title && video.title.toLowerCase().includes(lowerQuery);
    const descMatch = video.description && video.description.toLowerCase().includes(lowerQuery);
    return titleMatch || descMatch;
  });
  
  renderSearchResults(results);
}

function renderSearchResults(results) {
  const resultsContainer = document.getElementById('search-results');
  resultsContainer.innerHTML = '';
  
  if (results.length === 0) {
    resultsContainer.innerHTML = '<div class="search-no-results">검색 결과가 없습니다.</div>';
    resultsContainer.classList.add('show');
    return;
  }
  
  results.forEach(video => {
    const item = document.createElement('div');
    item.className = 'search-result-item';
    item.onclick = () => {
      window.location.href = `watch.html?id=${video.id}`;
    };
    
    const img = document.createElement('img');
    img.src = video.thumbnail_path || 'https://via.placeholder.com/100x56?text=No+Image';
    img.alt = video.title;
    
    const info = document.createElement('div');
    info.className = 'search-result-info';
    
    const title = document.createElement('div');
    title.className = 'search-result-title';
    title.textContent = video.title;
    
    const desc = document.createElement('div');
    desc.className = 'search-result-desc';
    desc.textContent = video.description || '설명 없음';
    
    info.appendChild(title);
    info.appendChild(desc);
    
    item.appendChild(img);
    item.appendChild(info);
    resultsContainer.appendChild(item);
  });
  
  resultsContainer.classList.add('show');
}

// ===== 정렬 기능 =====
function initSort() {
  const sortSelect = document.getElementById('sort-select');
  if (!sortSelect) return;
  
  sortSelect.addEventListener('change', (e) => {
    currentSortOrder = e.target.value;
    renderMainLists();
  });
}

function sortVideos(videos) {
  const sorted = [...videos];
  
  switch (currentSortOrder) {
    case 'title':
      sorted.sort((a, b) => (a.title || '').localeCompare(b.title || ''));
      break;
    case 'duration':
      sorted.sort((a, b) => (b.duration || 0) - (a.duration || 0));
      break;
    case 'latest':
    default:
      sorted.sort((a, b) => (b.id || 0) - (a.id || 0));
      break;
  }
  
  return sorted;
}

// ===== 모달 기능 =====
function initModals() {
  // 이어보기 모달
  const resumeModal = document.getElementById('resume-modal');
  const modalClose = document.getElementById('modal-close');
  const modalResume = document.getElementById('modal-resume');
  const modalRestart = document.getElementById('modal-restart');
  
  if (modalClose) {
    modalClose.addEventListener('click', () => {
      resumeModal.style.display = 'none';
      pendingVideo = null;
    });
  }
  
  if (modalResume) {
    modalResume.addEventListener('click', () => {
      if (pendingVideo) {
        const position = pendingVideo.position || 0;
        window.location.href = `watch.html?id=${pendingVideo.id}&t=${Math.floor(position)}`;
      }
    });
  }
  
  if (modalRestart) {
    modalRestart.addEventListener('click', () => {
      if (pendingVideo) {
        window.location.href = `watch.html?id=${pendingVideo.id}&t=0`;
      }
    });
  }
  
  // 모달 외부 클릭 시 닫기
  resumeModal.addEventListener('click', (e) => {
    if (e.target === resumeModal) {
      resumeModal.style.display = 'none';
      pendingVideo = null;
    }
  });
  
  // 상세 모달
  const detailModal = document.getElementById('detail-modal');
  const detailClose = document.getElementById('detail-close');
  const detailPlay = document.getElementById('detail-play');
  
  if (detailClose) {
    detailClose.addEventListener('click', () => {
      detailModal.style.display = 'none';
    });
  }
  
  if (detailPlay) {
    detailPlay.addEventListener('click', () => {
      if (pendingVideo) {
        window.location.href = `watch.html?id=${pendingVideo.id}`;
      }
    });
  }
  
  // 상세 모달 외부 클릭 시 닫기
  detailModal.addEventListener('click', (e) => {
    if (e.target === detailModal) {
      detailModal.style.display = 'none';
    }
  });
  
  // ESC 키로 모달 닫기
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      resumeModal.style.display = 'none';
      detailModal.style.display = 'none';
      pendingVideo = null;
    }
  });
}

function showResumeModal(video) {
  pendingVideo = video;
  
  const modal = document.getElementById('resume-modal');
  const thumb = document.getElementById('modal-thumb');
  const title = document.getElementById('modal-title');
  const position = document.getElementById('modal-position');
  const progress = document.getElementById('modal-progress');
  
  thumb.src = video.thumbnail_path || 'https://via.placeholder.com/320x180?text=No+Image';
  title.textContent = video.title;
  
  const positionSec = video.position || 0;
  const durationSec = video.duration || 0;
  const percent = durationSec > 0 ? (positionSec / durationSec) * 100 : 0;
  
  position.textContent = `${formatDuration(positionSec)} / ${formatDuration(durationSec)} (${Math.floor(percent)}% 시청)`;
  progress.style.width = `${percent}%`;
  
  modal.style.display = 'flex';
}

function showDetailModal(video) {
  pendingVideo = video;
  
  const modal = document.getElementById('detail-modal');
  const backdrop = document.getElementById('detail-backdrop');
  const title = document.getElementById('detail-title');
  const desc = document.getElementById('detail-desc');
  const duration = document.getElementById('detail-duration');
  
  backdrop.style.backgroundImage = `url('${video.thumbnail_path || ''}')`;
  title.textContent = video.title;
  desc.textContent = video.description || '설명이 없습니다.';
  duration.textContent = `재생시간: ${formatDuration(video.duration)}`;
  
  modal.style.display = 'flex';
}

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
    console.log('[WS] Continue cache updated:', continueCache.length, 'items', continueCache);
    renderContinueList();
    // continueCache 업데이트 후 메인 리스트도 다시 렌더링 (이어보기 모달 체크를 위해)
    if (videoCache.length > 0) {
      renderMainLists();
    }
  }
}

function renderMainLists() {
  if (videoCache.length === 0) return;

  const heroVideo = videoCache[0];
  updateHero(heroVideo);

  // 정렬 적용
  const sortedVideos = sortVideos(videoCache);
  renderCarousel('video-list', sortedVideos, false);
  renderCarousel('new-list', videoCache.slice().reverse().slice(0, 5), false);
}

function renderContinueList() {
  const container = document.getElementById('continue-list');
  if (!container) return;
  
  if (continueCache.length === 0) {
    container.innerHTML = '<p style="color: #666; padding: 20px;">시청 중인 콘텐츠가 없습니다.</p>';
    return;
  }
  renderCarousel('continue-list', continueCache, true);  // 진행 바 표시
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
  
  // 상세 정보 버튼
  const infoBtn = document.querySelector('.hero-actions .btn-secondary');
  if (infoBtn) {
    infoBtn.onclick = () => {
      showDetailModal(video);
    };
  }
}

function renderCarousel(elementId, videos, showProgress = false) {
  const container = document.getElementById(elementId);
  if (!container) return;
  
  container.innerHTML = '';
  
  videos.forEach(video => {
    const card = document.createElement('div');
    card.className = 'video-card';
    
    // 시청 기록 확인 (continueCache에서 해당 비디오 찾기)
    const watchHistory = continueCache.find(c => c.id === video.id || c.video_id === video.id);
    const hasWatchHistory = watchHistory && watchHistory.position && watchHistory.position > 10;
    
    // 디버그 로그
    if (continueCache.length > 0) {
      console.log(`[Carousel] Video ${video.id}: watchHistory=`, watchHistory, 'hasWatchHistory=', hasWatchHistory);
    }
    
    // 시청 중인 콘텐츠 섹션이거나 시청 기록이 있는 경우 이어보기 모달 표시
    if ((showProgress && video.position && video.position > 0) || hasWatchHistory) {
      card.onclick = () => {
        // 시청 기록 정보를 합친 비디오 객체 생성
        const videoWithHistory = hasWatchHistory ? {
          ...video,
          position: watchHistory.position,
          duration: watchHistory.duration || video.duration
        } : video;
        showResumeModal(videoWithHistory);
      };
    } else {
      card.onclick = () => {
        window.location.href = `watch.html?id=${video.id}`;
      };
    }
    
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
    
    // 진행 바 표시 (시청 중인 콘텐츠)
    if (showProgress && video.position && video.duration) {
      const remaining = Math.max(0, video.duration - video.position);
      meta.textContent = `${formatDuration(remaining)} 남음`;
    } else {
      meta.textContent = formatDuration(video.duration);
    }
    
    info.appendChild(title);
    info.appendChild(meta);
    
    card.appendChild(img);
    card.appendChild(info);
    
    // 진행 바 추가
    if (showProgress && video.position && video.duration) {
      const progressContainer = document.createElement('div');
      progressContainer.className = 'video-progress-container';
      
      const progressBar = document.createElement('div');
      progressBar.className = 'video-progress-bar';
      const percent = (video.position / video.duration) * 100;
      progressBar.style.width = `${percent}%`;
      
      progressContainer.appendChild(progressBar);
      card.appendChild(progressContainer);
    }
    
    container.appendChild(card);
  });
}
