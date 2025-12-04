# 📅 **OTT_QUIC 개발 계획서 (planv3.md)**

### Version 3.1

### Last Updated: 2025-12-06

---

# 1. 🎯 프로젝트 개요

본 문서는 *OTT_QUIC v3.1 SRS*를 기반으로 실제 개발을 수행하기 위한 전체 개발 계획을 정의한다.
QUIC/TLS 전송 계층, HTTPS/WSS 기반 사용자 기능, 관리자 UI, 스트리밍 엔진 구축까지의 흐름을 개발 전·중·후 단계로 나누어 정리한다.

---

# 2. 🧭 개발 전체 로드맵

## **2.1 상위 단계 로드맵**

```
[요구사항 분석]
      ↓
[아키텍처/DB 설계]
      ↓
[환경 구축 (Docker + OpenSSL 3.2 빌드)]
      ↓
[기능 개발]
  · 인증/세션
  · 관리자 페이지
  · 영상 목록/재생 UI
  · QUIC 엔진
  · TLS 연동(OpenSSL)
  · 스트리밍 엔진
      ↓
[통합 테스트]
      ↓
[성능 테스트]
      ↓
[배포/운영]
```

---

# 3. 🧱 개발 전 단계 (Pre-Development)

## **3.1 요구사항 분석 점검**

* SRS v3.1 요구사항 최종 확정
* 외부/내부 포트 정책 확정:

  * TCP 8443 = HTTPS/WSS
  * UDP 9443 = QUIC 전용
* 기술 스택 확정

  * C, SQLite, FFmpeg, OpenSSL 3.2+, Docker

**결정 필요사항**

* 영상 파일 저장 용량 정책(2GB 제한 유지?)
* 관리자 권한 구조 단일 role 또는 RBAC 확장 여부
* 스트리밍 청크 크기(현재 1MB 유지 여부)

---

## **3.2 시스템 아키텍처 설계**

### 작업 항목

* 전체 아키텍처 다이어그램 확정
* QUIC 엔진 내부 모듈 설계

  * 패킷 구조
  * ACK 처리
  * 스트림 관리
  * 재전송 타이머 구조
  * 혼잡 제어 알고리즘(cubic, reno or simple)
* TLS 핸드셰이크 절차 설계

  * OpenSSL BIO 연결 구조
  * CRYPTO 프레임 전달 방식
  * 키 교체 처리

### 산출물

* `/docs/architecture.md`
* `/docs/quic_flow.md` (패킷/스트림/핸드셰이크 흐름)

---

## **3.3 DB 스키마 설계**

### 작업 항목

* users / videos / sessions / watch_history 정의
* 관리자 role 추가 (`role='admin'`)
* 세션 만료 관리 전략 → cron 스레드 or lazy clean-up

### 산출물

* `/docs/db_schema.sql`
* ERD 이미지 (`docs/images/erd.png`)

---

## **3.4 개발 환경 구축**

### Docker 이미지 구성

1. Ubuntu 22.04 기반
2. OpenSSL 3.2+ 소스 컴파일
3. FFmpeg 설치
4. gcc/make 빌드 환경
5. 서버 코드 컴파일

### 산출물

* `Dockerfile`
* `docker-compose.yml`
* `/docs/build_env.md`

---

# 4. ⚙️ 개발 단계 (Development Phase)

---

## **4.1 백엔드 개발**

### **(1) 공통 유틸 개발**

| 구성 요소         | 설명                   |
| ------------- | -------------------- |
| logger        | 로그 레벨(INFO/ERROR) 관리 |
| config parser | 포트/경로/인증서 설정 로딩      |
| error handler | 에러 코드/메시지 통합 처리      |

완료 후 `/src/utils/` 폴더 구성.

---

## **4.2 인증/세션 개발 (HTTPS/WSS)**

### 작업 항목

* bcrypt 기반 password hash
* `/api/login`, `/api/logout` REST API 구현
* `sessions` 테이블 연동
* 세션 만료 타임아웃 관리
* HTTPS 기반 서버 초기 구성(8443)

### 산출물

* `/src/auth/session.c`
* `/src/http/http_server.c`

---

## **4.3 관리자 기능 개발**

### 기능

* 영상 업로드

  * 파일 업로드 처리
  * 파일 시스템 내 저장
  * FFmpeg로 썸네일 생성
* 사용자 추가/삭제
* 영상 삭제

### 파일 구성

* `/web/admin.html`
* `/web/js/admin.js`
* `/src/http/admin_api.c`

---

## **4.4 QUIC 엔진 개발**

### 개발 순서

1. **UDP 소켓 서버 구축**

   * 포트 9443
   * non-blocking I/O + epoll
2. **패킷 구조 정의**
3. **연결 관리(Connection ID, state machine)**
4. **스트림 관리(Stream ID, flow control)**
5. **ACK/손실 감지**
6. **재전송 로직**
7. **혼잡 제어(minimal version)**

### 산출물

* `/src/quic/quic.c`
* `/src/quic/quic_packet.c`
* `/src/quic/quic_stream.c`

---

## **4.5 TLS 연동(OpenSSL 3.2+)**

### 작업 항목

* TLS 1.3 핸드셰이크 처리
* OpenSSL BIO <-> QUIC CRYPTO 프레임 브리지
* key export API로 암·복호화 키 획득
* Initial / Handshake / 1-RTT 키 처리
* Key update 처리(optional)

### 산출물

* `/src/quic/quic_tls.c`
* `/docs/tls_handshake_flow.md`

---

## **4.6 스트리밍 엔진 개발**

### 작업 항목

* 영상 파일 1MB 단위 청크 분할
* QUIC 스트림으로 전송하는 Producer 구조
* 시킹 처리 (특정 byte offset 계산)
* 브라우저로 WebSocket 프레임 전달

### 산출물

* `/src/streaming/streaming.c`
* `/web/player.html`

---

## **4.7 프론트엔드 개발**

### 페이지 구성

| 페이지   | 파일          | 설명             |
| ----- | ----------- | -------------- |
| 로그인   | index.html  | 사용자 인증         |
| 영상 목록 | videos.html | 썸네일 기반 목록      |
| 영상 재생 | player.html | WSS 기반 스트리밍 재생 |
| 관리자   | admin.html  | 영상/사용자 관리      |

### 공통 요소

* WebSocket 연결 관리
* 세션 유지
* 재생 위치 자동 저장(10초 주기)

---

# 5. 🔍 개발 후 단계 (Post-Development)

---

## **5.1 통합 테스트**

### 항목

* API 기능 테스트
* 브라우저 스트리밍 재생 테스트
* QUIC 핸드셰이크 오류 검증
* 관리자 기능 검증
* DB 일관성 확인

### 도구

* curl
* Postman
* ffprobe
* Wireshark(QUIC 패킷 확인)

---

## **5.2 성능 테스트**

| 테스트 항목           | 목표                |
| ---------------- | ----------------- |
| 동시 5명 스트리밍       | 모든 사용자 재생 지연 ≤ 1초 |
| QUIC 패킷 손실 시 재전송 | 정상적으로 버퍼링 회복      |
| 관리자 페이지 응답 속도    | 300ms 이하          |

---

## **5.3 보안 점검**

* HTTPS 인증서 검증
* TLS 1.3 강제 적용
* 세션 탈취 방지(HSTS, Secure Cookie 등 적용 여부 검토)
* 관리자 기능 접근 제어 점검

---

## **5.4 배포**

### Docker Compose 기반 배포

* Web + QUIC 서버 단일 이미지
* volume:

  * `/app/data/videos`
  * `/app/data/ott.db`
  * `/app/certs`

예상 명령어:

```bash
docker-compose up -d --build
```

---

## **5.5 운영 계획**

* 로그 로테이션
* DB 백업 정책
* 영상 파일 관리 정책
* 성능 모니터링(grafana/prometheus optional)

---

# 6. 📌 일정 계획 (예시)

| 단계       | 기간   | 담당     | 산출물                       |
| -------- | ---- | ------ | ------------------------- |
| 분석/설계    | 1주   | FE+BE  | SRS, 아키텍처 문서              |
| 환경 구축    | 1주   | BE     | Dockerfile, OpenSSL 빌드    |
| 인증/세션    | 1주   | BE     | session.c, http_server.c  |
| 관리자 기능   | 1주   | FE+BE  | admin.html, admin API     |
| QUIC 엔진  | 3주   | BE     | quic.c, quic_packet.c     |
| TLS 연동   | 2주   | BE     | quic_tls.c                |
| 스트리밍 엔진  | 2주   | BE     | streaming.c               |
| 프론트엔드 전체 | 1.5주 | FE     | index/videos/player/admin |
| 통합 테스트   | 1주   | QA     | test report               |
| 배포/운영    | 0.5주 | DevOps | docker-compose            |

총 예상 개발 기간: **~14주**

---

# 7. 🗂 폴더 구조 제안

```
ott_quic/
├── docs/
│   ├── SRS.md
│   ├── plan.md
│   ├── architecture.md
│   ├── quic_flow.md
│   └── db_schema.sql
├── src/
│   ├── auth/
│   ├── http/
│   ├── quic/
│   ├── streaming/
│   ├── utils/
│   └── main.c
├── web/
│   ├── index.html
│   ├── videos.html
│   ├── player.html
│   └── admin.html
├── data/
├── certs/
├── Dockerfile
└── docker-compose.yml
```

---