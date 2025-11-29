# OTT_QUIC

QUIC 기반 OTT 스트리밍 학습용 프로젝트. [docs/SRS.md](docs/SRS.md)와 [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md)에 맞춰 개발됩니다.

## 현재 상태 (Sprint 4)
- SQLite 기반 데이터베이스 모듈(`src/db/database.*`) 및 스키마/CRUD 구현
- 비밀번호 해시 유틸(`src/auth/hash.*`)과 간단한 단위 테스트(`tests/db_test.c`)
- 멀티스레드 TCP 서버(`src/server/server.*`)와 WebSocket 핸드셰이크/프레임 처리(`src/server/http.*`, `src/server/websocket.*`)
- 서버/웹소켓 테스트(`tests/server_test.c`, `tests/websocket_utils_test.c`)
- 기본 실행 파일과 Docker/Docker Compose 환경 유지

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
```

## Docker 사용
```bash
# 이미지 빌드 및 컨테이너 실행
docker-compose up --build

# 종료
docker-compose down
```

## 개발 메모
- C 표준: C11
- 추후 모듈은 `src/server`, `src/auth`, `src/db`, `src/utils` 하위에 추가합니다.
- 데이터 파일은 `data/` 디렉터리에 저장되며 Git에서 제외됩니다.
