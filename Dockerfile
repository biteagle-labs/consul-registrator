# ─── 构建阶段 ────────────────────────────────────────────────────────────────
FROM alpine:latest AS builder

RUN apk add --no-cache gcc musl-dev make curl-dev cjson-dev

WORKDIR /src
COPY registrator.c Makefile ./
RUN make

# ─── 运行阶段 ────────────────────────────────────────────────────────────────
FROM alpine:latest

RUN apk add --no-cache libcurl cjson

COPY --from=builder /src/registrator /usr/local/bin/registrator

ENTRYPOINT ["/usr/local/bin/registrator"]
