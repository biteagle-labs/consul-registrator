# ─── Build stage ─────────────────────────────────────────────────────────────
FROM alpine:latest AS builder

RUN apk add --no-cache gcc musl-dev make curl-dev cjson-dev

WORKDIR /src
COPY registrator.c Makefile ./
RUN make

# ─── Runtime stage ───────────────────────────────────────────────────────────
FROM alpine:latest

RUN apk add --no-cache libcurl cjson

COPY --from=builder /src/registrator /usr/local/bin/registrator

ENTRYPOINT ["/usr/local/bin/registrator"]
