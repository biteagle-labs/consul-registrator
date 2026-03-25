# Consul Registrator

English | [中文](README_CN.md)

Lightweight Docker service registrator for Consul, written in C. Watches Docker container start/stop events and automatically registers/deregisters services with a Consul agent.

Built on Alpine Linux with libcurl and cJSON. Single binary, minimal footprint.

## Features

- **Opt-in mode** — only containers with `CONSUL_LISTEN_ENABLE=true` are registered
- **Dual-channel config** — reads from container environment variables first, then labels
- **Auto port detection** — `CONSUL_SERVICE_PORT` > HostPort mapping > `expose:` > EXPOSE > per-port labels; containers with no detectable port are skipped
- **TCP health check** — auto-generates a Consul TCP check targeting `container_ip:container_port`
- **Smart address resolution** — uses host IP for port-mapped containers, container IP for bridge/custom networks
- **POD_IP mode** — force registration with container IP + internal port (global or per-port)
- **Per-port service config** — register multiple services per container with `CONSUL_SERVICE_<port>_NAME`
- **Change-only logging** — resync only logs on state changes, reducing noise
- **Periodic resync** — full sync on interval to catch missed events and clean orphans
- **Three-thread architecture** — event stream + event processor + resync, no blocking

## Quick Start

### docker-compose.yaml (Recommended)

```yaml
services:
  registrator:
    image: biteagle/consul-registrator:latest
    container_name: registrator
    network_mode: host
    restart: always
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock:ro
    environment:
      - REGISTRATOR_TOKEN=${REGISTRATOR_TOKEN}
      - CONSUL_ADDR=${CONSUL_ADDR:-http://localhost:8500}
      - RESYNC_INTERVAL=${RESYNC_INTERVAL:-30}
      - ADVERTISE_ADDR=${ADVERTISE_ADDR:-}
      - LOG_LEVEL=${LOG_LEVEL:-info}
      - HOSTNAME_OVERRIDE=${HOSTNAME_OVERRIDE:-}
    logging:
      driver: json-file
      options:
        max-size: "10m"
        max-file: "3"
```

```bash
# Set your Consul ACL token
export REGISTRATOR_TOKEN=your-consul-acl-token

# Start
docker compose up -d

# View logs
docker compose logs -f registrator
```

### docker run

```bash
docker run -d \
  --name registrator \
  --network host \
  --restart always \
  -v /var/run/docker.sock:/var/run/docker.sock:ro \
  -e REGISTRATOR_TOKEN=your-consul-acl-token \
  -e CONSUL_ADDR=http://localhost:8500 \
  biteagle/consul-registrator:latest
```

## Registering a Service

Add `CONSUL_LISTEN_ENABLE=true` to your container via environment variable or label:

```bash
# Via environment variable
docker run -d -p 8080:80 \
  -e CONSUL_LISTEN_ENABLE=true \
  -e CONSUL_SERVICE_NAME=my-web \
  -e CONSUL_SERVICE_TAGS=v1,production \
  --name my-web nginx:alpine

# Via labels
docker run -d -p 8080:80 \
  -l CONSUL_LISTEN_ENABLE=true \
  -l CONSUL_SERVICE_NAME=my-web \
  -l CONSUL_SERVICE_TAGS=v1,production \
  --name my-web nginx:alpine
```

Containers **without** `CONSUL_LISTEN_ENABLE=true` are ignored.

## Container Configuration

Set via container environment variables (`-e`) or labels (`-l`). Environment variables take priority.

| Key | Description | Default |
|-----|-------------|---------|
| `CONSUL_LISTEN_ENABLE` | Set to `true` to enable registration (required) | `false` |
| `CONSUL_SERVICE_NAME` | Service name in Consul | Image name |
| `CONSUL_SERVICE_TAGS` | Comma-separated tags | _(empty)_ |
| `CONSUL_SERVICE_PORT` | Override auto-detected port (internal/container port) | Auto-detect |
| `CONSUL_SERVICE_CHECK_INTERVAL` | TCP health check interval | `10s` |
| `CONSUL_SERVICE_POD_IP` | Set to `false` to use host IP + host port for port-mapped containers | `true` |
| `CONSUL_SERVICE_<port>_NAME` | Service name for a specific port | Same as above |
| `CONSUL_SERVICE_<port>_TAGS` | Tags for a specific port | Same as above |
| `CONSUL_SERVICE_<port>_POD_IP` | Per-port POD_IP override | `true` |

### Port Detection Priority

1. `CONSUL_SERVICE_PORT` environment variable / label
2. Docker port mapping (`HostPort`)
3. `NetworkSettings.Ports` keys (catches compose `expose:` and other Docker-known ports)
4. `EXPOSE` directive from Dockerfile
5. `CONSUL_SERVICE_<port>_*` labels / env vars (per-port labels implicitly declare ports)

Containers with no detectable port are **skipped** (not registered). For docker-compose services without explicit port declarations, you can add `expose:` in the compose file, `EXPOSE` in the Dockerfile, or use per-port labels like `CONSUL_SERVICE_8080_NAME=myapp` to declare ports directly.

### Health Check

A TCP health check is automatically created for each registered service:
- Targets `container_ip:container_port` when the container IP is available (preferred, since the Consul agent can reach Docker bridge networks)
- Falls back to `service_address:service_port` otherwise
- Default interval: `10s` (configurable via `CONSUL_SERVICE_CHECK_INTERVAL`)

### Address Resolution

The service address registered in Consul is determined automatically:

| Scenario | Address | Port |
|----------|---------|------|
| Container with `network_mode: host` | Host IP | Container port |
| Non-host container (default, `POD_IP=true`) | Container IP | Container port |
| Non-host + `CONSUL_SERVICE_POD_IP=false` + port mapping | Host IP | Host port |
| Non-host + `CONSUL_SERVICE_POD_IP=false` + EXPOSE only | Container IP | Container port |

Host IP detection priority: `ADVERTISE_ADDR` env > `getaddrinfo(hostname)` > outbound IP via UDP probe.

## Registrator Configuration

Environment variables for the registrator container itself:

| Variable | Description | Default |
|----------|-------------|---------|
| `REGISTRATOR_TOKEN` | Consul ACL token | _(none)_ |
| `CONSUL_ADDR` | Consul HTTP address | `http://localhost:8500` |
| `RESYNC_INTERVAL` | Full sync interval in seconds | `30` |
| `ADVERTISE_ADDR` | Override auto-detected host IP for Consul registration | Auto-detect |
| `HOSTNAME_OVERRIDE` | Override system hostname used in service IDs | System hostname |
| `LOG_LEVEL` | Set to `debug` for verbose output | `info` |

## Architecture

### Thread Model

The registrator runs three threads that never block each other:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        registrator (host network)                   │
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌───────────────────────┐  │
│  │ Main Thread   │    │ Event        │    │ Resync Thread         │  │
│  │              │    │ Processor    │    │                       │  │
│  │ curl SSE     │    │              │    │ sleep(N)              │  │
│  │ /events ─────┼──► │ dequeue ───► │    │   │                   │  │
│  │ (long conn)  │ eq │ register /   │    │   ▼                   │  │
│  │              │    │ deregister   │    │ sync_all()            │  │
│  └──────┬───────┘    └──────┬───────┘    │  ├─ register_container│  │
│         │                   │            │  └─ cleanup_orphans   │  │
│         │                   │            └───────────┬───────────┘  │
│         │ unix sock         │ HTTP                   │ HTTP         │
└─────────┼───────────────────┼───────────────────────┼──────────────┘
          │                   │                       │
          ▼                   ▼                       ▼
   Docker daemon         Consul agent           Consul agent
   /events stream        /v1/agent/service/*     /v1/agent/service/*
```

- **Main thread** — holds a long-lived SSE connection to Docker `/events`. Incoming JSON lines are parsed in the curl write callback and pushed onto a lock-free event queue. No Consul I/O happens here.
- **Event processor thread** — waits on the event queue (`pthread_cond_wait`). On `start` → `register_container()`; on `stop`/`die` → `deregister_container()`.
- **Resync thread** — sleeps for `RESYNC_INTERVAL` seconds, then runs a full `sync_all()`: re-registers all enabled containers and cleans up orphaned services whose containers no longer exist.

### Startup Sequence

```
main()
 ├─ Load env config (CONSUL_ADDR, REGISTRATOR_TOKEN, ...)
 ├─ resolve_host_ip()
 │    ├─ 1. ADVERTISE_ADDR env var
 │    ├─ 2. getaddrinfo(hostname), skip 127.x.x.x
 │    ├─ 3. UDP connect trick (outbound IP via kernel routing)
 │    └─ 4. Fallback to hostname string
 ├─ preflight_check()
 │    ├─ Docker API: GET /info
 │    └─ Consul API: GET /v1/status/leader
 ├─ sync_all()  ← initial full sync
 ├─ Start resync_thread
 ├─ Start event_processor_thread
 └─ watch_events()  ← blocks on main thread
```

### Registration Flow

For each container with `CONSUL_LISTEN_ENABLE=true`:

```
register_container(container_id)
 │
 ├─ Docker API: GET /containers/{id}/json
 │
 ├─ Gate: CONSUL_LISTEN_ENABLE != "true" → skip (return 0)
 │
 ├─ detect_ports()
 │    ├─ 1. CONSUL_SERVICE_PORT → look up HostPort mapping
 │    ├─ 2. NetworkSettings.Ports HostPort bindings (deduplicated)
 │    ├─ 3. NetworkSettings.Ports keys (expose: / Docker-known ports)
 │    ├─ 4. Config.ExposedPorts (EXPOSE)
 │    ├─ 5. CONSUL_SERVICE_<port>_* labels/env vars (implicit port declaration)
 │    └─ 6. No port found → port_count=0, skip registration
 │
 ├─ Gate: port_count == 0 → skip (return 1, still counts as enabled)
 │
 ├─ Detect network mode
 │    └─ container_ip empty → is_host_network = true
 │
 └─ For each port:
      │
      ├─ Read per-port config (CONSUL_SERVICE_{port}_NAME / _TAGS / _POD_IP)
      │
      ├─ Address resolution (4-level priority):
      │    ├─ host network      → host_ip + container_port  (POD_IP ignored)
      │    ├─ POD_IP=true (def) → container_ip + container_port
      │    ├─ POD_IP=false + mapped → host_ip + host_port
      │    └─ POD_IP=false + EXPOSE → container_ip + container_port
      │
      ├─ TCP health check target:
      │    ├─ container_ip available → container_ip:container_port
      │    └─ otherwise             → service_addr:service_port
      │
      └─ PUT /v1/agent/service/register
           { ID, Name, Address, Port, Tags, Meta, Check }
```

### Deregistration & Orphan Cleanup

```
deregister_container(container_id)         cleanup_orphans(valid_ids)
 │                                          │
 ├─ GET /v1/agent/services                  ├─ GET /v1/agent/services
 │                                          │
 ├─ Match by:                               ├─ Filter: Meta.registrator == "self-hosted"
 │   1. Meta.container_id                   │
 │   2. service_id prefix (fallback)        ├─ Meta.container_id not in valid_ids?
 │                                          │   → container gone, deregister
 └─ PUT /v1/agent/service/deregister/{id}   │
                                            └─ PUT /v1/agent/service/deregister/{id}
```

## License

MIT
