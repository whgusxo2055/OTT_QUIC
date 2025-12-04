const sessionId = checkLogin();

if (localStorage.getItem('ott_is_admin') !== 'true') {
  alert('관리자 권한이 필요합니다.');
  window.location.href = 'index.html';
}

document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('logout-btn').addEventListener('click', logout);
  document.getElementById('upload-btn').addEventListener('click', uploadVideo);
});

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
