# OTT_QUIC

QUIC 기반 OTT 스트리밍 학습용 프로젝트. [docs/SRS.md](docs/SRS.md)와 [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md)에 맞춰 개발됩니다.

## 현재 상태
- 인증/세션: 로그인/회원가입(닉네임/아이디/비밀번호), 세션/관리자 권한 검사 (`/login`, `/signup`, `/logout`)
- DB/파일: SQLite 스키마·CRUD (`src/db/database.*`), 업로드/썸네일/세그먼트 저장 (`src/server/upload.*`)
- 서버/프로토콜: 멀티스레드 TLS HTTP + WebSocket, QUIC 엔진/패킷 모듈
- 프런트: 시청/이어보기/검색/정렬, 관리자 업로드·영상 메타 수정/삭제, 닉네임 표시
- 정적 자원: nginx 컨테이너에서 `/data` 서빙(8080) - 썸네일 등은 http://localhost:8080/ 경로 사용

## 프로젝트 구조
```
ott_quic/
├── docs/
├── src/
├── web/
├── data/
├── tools/
├── Dockerfile
├── docker-compose.yml
├── Makefile
└── README.md
```

## 빌드 방법
```bash
make        # build/ott_server 생성
./build/ott_server
make test   # DB/서버/WebSocket 단위 테스트 실행
# OpenSSL 헤더/라이브러리 설치 시 TLS 내장 빌드는 TLS=1 플래그로 실행하세요.
# 예: make TLS=1
```

## Docker 사용
```bash
# 이미지 빌드 및 컨테이너 실행
docker-compose up --build

# 종료
docker-compose down

# 정적 자원 확인은 https://localhost:8443/data/... (80/8080은 8443으로 리다이렉트)
```

**중요:** Docker 환경에서는 반드시 **HTTPS(8443)**와 **WSS(WebSocket Secure)**를 사용해야 합니다.
- ✅ 웹 접속: https://localhost:8443 (nginx TLS 종료)
- ✅ API/WebSocket: wss://localhost:8443 (nginx → 내부 8080 백엔드 프록시)
- ❌ http://localhost:8443 (평문 HTTP → SSL 에러 발생)

## 웹/관리자 사용
- 로그인: `/web/login.html` → 세션/닉네임 저장 후 홈 이동
- 회원가입: `/web/signup.html` (닉네임/아이디/비밀번호)
- 관리자: `admin.html`(업로드/영상 메타 수정·삭제), 홈 우측 관리자 버튼은 `ott_is_admin` 로컬스토리지에 따라 표시
- 썸네일/정적 자원도 동일 오리진(https://localhost:8443)에서 서빙

## 개발 메모
- C 표준: C11, 기본 포트: 외부 TLS 8443(nginx), 내부 HTTP 백엔드 8080, UDP 9443(QUIC)
- 데이터 파일은 `data/` 디렉터리에 저장되며 Git에서 제외됩니다. 인증서 `certs/`도 Git 무시 대상입니다.
- 일부 테스트(`server_test`, `quic_engine_test`)는 포트 바인딩이 불가한 환경에서 skip될 수 있습니다.
- **SSL 에러 방지:** HTTPS 포트(8443)에는 반드시 HTTPS/WSS 요청만 보내야 합니다. 평문 HTTP 요청 시 SSL 핸드셰이크 실패 에러가 발생하지만 서버는 정상 동작합니다.
