# 소프트웨어 요구사항 명세서 (SRS)
## OTT_QUIC - QUIC 기반 OTT 스트리밍 플랫폼

---

## 문서 정보

| 항목 | 내용 |
|------|------|
| 프로젝트명 | OTT_QUIC |
| 버전 | 1.0.0 |
| 작성일 | 2025년 11월 29일 |
| 저장소 | https://github.com/whgusxo2055/OTT_QUIC |

---

## 목차

1. [개요](#1-개요)
2. [시스템 아키텍처](#2-시스템-아키텍처)
3. [기능 요구사항](#3-기능-요구사항)
4. [비기능 요구사항](#4-비기능-요구사항)
5. [데이터베이스 설계](#5-데이터베이스-설계)
6. [인터페이스 요구사항](#6-인터페이스-요구사항)
7. [기술 스택](#7-기술-스택)
8. [제약사항](#8-제약사항)
9. [추후 개선사항](#9-추후-개선사항)

---

## 1. 개요

### 1.1 프로젝트 목적

다수 사용자가 웹 브라우저(Chrome)로 접속하여 동영상을 시청할 수 있는 OTT(Over-The-Top) 스트리밍 플랫폼 개발. QUIC 프로토콜을 직접 구현하여 네트워크 프로그래밍의 핵심 개념을 학습한다.

### 1.2 프로젝트 범위

- QUIC 프로토콜 직접 구현 (UDP 소켓 기반)
- 웹 기반 사용자 인터페이스
- 사용자 인증 및 세션 관리
- 동영상 스트리밍 서비스
- 시청 이력 관리 (이어보기 기능)

### 1.3 용어 정의

| 용어 | 설명 |
|------|------|
| QUIC | Quick UDP Internet Connection, UDP 기반의 전송 프로토콜 |
| OTT | Over-The-Top, 인터넷을 통해 미디어 콘텐츠를 제공하는 서비스 |
| WebSocket | TCP 기반의 양방향 통신 프로토콜 |
| 청크 | 동영상 데이터를 분할한 단위 |
| HLS | HTTP Live Streaming (본 프로젝트에서는 사용하지 않음) |

---

## 2. 시스템 아키텍처

### 2.1 전체 아키텍처

```
┌─────────────────────────────────────────────────────────┐
│                    Chrome 브라우저                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │  index.html │  │  영상 목록   │  │  플레이어   │      │
│  │  (로그인)   │  │   페이지    │  │   페이지    │      │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
└────────────────────────┬────────────────────────────────┘
                         │ WebSocket (TCP:8080)
                         ▼
┌─────────────────────────────────────────────────────────┐
│                   C 서버 (Docker/Linux)                  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │              WebSocket 핸들러 (TCP:8080)           │  │
│  │         • HTTP 업그레이드 • 프레임 처리            │  │
│  └───────────────────────┬───────────────────────────┘  │
│                          │                              │
│  ┌───────────────────────▼───────────────────────────┐  │
│  │              QUIC 엔진 (UDP:8443)                  │  │
│  │   • 연결 관리  • 스트림 다중화  • 패킷 재전송      │  │
│  └───────────────────────┬───────────────────────────┘  │
│                          │                              │
│  ┌───────────────────────▼───────────────────────────┐  │
│  │                  스트리밍 엔진                      │  │
│  │           • 청크 분할 (1MB)  • 순차 전송            │  │
│  └───────────────────────┬───────────────────────────┘  │
│                          │                              │
│  ┌─────────────┬─────────▼─────────┬─────────────────┐  │
│  │   인증/세션  │     SQLite DB     │   썸네일 추출   │  │
│  │   (메모리)  │                   │   (FFmpeg)      │  │
│  └─────────────┴───────────────────┴─────────────────┘  │
│                          │                              │
│  ┌───────────────────────▼───────────────────────────┐  │
│  │              동영상 파일 저장소 (Volume)            │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 2.2 통신 흐름

```
[사용자 로그인]
브라우저 → WebSocket → 서버 → SQLite 조회 → 세션 생성 → 응답

[영상 목록 요청]
브라우저 → WebSocket → 서버 → SQLite 조회 → 목록 + 썸네일 경로 응답

[영상 스트리밍]
브라우저 → WebSocket → 서버 내부 QUIC 처리 → 청크 전송 → 브라우저 재생

[시청 위치 저장]
브라우저 → WebSocket → 서버 → SQLite 저장 (10초 주기)
```

---

## 3. 기능 요구사항

### 3.1 사용자 인증 (FR-AUTH)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-AUTH-01 | 사용자는 ID/Password로 로그인할 수 있다 | 필수 |
| FR-AUTH-02 | 로그인 성공 시 세션이 생성되고 영상 목록 페이지로 이동한다 | 필수 |
| FR-AUTH-03 | 세션은 30분 후 자동 만료된다 | 필수 |
| FR-AUTH-04 | 비밀번호는 해시화하여 저장한다 | 필수 |
| FR-AUTH-05 | 로그인 실패 시 에러 메시지를 표시한다 | 필수 |

### 3.2 동영상 목록 (FR-LIST)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-LIST-01 | 서버에 등록된 동영상 목록을 표시한다 | 필수 |
| FR-LIST-02 | 각 영상은 제목, 썸네일, 설명, 재생시간을 포함한다 | 필수 |
| FR-LIST-03 | 영상 클릭 시 재생 페이지로 이동한다 | 필수 |
| FR-LIST-04 | 이전 시청 기록이 있는 영상은 표시한다 | 필수 |

### 3.3 동영상 재생 (FR-PLAY)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-PLAY-01 | 선택한 영상의 스트리밍을 시작한다 | 필수 |
| FR-PLAY-02 | 사용자가 특정 시간 위치부터 재생할 수 있다 | 필수 |
| FR-PLAY-03 | 재생/일시정지/탐색 컨트롤을 제공한다 | 필수 |
| FR-PLAY-04 | 영상은 원본 해상도로 재생된다 | 필수 |

### 3.4 이어보기 기능 (FR-RESUME)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-RESUME-01 | 재생 중 10초마다 시청 위치를 자동 저장한다 | 필수 |
| FR-RESUME-02 | 사용자가 영상을 정상 종료하면 해당 위치를 저장한다 | 필수 |
| FR-RESUME-03 | 시청 기록이 있는 영상 선택 시 "이어보기" 또는 "처음부터 보기" 선택 가능하다 | 필수 |
| FR-RESUME-04 | 다시 로그인 시 이전 시청 기록이 복원된다 | 필수 |

### 3.5 썸네일 추출 (FR-THUMB)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-THUMB-01 | 동영상 파일에서 썸네일을 자동 추출한다 | 필수 |
| FR-THUMB-02 | 썸네일은 영상 시작 10초 지점에서 추출한다 | 필수 |
| FR-THUMB-03 | 썸네일은 원본 비율을 유지하여 리사이즈한다 | 필수 |
| FR-THUMB-04 | FFmpeg CLI를 사용하여 추출한다 | 필수 |

### 3.6 관리자 기능 (FR-ADMIN)

| ID | 요구사항 | 우선순위 |
|----|----------|----------|
| FR-ADMIN-01 | CLI를 통해 동영상을 추가할 수 있다 | 필수 |
| FR-ADMIN-02 | CLI를 통해 동영상을 삭제할 수 있다 | 필수 |
| FR-ADMIN-03 | CLI를 통해 사용자를 추가할 수 있다 | 필수 |
| FR-ADMIN-04 | CLI를 통해 사용자를 삭제할 수 있다 | 필수 |

---

## 4. 비기능 요구사항

### 4.1 성능 (NFR-PERF)

| ID | 요구사항 | 목표값 |
|----|----------|--------|
| NFR-PERF-01 | 동시 접속자 수 | 최대 5명 |
| NFR-PERF-02 | 스트리밍 청크 크기 | 1MB |
| NFR-PERF-03 | 시청 위치 저장 주기 | 10초 |

### 4.2 용량 (NFR-CAP)

| ID | 요구사항 | 제한값 |
|----|----------|--------|
| NFR-CAP-01 | 최대 동영상 파일 크기 | 2GB |
| NFR-CAP-02 | 최대 등록 동영상 수 | 10개 |
| NFR-CAP-03 | 최대 등록 사용자 수 | 10명 |

### 4.3 가용성 (NFR-AVAIL)

| ID | 요구사항 | 목표값 |
|----|----------|--------|
| NFR-AVAIL-01 | 세션 만료 시간 | 30분 |
| NFR-AVAIL-02 | 서버 재시작 시 데이터 유지 | SQLite 파일 보존 |

### 4.4 보안 (NFR-SEC)

| ID | 요구사항 | 비고 |
|----|----------|------|
| NFR-SEC-01 | 비밀번호 해시화 저장 | 간단한 해시 함수 사용 |
| NFR-SEC-02 | HTTPS 통신 | 미지원 (HTTP 평문) |

---

## 5. 데이터베이스 설계

### 5.1 ERD

```
┌─────────────────┐       ┌─────────────────┐
│     users       │       │     videos      │
├─────────────────┤       ├─────────────────┤
│ id (PK)         │       │ id (PK)         │
│ username        │       │ title           │
│ nickname        │       │ description     │
│ password_hash   │       │ file_path       │
│ created_at      │       │ thumbnail_path  │
└────────┬────────┘       │ duration        │
         │                │ upload_date     │
         │                └────────┬────────┘
         │                         │
         │    ┌────────────────────┘
         │    │
         ▼    ▼
┌─────────────────────────┐
│    watch_history        │
├─────────────────────────┤
│ id (PK)                 │
│ user_id (FK)            │
│ video_id (FK)           │
│ last_position (seconds) │
│ updated_at              │
└─────────────────────────┘
```

### 5.2 테이블 정의

#### 5.2.1 users (사용자)

| 컬럼명 | 타입 | 제약조건 | 설명 |
|--------|------|----------|------|
| id | INTEGER | PRIMARY KEY, AUTOINCREMENT | 사용자 고유 ID |
| username | TEXT | NOT NULL, UNIQUE | 로그인 ID |
| nickname | TEXT | NOT NULL | 닉네임 |
| password_hash | TEXT | NOT NULL | 해시된 비밀번호 |
| created_at | DATETIME | DEFAULT CURRENT_TIMESTAMP | 생성일시 |

#### 5.2.2 videos (동영상)

| 컬럼명 | 타입 | 제약조건 | 설명 |
|--------|------|----------|------|
| id | INTEGER | PRIMARY KEY, AUTOINCREMENT | 동영상 고유 ID |
| title | TEXT | NOT NULL | 제목 |
| description | TEXT | | 설명 |
| file_path | TEXT | NOT NULL | 파일 경로 |
| thumbnail_path | TEXT | | 썸네일 경로 |
| duration | INTEGER | | 재생시간 (초) |
| upload_date | DATETIME | DEFAULT CURRENT_TIMESTAMP | 업로드일 |

#### 5.2.3 watch_history (시청 이력)

| 컬럼명 | 타입 | 제약조건 | 설명 |
|--------|------|----------|------|
| id | INTEGER | PRIMARY KEY, AUTOINCREMENT | 이력 고유 ID |
| user_id | INTEGER | FOREIGN KEY (users.id) | 사용자 ID |
| video_id | INTEGER | FOREIGN KEY (videos.id) | 동영상 ID |
| last_position | INTEGER | NOT NULL, DEFAULT 0 | 마지막 시청 위치 (초) |
| updated_at | DATETIME | DEFAULT CURRENT_TIMESTAMP | 갱신일시 |

---

## 6. 인터페이스 요구사항

### 6.1 네트워크 인터페이스

| 프로토콜 | 포트 | 용도 |
|----------|------|------|
| TCP (WebSocket) | 8080 | 브라우저 ↔ 서버 통신 |
| UDP (QUIC) | 8443 | 내부 스트리밍 처리 |

### 6.2 WebSocket 메시지 형식

#### 요청 (클라이언트 → 서버)

```json
{
  "type": "request_type",
  "session_id": "session_token",
  "data": { ... }
}
```

#### 응답 (서버 → 클라이언트)

```json
{
  "type": "response_type",
  "status": "success|error",
  "data": { ... }
}
```

### 6.3 주요 API

| Type | 설명 | 요청 데이터 | 응답 데이터 |
|------|------|-------------|-------------|
| login | 로그인 | username, password | session_id |
| logout | 로그아웃 | session_id | - |
| get_videos | 영상 목록 조회 | - | videos[] |
| get_video | 영상 상세 조회 | video_id | video info |
| stream_start | 스트리밍 시작 | video_id, position | - |
| stream_chunk | 청크 데이터 | - | chunk_data |
| save_position | 시청 위치 저장 | video_id, position | - |
| get_history | 시청 이력 조회 | - | history[] |

### 6.4 웹 UI 페이지

| 페이지 | 파일명 | 설명 |
|--------|--------|------|
| 로그인 | index.html | 기본 페이지, ID/PW 입력 폼 |
| 영상 목록 | videos.html | 영상 카드 목록, 썸네일 표시 |
| 영상 재생 | player.html | 비디오 플레이어, 컨트롤 UI |

---

## 7. 기술 스택

### 7.1 서버

| 구분 | 기술 | 비고 |
|------|------|------|
| 언어 | C | |
| 소켓 | POSIX BSD Socket | TCP + UDP |
| QUIC | 직접 구현 | UDP 소켓 기반 |
| 멀티스레딩 | pthread | |
| 데이터베이스 | SQLite3 | |
| 썸네일 추출 | FFmpeg CLI | system() 호출 |

### 7.2 클라이언트

| 구분 | 기술 |
|------|------|
| 마크업 | HTML5 |
| 스타일 | CSS3 |
| 스크립트 | JavaScript (Vanilla) |
| 통신 | WebSocket API |

### 7.3 인프라

| 구분 | 기술 |
|------|------|
| 컨테이너 | Docker |
| 베이스 이미지 | Ubuntu 22.04 |
| 볼륨 | 동영상 파일, SQLite DB |

### 7.4 사용 헤더 파일

```c
/* 소켓 프로그래밍 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

/* 표준 라이브러리 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* 멀티스레딩 */
#include <pthread.h>

/* 데이터베이스 */
#include <sqlite3.h>
```

---

## 8. 제약사항

### 8.1 기술적 제약

| 항목 | 제약 내용 |
|------|-----------|
| 플랫폼 | Linux (Docker) 환경만 지원 |
| 브라우저 | Chrome 우선 지원 |
| HTTPS | 미지원 (HTTP 평문 통신) |
| QUIC | 브라우저 직접 연결 불가 (WebSocket 브릿지 사용) |

### 8.2 운영 제약

| 항목 | 제약 내용 |
|------|-----------|
| 동시 접속 | 최대 5명 |
| 동영상 파일 | MP4 형식, 최대 2GB |
| 등록 영상 수 | 최대 10개 |
| 등록 사용자 | 최대 10명 |

---

## 9. 추후 개선사항

### 9.1 기능 개선

| 우선순위 | 개선 항목 | 설명 |
|----------|-----------|------|
| 높음 | 해상도 선택 기능 | 사용자가 재생 해상도를 선택할 수 있도록 함 |
| 중간 | 적응형 비트레이트 | 네트워크 상황에 따른 자동 품질 조절 |
| 중간 | 검색 기능 | 영상 제목/설명 기반 검색 |
| 낮음 | 시청 통계 | 인기 영상, 시청 시간 통계 |

### 9.2 기술 개선

| 우선순위 | 개선 항목 | 설명 |
|----------|-----------|------|
| 높음 | HTTPS 지원 | TLS 암호화 통신 |
| 중간 | HLS 스트리밍 | 표준 스트리밍 프로토콜 지원 |
| 중간 | 동시 접속 확장 | 100명 이상 지원 |
| 낮음 | 크로스 플랫폼 | Windows/macOS 네이티브 지원 |

---

## 부록

### A. 디렉토리 구조 (예상)

```
ott_quic/
├── docs/
│   └── SRS.md
├── src/
│   ├── main.c
│   ├── server/
│   │   ├── websocket.c
│   │   ├── websocket.h
│   │   ├── quic.c
│   │   ├── quic.h
│   │   ├── streaming.c
│   │   └── streaming.h
│   ├── auth/
│   │   ├── session.c
│   │   ├── session.h
│   │   ├── hash.c
│   │   └── hash.h
│   ├── db/
│   │   ├── database.c
│   │   └── database.h
│   └── utils/
│       ├── thumbnail.c
│       └── thumbnail.h
├── web/
│   ├── index.html
│   ├── videos.html
│   ├── player.html
│   ├── css/
│   │   └── style.css
│   └── js/
│       └── app.js
├── data/
│   ├── videos/
│   ├── thumbnails/
│   └── ott.db
├── tools/
│   └── admin.sh
├── Dockerfile
├── docker-compose.yml
├── Makefile
└── README.md
```

### B. Docker 구성 (예상)

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libsqlite3-dev \
    ffmpeg

WORKDIR /app
COPY . .
RUN make

EXPOSE 8080/tcp
EXPOSE 8443/udp

VOLUME ["/app/data/videos", "/app/data"]

CMD ["./ott_server"]
```

### C. 빌드 명령어 (예상)

```bash
# 로컬 빌드
make

# Docker 빌드
docker build -t ott_quic .

# Docker 실행
docker run -d \
  -p 8080:8080 \
  -p 8443:8443/udp \
  -v $(pwd)/data:/app/data \
  ott_quic
```

---

## 변경 이력

| 버전 | 날짜 | 변경 내용 | 작성자 |
|------|------|-----------|--------|
| 1.0.0 | 2025-11-29 | 최초 작성 | - |
