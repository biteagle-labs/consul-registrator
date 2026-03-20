# Consul Registrator

English | [中文](README_CN.md)

Lightweight Docker service registrator for Consul, written in C. Watches Docker container start/stop events and automatically registers/deregisters services with a Consul agent.

Built on Alpine Linux with libcurl and cJSON. Single binary, minimal footprint.

## Features

- **Opt-in mode** — only containers with `CONSUL_LISTEN_ENABLE=true` are registered
- **Dual-channel config** — reads from container environment variables first, then labels
- **Auto port detection** — `CONSUL_SERVICE_PORT` > HostPort mapping > EXPOSE > default 8080
- **Smart address resolution** — uses host IP for port-mapped containers, container IP for bridge/custom networks
- **Per-port service config** — register multiple services per container with `CONSUL_SERVICE_<port>_NAME`
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
      - DEFAULT_PORT=${DEFAULT_PORT:-8080}
      - ADVERTISE_ADDR=${ADVERTISE_ADDR:-}
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
| `CONSUL_SERVICE_PORT` | Override auto-detected port | Auto-detect |
| `CONSUL_SERVICE_<port>_NAME` | Service name for a specific port | Same as above |
| `CONSUL_SERVICE_<port>_TAGS` | Tags for a specific port | Same as above |

### Port Detection Priority

1. `CONSUL_SERVICE_PORT` environment variable / label
2. Docker port mapping (`HostPort`)
3. `EXPOSE` directive from Dockerfile
4. Default port (8080)

### Address Resolution

The service address registered in Consul is determined automatically:

| Scenario | Address |
|----------|---------|
| Container with port mapping (`-p`) | Host IP (auto-detected or `ADVERTISE_ADDR`) |
| Container on bridge/custom network (no port mapping) | Container IP |
| Container with `network_mode: host` | Host IP |

Host IP detection priority: `ADVERTISE_ADDR` env > `getaddrinfo(hostname)` > outbound IP via UDP probe.

## Registrator Configuration

Environment variables for the registrator container itself:

| Variable | Description | Default |
|----------|-------------|---------|
| `REGISTRATOR_TOKEN` | Consul ACL token | _(none)_ |
| `CONSUL_ADDR` | Consul HTTP address | `http://localhost:8500` |
| `RESYNC_INTERVAL` | Full sync interval in seconds | `30` |
| `DEFAULT_PORT` | Fallback port when none detected | `8080` |
| `ADVERTISE_ADDR` | Override auto-detected host IP for Consul registration | Auto-detect |

## Architecture

```
Docker daemon
    |
    +-- /var/run/docker.sock --> registrator (host network)
    |                               |
    |   +---------------------------+
    |   |
    |   +-- libcurl unix-socket -> Docker API (/events stream, /containers)
    |   +-- libcurl HTTP -> Consul HTTP API (/v1/agent/service/register|deregister)
    |
    +-- App containers (only those with CONSUL_LISTEN_ENABLE=true are registered)
```

## License

MIT
