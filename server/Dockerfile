# Compass sync server: one static binary (pure-Go SQLite, no cgo) on a
# distroless base, running as non-root, with its database in /data.
FROM public.ecr.aws/docker/library/golang:1.27 AS builder

ARG VERSION
ARG BUILD_TIME
ARG COMMIT

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .

RUN CGO_ENABLED=0 GOOS=linux go build -trimpath \
    -ldflags "-s -w -X github.com/brave/go-sync/server.version=${VERSION} -X github.com/brave/go-sync/server.buildTime=${BUILD_TIME} -X github.com/brave/go-sync/server.commit=${COMMIT}" \
    -o /out/compass-sync-server . \
 && mkdir -p /out/data

FROM gcr.io/distroless/static-debian12:nonroot AS artifact
COPY --from=builder /out/compass-sync-server /compass-sync-server
# Owned by the nonroot user (65532), so a new volume on /data is writable.
COPY --from=builder --chown=65532:65532 /out/data /data

USER 65532:65532
ENV COMPASS_SYNC_DB=/data/compass-sync.db \
    COMPASS_SYNC_ADDR=:8295
VOLUME /data
EXPOSE 8295
HEALTHCHECK --interval=30s --timeout=5s CMD ["/compass-sync-server", "healthcheck"]
ENTRYPOINT ["/compass-sync-server"]
