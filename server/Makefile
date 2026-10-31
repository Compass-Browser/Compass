GIT_VERSION := $(shell git describe --abbrev=8 --dirty --always --tags)
GIT_COMMIT := $(shell git rev-parse --short HEAD)
BUILD_TIME := $(shell date +%s)

.PHONY: all build test lint clean docker

all: lint test build

repath-proto:
	find ./schema/protobuf/sync_pb | grep "\.proto$\" | xargs sed -i 's/import \"components\/sync\/protocol\//import \"/g'
	find ./schema/protobuf/sync_pb | grep "\.proto$\" | xargs sed -i 's/import \"brave\/components\/sync\/protocol\//import \"/g'

proto-go-module:
	find ./schema/protobuf/sync_pb | grep "\.proto$\" | xargs sed -i 's/syntax = \"proto2\";/syntax = \"proto2\";\n\noption go_package = \"\.\/sync_pb\";/'

protobuf:
	protoc -I schema/protobuf/sync_pb/ schema/protobuf/sync_pb/*.proto --go_out=schema/protobuf/

build:
	CGO_ENABLED=0 go build -trimpath -o compass-sync-server .

test:
	go test ./...

lint:
	docker run -t --rm -v "$$(pwd):/app" -w /app golangci/golangci-lint golangci-lint run -v

clean:
	rm -f compass-sync-server

docker:
	docker build --build-arg COMMIT=$(GIT_COMMIT) --build-arg VERSION=$(GIT_VERSION) \
		--build-arg BUILD_TIME=$(BUILD_TIME) -t compass-sync-server .

instrumented:
	gowrap gen -p github.com/brave/go-sync/datastore -i Datastore -t ./.prom-gowrap.tmpl -o ./datastore/instrumented_datastore.go
