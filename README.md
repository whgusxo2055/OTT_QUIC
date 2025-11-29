# OTT_QUIC

QUIC 기반 OTT 스트리밍 학습용 프로젝트. [docs/SRS.md](docs/SRS.md)와 [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md)에 맞춰 개발됩니다.

## 현재 상태 (Sprint 1)
- 기본 디렉터리 구조 생성
- C 진입점(`src/main.c`) 및 빌드 시스템 준비
- Docker/Docker Compose 환경 정의

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
