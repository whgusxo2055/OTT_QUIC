FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        gcc \
        libsqlite3-dev \
        libssl-dev \
        openssl \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# 기본적으로 TLS=1로 빌드하여 HTTPS/WSS 지원을 켭니다.
RUN rm -rf build && make TLS=1

EXPOSE 8443/tcp
EXPOSE 9443/udp

CMD ["./build/ott_server"]
