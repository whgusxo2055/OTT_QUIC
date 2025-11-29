FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        gcc \
        libsqlite3-dev \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN rm -rf build && make

EXPOSE 8080/tcp
EXPOSE 8443/udp

CMD ["./build/ott_server"]
