# Consul Registrator

English | [中文](README_CN.md)

Lightweight Docker service registrator for Consul, written in C. Watches Docker container start/stop events and automatically registers/deregisters services with a Consul agent.

Built on Alpine Linux with libcurl and cJSON. Single binary, minimal footprint.

> This repository also ships an optional **OpenResty gateway** (`openresty/`)
> that consumes what the registrator publishes: a Consul-KV-driven dynamic
> router with hostname- and path-based routing. See [Gateway (OpenResty)](#gateway-openresty).

## Features

- **Opt-in mode** — only containers with `CONSUL_LISTEN_ENABLE=true` are registered
- **Dual-channel config** — reads from container environment variables first, then labels
- **Auto default port selection** — `CONSUL_SERVICE_PORT` > smallest HostPort-mapped container port > smallest Docker-known port > smallest EXPOSE port
- **TCP health check** — auto-generates a Consul TCP check targeting `container_ip:container_port`
- **Smart address resolution** — uses host IP for port-mapped containers, container IP for bridge/custom networks
- **POD_IP mode** — force registration with container IP + internal port (global or per-port)
- **Per-port service config** — explicitly declared `CONSUL_SERVICE_<port>_*` ports are always merged with the default port
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
| `CONSUL_SERVICE_<port>_NAME` | Declare and name an additional service on a specific container port | Same as above |
| `CONSUL_SERVICE_<port>_TAGS` | Tags for a declared container port | Same as above |
| `CONSUL_SERVICE_<port>_POD_IP` | Per-port POD_IP override for a declared container port | Inherits `CONSUL_SERVICE_POD_IP` |

### Port Selection

Registered services are built from two sources:

1. Explicit `CONSUL_SERVICE_<port>_*` labels / env vars. These always declare that container port and are always merged into the final service set.
2. One default port selected by:
   1. `CONSUL_SERVICE_PORT` environment variable / label
   2. Smallest Docker port mapping (`HostPort`) container port
   3. Smallest `NetworkSettings.Ports` key (catches compose `expose:` and other Docker-known ports)
   4. Smallest `EXPOSE` directive from Dockerfile

If an explicit per-port declaration points at a host-mapped container port, the host mapping is still used when `CONSUL_SERVICE_<port>_POD_IP=false`.

Containers with no declared port and no detectable default port are **skipped** (not registered). For docker-compose services without explicit port declarations, you can add `expose:` in the compose file, `EXPOSE` in the Dockerfile, or use per-port labels like `CONSUL_SERVICE_8080_NAME=myapp` to declare ports directly.

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

Host IP detection priority: `ADVERTISE_ADDR` env > `getaddrinfo(hostname)` > outbound IP via UDP probe > hostname string fallback.

## Registrator Configuration

Environment variables for the registrator container itself:

| Variable | Description | Default |
|----------|-------------|---------|
| `REGISTRATOR_TOKEN` | Consul ACL token | _(none)_ |
| `CONSUL_ADDR` | Consul HTTP address | `http://localhost:8500` |
| `DOCKER_SOCK` | Docker Engine socket path | `/var/run/docker.sock` |
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
 ├─ collect_declared_ports() + detect_default_port()
 │    ├─ Explicit CONSUL_SERVICE_<port>_* labels/env vars → always merged
 │    ├─ Default port: CONSUL_SERVICE_PORT → look up HostPort mapping
 │    ├─ Default port: smallest NetworkSettings.Ports HostPort binding
 │    ├─ Default port: smallest NetworkSettings.Ports key
 │    ├─ Default port: smallest Config.ExposedPorts entry
 │    └─ No declared/default port → port_count=0, skip registration
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

## Gateway (OpenResty)

The `openresty/` directory ships an optional OpenResty gateway that turns what
the registrator publishes into a Consul-driven dynamic router. It polls the
**local** Consul agent, keeps a routing table in shared memory, and reverse-
proxies each request to a healthy backend by hostname or path. Routing config
lives in Consul KV, so domains can be regrouped at runtime without restarting
the gateway.

### Domain groups (Consul KV)

Config is **per node**: the gateway resolves its Consul node name (from the
local agent's `GET /v1/agent/self`, or the `NODE_NAME` env override) and reads
`gateway/<node>/configuration` — a JSON array of groups. Each group binds a set
of domain suffixes to a set of tags:

```bash
# <node> is this gateway's Consul node name (e.g. from `consul members`)
consul kv put gateway/<node>/configuration '[
  {"name":"web",  "suffixes":["svc.local"],                  "tags":["web","prometheus"]},
  {"name":"data", "suffixes":["data.local","data.internal"], "tags":["data"]}
]'
```

- A service is routable through a group only if it carries one of the group's
  tags. A service that matches several groups is registered in **each** of them
  (multi-membership) and reachable through every matching group's suffixes.
- Config is pulled every `POLL_INTERVAL` seconds. On any KV read failure the
  last good config is kept; if nothing was ever loaded, every request returns
  `404`. There is no global default and no env fallback — each node must have
  its own `gateway/<node>/configuration` key (pure per-node KV).

### Access forms

Every suffix accepts two equivalent request forms:

| Form | Example | Backend receives |
|------|---------|------------------|
| Subdomain | `orders.svc.local/api` | `/api` |
| Path | `svc.local/orders/api` | `/api` (the `/orders` prefix is stripped) |

Both reach service `orders` (as long as `orders` carries a tag of the group
that owns `svc.local`). The path form is what lets one "main domain" front many
services, e.g. `node.cen/<service>/...`.

### Gateway configuration

Environment variables for the gateway container (`openresty/docker-compose.yaml`):

| Variable | Description | Default |
|----------|-------------|---------|
| `CONSUL_ADDR` | Local Consul agent HTTP address | `http://127.0.0.1:8500` |
| `CONSUL_TOKEN` | Consul ACL token (needs agent + service + KV read) | _(empty)_ |
| `POLL_INTERVAL` | KV + service poll interval, seconds | `5` |
| `NODE_NAME` | Override the Consul node name (else read from agent/self) | _(auto)_ |

The token needs read on the agent (for the node name), the service catalog, and
the `gateway/` KV prefix — a ready-to-apply policy is in
[`consul/gateway-policy.hcl`](consul/gateway-policy.hcl).

### Node scope (important)

**The gateway is node-local.** It discovers services with the local agent's
`GET /v1/agent/services`, which only returns services registered on **the same
Consul node the gateway runs on** — the containers this node's registrator
registered. It sees nothing on other nodes and does no per-node pinning.

- To route a node's services, run a gateway **on that node** (co-located with
  its registrator and agent). For a cluster, deploy one gateway per node.
- `node.cen` is a **literal** suffix you configure, **not** a Consul node name.
  Per-node addressing (`<service>.<node>.cen`) is **not** supported; it would
  require catalog/health discovery instead of the agent-local view.
- Prometheus `consul_sd` discovers targets **cluster-wide** (catalog) while the
  gateway is node-local. A target `<service>.cen` that resolves to a gateway not
  on the service's node will `404`. Keep `*.cen` DNS pointing at a gateway
  co-located with the services it must reach (single-node, or per-node gateways
  with node-scoped DNS).

## License

MIT
