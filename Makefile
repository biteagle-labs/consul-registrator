CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcurl -lpthread -lcjson
TARGET  = registrator
SRCS    = registrator.c

IMAGE   = biteagle/consul-registrator
VERSION = $(strip $(file < VERSION))

.PHONY: all clean docker push release patch minor major \
       gateway-up gateway-down gateway-reload gateway-rebuild gateway-logs

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

# Build Docker image, tagged with version + latest
docker:
	docker build -t $(IMAGE):$(VERSION) -t $(IMAGE):latest .

# Push both tags to registry
push:
	docker push $(IMAGE):$(VERSION)
	docker push $(IMAGE):latest

# Bump patch (0.1.0 → 0.1.1), build, and push
patch:
	@awk -F. '{printf "%d.%d.%d\n", $$1, $$2, $$3+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) docker push

# Bump minor (0.1.0 → 0.2.0), build, and push
minor:
	@awk -F. '{printf "%d.%d.0\n", $$1, $$2+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) docker push

# Bump major (0.1.0 → 1.0.0), build, and push
major:
	@awk -F. '{printf "%d.0.0\n", $$1+1}' VERSION > VERSION.tmp && mv VERSION.tmp VERSION
	@echo "Version: $$(cat VERSION)"
	$(MAKE) docker push

# Alias: bump patch + build + push (most common)
release: patch

# --- OpenResty gateway ---

GATEWAY_DIR = openresty

# Hot reload: pick up lua/nginx.conf changes without restart
gateway-reload:
	docker exec consul-gateway nginx -s reload

# Cold restart: recreate container (picks up env/volume changes)
gateway-down:
	cd $(GATEWAY_DIR) && docker compose down

gateway-up:
	cd $(GATEWAY_DIR) && docker compose up -d

# Full rebuild: rebuild image + recreate container
gateway-rebuild:
	cd $(GATEWAY_DIR) && docker compose up -d --build

gateway-logs:
	cd $(GATEWAY_DIR) && docker compose logs -f --tail=50
