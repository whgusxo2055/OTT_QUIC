## 1.1 프로젝트 소스
- https://github.com/whgusxo2055/OTT_QUIC.git

## 2.1 개발환경 및 컴파일 환경
- 언어/표준: C11, JavaScript(ES6), HTML/CSS
- 빌드: `make` (옵션 `TLS=1` 시 OpenSSL 링크), 산출물 `build/ott_server`
- 의존: SQLite3, OpenSSL(서버 TLS), FFmpeg(썸네일 추출, CLI 호출 전제), pthread
- 런타임: Docker Compose (Nginx TLS 종료 8443, 내부 백엔드 8080, QUIC 9443/UDP)

## 2.2 설계 명세서
- 네트워크 계층: Nginx가 TLS 종료(443/8443) 후 내부 HTTP(8080)로 프록시, QUIC 엔진은 9443/UDP 별도.
- 서버 구조: 멀티스레드 TCP 서버(HTTP/WebSocket) + QUIC 엔진(UDP). WebSocket으로 목록/스트림 제어, HTTP로 인증/업로드/API.
- 저장소: SQLite DB(user, video, watch history), 파일 스토리지(data/에 원본·썸네일·세그먼트).
- 프런트: 단일 페이지들(index/login/signup/watch/admin) + JS(fetch/WebSocket)로 API/WS 연동.
- 썸네일: 업로드 시 FFmpeg를 외부 도구로 호출해 대표 이미지 생성 후 파일 시스템에 저장, 메타는 DB에 기록.

## 2.3 요구사항 구현 명세 (충족 요약표)

| ID | 요구사항 요약 | 설명 | 구현 상태 |
| --- | --- | --- | --- |
| 1 | 상용 MP4 입력, 파일+DB 보관 | 업로드 후 `data/` 보관, 메타 DB 기록 | ✅ |
| 2 | FFmpeg 썸네일 추출 | 업로드 파이프라인에서 FFmpeg 호출로 대표 이미지 생성 | ✅ |
| 3 | 웹 UI, Index 기본, 로그인 지원 | `web/index.html` 기본, `login.html`/`signup.html` 제공 | ✅ |
| 4 | ID/PW 로그인, 성공 시 목록 | `/login` API, 성공 시 세션·로컬스토리지 저장 후 index 이동 | ✅ |
| 5 | 동영상 목록 표시(제목/썸네일/재생) | WebSocket으로 목록 수신 후 UI 렌더링, 썸네일 경로 노출 | ✅ |
| 6 | 선택 시 재생 | `watch.html`에서 WS 제어+스트림 세그먼트 수신 | ✅ |
| 7 | 시청 이력/이어보기 | DB에 시청 위치 저장, 재접속 시 이어보기/처음부터 선택 | ✅ |
| 8 | 특정 위치 재생 | `t` 파라미터 및 WS 명령으로 오프셋 시킹 지원 | ✅ |
| 9 | 고속 멀티유저(멀티스레드/QUIC) | pthread 기반 TCP, QUIC 엔진 스레드 별도, Nginx 프록시로 부하 분리 | ✅ |
| 10 | 네트워크 프로그래밍 기반 | TCP/UDP 소켓 직접 사용, TLS1.3 종단 분리(Nginx) | ✅ |

## 2.4 요구사항 세부 설명
- 입력/저장: 업로드된 MP4를 `data/`에 보관, DB에는 메타데이터(제목, 경로, 썸네일, 길이)를 기록.
- 썸네일: FFmpeg로 대표 프레임 추출 → `/data/thumbs/...` 저장 → 목록/히어로 섹션에 사용.
- UI: 로그인/회원가입/목록/시청/관리자 페이지로 구성. 기본 진입은 `index.html`, 로그인 페이지로 리다이렉트 흐름 제공.
- 인증: `/login` POST로 세션 발급, 쿠키+로컬스토리지 저장. `/signup`으로 신규 사용자 생성. 성공 시 목록 페이지로 이동.
- 목록/재생: WebSocket으로 비디오 리스트·추천·이어보기 정보를 받아 렌더링. 항목별 재생 버튼이 `watch.html`로 이동.
- 스트리밍: WebSocket으로 제어 메시지, MSE 기반 재생. QUIC 엔진이 패킷 처리(9443/UDP), TCP는 WS/HTTP 처리.
- 이어보기/추천: 시청 위치를 주기적으로 DB에 저장, 재로그인 시 이어보기/추천 섹션에서 노출.
- 시킹: `watch.html`에서 특정 시점부터 재생 요청 가능(쿼리 파라미터 `t` 및 WS 메시지로 오프셋 전달).
- 동시성: 멀티스레드 서버(accept + per-connection threads), QUIC는 별도 스레드 풀. Nginx가 TLS 종료·정적 서빙·프록시를 담당해 앱 부하를 분리.
