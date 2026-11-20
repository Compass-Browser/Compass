# Compass sync server: one static binary (pure-Go SQLite, no cgo) on a
# distroless base, running as non-root, with its database in /data.
# The builder runs on the build machine's platform and cross-compiles for the
# target one (linux/amd64 and linux/arm64 images, .github/workflows/server.yml).
FROM --platform=$BUILDPLATFORM public.ecr.aws/docker/library/golang:1.27 AS builder

ARG TARGETOS=linux
ARG TARGETARCH
ARG VERSION
ARG BUILD_TIME
ARG COMMIT

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .

RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath \
    -ldflags "-s -w -X github.com/brave/go-sync/server.version=${VERSION} -X github.com/brave/go-sync/server.buildTime=${BUILD_TIME} -X github.com/brave/go-sync/server.commit=${COMMIT}" \
    -o /out/compass-sync-server . \
 && mkdir -p /out/data

FROM gcr.io/distroless/static-debian12:nonroot AS artifact
LABEL org.opencontainers.image.source="https://github.com/Compass-Browser/Compass" \
      org.opencontainers.image.description="Compass sync server" \
      org.opencontainers.image.licenses="MPL-2.0"
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
