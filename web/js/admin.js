const sessionId = checkLogin();

if (localStorage.getItem('ott_is_admin') !== 'true') {
  alert('관리자 권한이 필요합니다.');
  window.location.href = 'index.html';
}

document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('logout-btn').addEventListener('click', logout);
  document.getElementById('upload-btn').addEventListener('click', uploadVideo);
  initTabs();
  loadVideoList();
});

function initTabs() {
  const tabButtons = document.querySelectorAll('.tab-btn');
  tabButtons.forEach(btn => {
    btn.addEventListener('click', () => {
      tabButtons.forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      const target = btn.getAttribute('data-tab');
      document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
      document.getElementById(target).classList.add('active');
    });
  });
}

async function uploadVideo() {
  const fileInput = document.getElementById('upload-file');
  const titleInput = document.getElementById('upload-title');
  const descInput = document.getElementById('upload-desc');
  const progressFill = document.getElementById('upload-progress');
  const msgEl = document.getElementById('upload-msg');

  if (!fileInput.files.length) {
    alert('파일을 선택해주세요.');
    return;
  }

  const formData = new FormData();
  formData.append('file', fileInput.files[0]);
  formData.append('title', titleInput.value);
  formData.append('description', descInput.value);

  msgEl.textContent = '업로드 중...';
  progressFill.style.width = '0%';

  try {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', apiBase + '/upload');
    
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const percent = (e.loaded / e.total) * 100;
        progressFill.style.width = percent + '%';
      }
    };

    xhr.onload = () => {
      if (xhr.status === 200) {
        msgEl.textContent = '업로드 완료!';
        progressFill.style.width = '100%';
        alert('업로드가 완료되었습니다.');
        // Reset form
        fileInput.value = '';
        titleInput.value = '';
        descInput.value = '';
      } else {
        msgEl.textContent = '업로드 실패: ' + xhr.statusText;
      }
    };

    xhr.onerror = () => {
      msgEl.textContent = '네트워크 오류';
    };

    xhr.send(formData);
  } catch (e) {
    console.error(e);
    msgEl.textContent = '오류 발생';
  }
}

async function loadVideoList() {
  const container = document.getElementById('video-list');
  container.innerHTML = '로딩 중...';
  try {
    const res = await fetch(apiBase + '/admin/video/list', { credentials: 'include' });
    if (!res.ok) {
      container.innerHTML = '목록을 불러오지 못했습니다.';
      return;
    }
    const data = await res.json();
    renderVideoList(data.items || []);
  } catch (e) {
    console.error(e);
    container.innerHTML = '오류가 발생했습니다.';
  }
}

function renderVideoList(items) {
  const container = document.getElementById('video-list');
  if (!items.length) {
    container.innerHTML = '<p style="color:#ccc;">등록된 영상이 없습니다.</p>';
    return;
  }
  container.innerHTML = '';
  items.forEach(video => {
    const card = document.createElement('div');
    card.className = 'admin-card';
    card.innerHTML = `
      <div class="admin-card-info">
        <div class="admin-file">${video.file_path || ''}</div>
        <div class="admin-title">${video.title || ''}</div>
        <div class="admin-desc">${video.description || ''}</div>
      </div>
      <div class="admin-card-actions">
        <button class="btn btn-secondary btn-sm" data-id="${video.id}" data-title="${video.title || ''}" data-desc="${video.description || ''}">수정</button>
        <button class="btn btn-red btn-sm" data-del="${video.id}">삭제</button>
      </div>
    `;
    container.appendChild(card);
  });
  container.querySelectorAll('button[data-id]').forEach(btn => {
    btn.onclick = () => openEditModal(btn.getAttribute('data-id'), btn.getAttribute('data-title'), btn.getAttribute('data-desc'));
  });
  container.querySelectorAll('button[data-del]').forEach(btn => {
    btn.onclick = () => deleteVideo(btn.getAttribute('data-del'));
  });
}

function openEditModal(id, title, desc) {
  const modal = document.getElementById('edit-modal');
  modal.style.display = 'flex';
  modal.setAttribute('data-id', id);
  document.getElementById('edit-title').value = title || '';
  document.getElementById('edit-desc').value = desc || '';
}

document.getElementById('edit-cancel').onclick = () => {
  document.getElementById('edit-modal').style.display = 'none';
};

document.getElementById('edit-save').onclick = async () => {
  const modal = document.getElementById('edit-modal');
  const id = modal.getAttribute('data-id');
  const title = document.getElementById('edit-title').value;
  const desc = document.getElementById('edit-desc').value;
  try {
    const res = await fetch(apiBase + '/admin/video/update', {
      method: 'POST',
      credentials: 'include',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ video_id: Number(id), title, description: desc }),
    });
    if (res.ok) {
      alert('수정되었습니다.');
      modal.style.display = 'none';
      loadVideoList();
    } else {
      alert('수정 실패: ' + res.status);
    }
  } catch (e) {
    console.error(e);
    alert('수정 오류');
  }
};

async function deleteVideo(id) {
  if (!confirm('정말 삭제하시겠습니까?')) return;
  try {
    const res = await fetch(apiBase + '/admin/video/delete', {
      method: 'POST',
      credentials: 'include',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ video_id: Number(id) }),
    });
    if (res.ok) {
      alert('삭제되었습니다.');
      loadVideoList();
    } else {
      alert('삭제 실패: ' + res.status);
    }
  } catch (e) {
    console.error(e);
    alert('삭제 오류');
  }
}
