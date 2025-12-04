# Repository Guidelines

## Project Structure & Module Organization
- Core C sources live in `src/` (`auth/`, `db/`, `server/`, `utils/`, `main.c`); add new modules in their own subdirs, headers for APIs, `.c` for implementations.
- Tests in `tests/` mirror modules (e.g., `tests/quic_packet_test.c` for `src/server/quic_packet.c`).
- Build artifacts land in `build/`; local data in `data/` (git-ignored). Docs/specs in `docs/`; Docker assets in `Dockerfile`, `docker-compose.yml`, `nginx.conf`; frontend in `web/`.

## Build, Test, and Development Commands
- `make` — compile all sources to `build/ott_server` (C11, strict warnings).
- `make test` — build/run test bins under `build/tests/` (DB/server/WebSocket/QUIC/auth).
- `./build/ott_server` — run the server locally after building.
- `docker-compose up --build` / `docker-compose down` — full stack with Docker.
- Format/lint with `clang-format`/`clang-tidy` (Google C style if no config).

## Coding Style & Naming Conventions
- C11 with flags `-Wall -Wextra -Werror -pedantic -pthread`; avoid warning suppression.
- Headers expose types/prototypes/macros only; implementations stay in `.c`.
- Naming: snake_case for functions/vars, UpperCamelCase for structs/types, ALL_CAPS for macros/const globals.
- Comment the “why/constraints/edge cases”, especially around threads, network I/O, ownership, and lifetimes.
- Errors: return 0 on success, negative on failure; log as `[level][module] message`, no PII.

## Testing Guidelines
- Plain C test bins under `tests/`, built via Makefile; add `tests/<module>_test.c` for new modules.
- Cover boundaries/timeouts/error paths; keep tests deterministic and service-free when possible.
- Run `make test` before PRs; include new tests with new functionality.

## Commit & Pull Request Guidelines
- Commit messages in Korean, present/imperative, ~50 chars (e.g., “QUIC 핸드셰이크 응답 추가”); one topic per commit with code+tests+docs together.
- Sprint-end commits: add 3-line progress summary and tag (e.g., `[Sprint5]`).
- PRs: brief summary, linked issues, risks/timeouts, `make test` results, and doc updates (`docs/`, `README.md`, `codex.md`, this file). Attach logs/screenshots for networking changes; call out thread/socket ownership rules.

## Codex 규칙 요약 (필수 준수)
- 커뮤니케이션은 한국어로, 결정 변경 시 이유를 짧게 기록.
- 스레드/네트워크 코드: 오류 반환 체크, 타임아웃/취소 플래그 마련, 락/소켓 수명 명확화.
- 입력 검증 후 처리하고, 해시/인증은 솔트 포함 느린 해시 사용; 로그에는 민감 정보 제외.
- 새 경고 억제 시 근거 주석; 자동 포매팅 후 diff 확인. 새 의존성 추가 시 `README.md`/`Makefile` 갱신.
- 문서 변경 시 관련 문서(`docs/`, `codex.md`, `AGENTS.md`)를 함께 업데이트하고, 최소 예시와 에러 시나리오를 남긴다.
