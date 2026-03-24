CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcurl -lpthread -lcjson
TARGET  = registrator
SRCS    = registrator.c

IMAGE   = biteagle/consul-registrator
VERSION = $(shell cat VERSION 2>/dev/null || echo 0.0.0)

.PHONY: all clean docker push release patch minor major

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

# Build Docker image, tagged with version + latest
docker:
	docker build -t $(IMAGE):$(VERSION) -t $(IMAGE):latest .

# Push both tags to registry (build first if not built)
push:
	docker push $(IMAGE):$(VERSION)
	docker push $(IMAGE):latest

# Bump patch (0.1.0 → 0.1.1), build, and push
patch:
	@awk -F. '{printf "%d.%d.%d\n", $$1, $$2, $$3+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) push

# Bump minor (0.1.0 → 0.2.0), build, and push
minor:
	@awk -F. '{printf "%d.%d.0\n", $$1, $$2+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) push

# Bump major (0.1.0 → 1.0.0), build, and push
major:
	@awk -F. '{printf "%d.0.0\n", $$1+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) push

# Alias: bump patch + build + push (most common)
release: patch
