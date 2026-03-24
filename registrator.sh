#!/bin/sh
set -e

# =============================================================================
# Consul Registrator -- Alpine-based Docker service auto-registrator (shell)
#
# Watches Docker container start/stop events and automatically
# registers/deregisters services with Consul.
# Opt-in mode: only containers with CONSUL_LISTEN_ENABLE=true are registered.
# =============================================================================

# --- Configuration ---
CONSUL_ADDR="${CONSUL_ADDR:-http://localhost:8500}"
CONSUL_TOKEN="${REGISTRATOR_TOKEN:-}"
DOCKER_SOCK="${DOCKER_SOCK:-/var/run/docker.sock}"
RESYNC_INTERVAL="${RESYNC_INTERVAL:-30}"
HOSTNAME_OVERRIDE="${HOSTNAME_OVERRIDE:-$(hostname)}"
LOG_LEVEL="${LOG_LEVEL:-info}"

# --- Global state ---
RESYNC_PID=""
WATCH_PID=""
HOST_IP=""

# --- Logging ---
log_info()  { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [INFO ] $*"; }
log_warn()  { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [WARN ] $*"; }
log_error() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [ERROR] $*" >&2; }
log_debug() {
    case "$LOG_LEVEL" in
        debug|DEBUG) echo "[$(date '+%Y-%m-%d %H:%M:%S')] [DEBUG] $*" ;;
    esac
}

# --- Service state tracking (change-only logging) ---
TRACKED_SERVICES=""

track_is_new() {
    local service_id="$1"
    case " $TRACKED_SERVICES " in
        *" $service_id "*) return 1 ;;  # already tracked
        *) return 0 ;;                  # new
    esac
}

track_register() {
    local service_id="$1"
    if track_is_new "$service_id"; then
        TRACKED_SERVICES="$TRACKED_SERVICES $service_id"
        return 0  # is new
    fi
    return 1  # already tracked
}

track_deregister() {
    local service_id="$1"
    TRACKED_SERVICES=$(echo "$TRACKED_SERVICES" | sed "s| $service_id | |g; s|^ ||; s| $||")
}

# --- Docker API ---
docker_api() {
    local endpoint="$1"
    curl -s --unix-socket "$DOCKER_SOCK" "http://localhost$endpoint"
}

# --- Consul API ---
consul_put() {
    local endpoint="$1"
    local data="$2"
    local args="-s -X PUT"
    if [ -n "$CONSUL_TOKEN" ]; then
        args="$args -H \"X-Consul-Token: $CONSUL_TOKEN\""
    fi
    if [ -n "$data" ]; then
        eval curl $args -H "'Content-Type: application/json'" -d "'$data'" "\"${CONSUL_ADDR}${endpoint}\""
    else
        eval curl $args "\"${CONSUL_ADDR}${endpoint}\""
    fi
}

consul_get() {
    local endpoint="$1"
    if [ -n "$CONSUL_TOKEN" ]; then
        curl -s -H "X-Consul-Token: $CONSUL_TOKEN" "${CONSUL_ADDR}${endpoint}"
    else
        curl -s "${CONSUL_ADDR}${endpoint}"
    fi
}

# --- Resolve host IP address ---
# Priority: ADVERTISE_ADDR > getent hostname > outbound IP via UDP trick
resolve_host_ip() {
    # 1. Explicit env var override
    if [ -n "${ADVERTISE_ADDR:-}" ]; then
        HOST_IP="$ADVERTISE_ADDR"
        return
    fi

    # 2. Try resolving hostname (skip 127.x.x.x)
    local resolved
    resolved=$(getent ahostsv4 "$(hostname)" 2>/dev/null | awk 'NR==1{print $1}')
    if [ -n "$resolved" ] && ! echo "$resolved" | grep -q '^127\.'; then
        HOST_IP="$resolved"
        return
    fi

    # 3. UDP connect trick -- get outbound IP (no packet sent)
    local outbound
    outbound=$(python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(('8.8.8.8', 53))
print(s.getsockname()[0])
s.close()
" 2>/dev/null || true)
    if [ -n "$outbound" ] && [ "$outbound" != "0.0.0.0" ]; then
        HOST_IP="$outbound"
        return
    fi

    # 4. Fallback to hostname
    HOST_IP="$HOSTNAME_OVERRIDE"
}

# --- Get config value from container ---
# Priority: env var > label > default
get_container_config() {
    local inspect_json="$1"
    local key="$2"
    local default_val="${3:-}"

    # Check environment variables first
    local val
    val=$(echo "$inspect_json" | jq -r \
        --arg key "$key" \
        '[.Config.Env[] // empty | select(startswith($key + "="))] | if length > 0 then .[0] | split("=") | .[1:] | join("=") else empty end' 2>/dev/null)

    if [ -n "$val" ]; then
        echo "$val"
        return
    fi

    # Then check labels
    val=$(echo "$inspect_json" | jq -r --arg key "$key" '.Config.Labels[$key] // empty' 2>/dev/null)

    if [ -n "$val" ]; then
        echo "$val"
        return
    fi

    echo "$default_val"
}

# --- Extract service name from image ---
extract_image_name() {
    local image="$1"
    # Strip registry prefix and tag
    echo "$image" | sed 's|.*/||' | sed 's|:.*||'
}

# --- Get container name ---
get_container_name() {
    local inspect_json="$1"
    echo "$inspect_json" | jq -r '.Name' | sed 's|^/||'
}

# --- Get container IP ---
get_container_ip() {
    local inspect_json="$1"

    # 1. NetworkSettings.IPAddress (default bridge)
    local ip
    ip=$(echo "$inspect_json" | jq -r '.NetworkSettings.IPAddress // empty' 2>/dev/null)
    if [ -n "$ip" ]; then
        echo "$ip"
        return
    fi

    # 2. NetworkSettings.Networks.<name>.IPAddress (custom networks)
    ip=$(echo "$inspect_json" | jq -r '[.NetworkSettings.Networks // {} | to_entries[] | .value.IPAddress // empty | select(. != "")] | first // empty' 2>/dev/null)
    if [ -n "$ip" ]; then
        echo "$ip"
        return
    fi
}

# --- Detect ports ---
# Priority: CONSUL_SERVICE_PORT > HostPort mapping > EXPOSE
# Returns JSON array: [{"host_port": N, "container_port": N, "host_mapped": bool}, ...]
detect_ports() {
    local inspect_json="$1"

    # 1. Manual port override (CONSUL_SERVICE_PORT = internal/container port)
    local manual_port
    manual_port=$(get_container_config "$inspect_json" "CONSUL_SERVICE_PORT" "")
    if [ -n "$manual_port" ]; then
        # Look up corresponding HostPort for this internal port
        local host_port host_mapped
        host_port=$(echo "$inspect_json" | jq -r --arg cp "$manual_port" '
            .NetworkSettings.Ports // {} | to_entries[] |
            select(.key | startswith($cp + "/")) |
            .value // [] | .[] |
            select(.HostPort != null and .HostPort != "") |
            .HostPort' 2>/dev/null | head -1)

        if [ -n "$host_port" ]; then
            echo "[{\"host_port\":$host_port,\"container_port\":$manual_port,\"host_mapped\":true}]"
        else
            echo "[{\"host_port\":$manual_port,\"container_port\":$manual_port,\"host_mapped\":false}]"
        fi
        return
    fi

    # 2. Port mappings (HostPort) -- deduplicate
    local mapped
    mapped=$(echo "$inspect_json" | jq '
        [.NetworkSettings.Ports // {} | to_entries[] |
         select(.value != null) |
         {container_port: (.key | split("/")[0] | tonumber),
          bindings: [.value[] | select(.HostPort != null and .HostPort != "") | .HostPort | tonumber]} |
         select(.bindings | length > 0) |
         .bindings[] as $hp |
         {host_port: $hp, container_port: .container_port, host_mapped: true}
        ] | unique_by(.host_port)' 2>/dev/null)

    if [ -n "$mapped" ] && [ "$mapped" != "[]" ]; then
        echo "$mapped"
        return
    fi

    # 3. EXPOSE ports (no host mapping -- container IP)
    local exposed
    exposed=$(echo "$inspect_json" | jq '
        [.Config.ExposedPorts // {} | keys[] | split("/")[0] | tonumber] | unique |
        [.[] | {host_port: ., container_port: ., host_mapped: false}]' 2>/dev/null)

    if [ -n "$exposed" ] && [ "$exposed" != "[]" ]; then
        echo "$exposed"
        return
    fi

    # 4. No port detected
    echo "[]"
}

# --- Register a single container ---
# Returns exit code 0 if container was enabled (CONSUL_LISTEN_ENABLE=true)
register_container() {
    local container_id="$1"

    # Get container details
    local inspect_json
    inspect_json=$(docker_api "/containers/$container_id/json")

    if [ -z "$inspect_json" ] || echo "$inspect_json" | jq -e '.message' >/dev/null 2>&1; then
        log_warn "Failed to inspect container: $container_id"
        return 1
    fi

    # Check opt-in flag
    local enabled
    enabled=$(get_container_config "$inspect_json" "CONSUL_LISTEN_ENABLE" "false")
    if [ "$enabled" != "true" ]; then
        return 1
    fi

    local container_name
    container_name=$(get_container_name "$inspect_json")

    local image
    image=$(echo "$inspect_json" | jq -r '.Config.Image')

    local default_service_name
    default_service_name=$(extract_image_name "$image")

    local service_name
    service_name=$(get_container_config "$inspect_json" "CONSUL_SERVICE_NAME" "$default_service_name")

    local service_tags_str
    service_tags_str=$(get_container_config "$inspect_json" "CONSUL_SERVICE_TAGS" "")

    # Convert tags to JSON array
    local service_tags="[]"
    if [ -n "$service_tags_str" ]; then
        service_tags=$(echo "$service_tags_str" | jq -R 'split(",") | map(gsub("^\\s+|\\s+$"; "")) | map(select(length > 0))')
    fi

    # Get port list (JSON array)
    local ports_json
    ports_json=$(detect_ports "$inspect_json")

    local port_count
    port_count=$(echo "$ports_json" | jq 'length')

    # Skip registration if no port was detected
    if [ "$port_count" -eq 0 ]; then
        log_debug "Skipping $container_name ($container_id): no port detected, service not registered"
        return 0  # still enabled, just no port
    fi

    # Get container IP and detect host network mode
    local container_ip
    container_ip=$(get_container_ip "$inspect_json")
    local is_host_network=false
    if [ -z "$container_ip" ]; then
        is_host_network=true
    fi

    # Global check interval and POD_IP
    local check_interval
    check_interval=$(get_container_config "$inspect_json" "CONSUL_SERVICE_CHECK_INTERVAL" "10s")
    local global_pod_ip
    global_pod_ip=$(get_container_config "$inspect_json" "CONSUL_SERVICE_POD_IP" "false")

    if [ "$is_host_network" = "true" ] && [ "$global_pod_ip" = "true" ]; then
        log_debug "POD_IP ignored for $container_name: container is in host network mode"
    fi

    # Register a service for each port
    local i=0
    while [ "$i" -lt "$port_count" ]; do
        local port cport host_mapped
        port=$(echo "$ports_json" | jq -r ".[$i].host_port")
        cport=$(echo "$ports_json" | jq -r ".[$i].container_port")
        host_mapped=$(echo "$ports_json" | jq -r ".[$i].host_mapped")

        # Per-port config overrides (keys always refer to internal port)
        local port_name
        port_name=$(get_container_config "$inspect_json" "CONSUL_SERVICE_${cport}_NAME" "$service_name")

        local port_tags_str
        port_tags_str=$(get_container_config "$inspect_json" "CONSUL_SERVICE_${cport}_TAGS" "")
        local port_tags="$service_tags"
        if [ -n "$port_tags_str" ]; then
            port_tags=$(echo "$port_tags_str" | jq -R 'split(",") | map(gsub("^\\s+|\\s+$"; "")) | map(select(length > 0))')
        fi

        # POD_IP: per-port > global
        local port_pod_ip use_pod_ip
        port_pod_ip=$(get_container_config "$inspect_json" "CONSUL_SERVICE_${cport}_POD_IP" "")
        use_pod_ip="$global_pod_ip"
        if [ -n "$port_pod_ip" ]; then
            use_pod_ip="$port_pod_ip"
        fi

        # Address/port resolution priority:
        # 1. host network mode → always host IP, POD_IP meaningless
        # 2. POD_IP=true (force) → container IP + internal port
        # 3. port mapped → host IP + host port
        # 4. EXPOSE only → container IP + container port
        local svc_addr reg_port
        if [ "$is_host_network" = "true" ]; then
            svc_addr="$HOST_IP"
            reg_port="$cport"
        elif [ "$use_pod_ip" = "true" ]; then
            svc_addr="$container_ip"
            reg_port="$cport"
        elif [ "$host_mapped" = "true" ]; then
            svc_addr="$HOST_IP"
            reg_port="$port"
        else
            svc_addr="$container_ip"
            reg_port="$cport"
        fi

        local service_id="${HOSTNAME_OVERRIDE}:${container_name}:${reg_port}"

        # TCP health check target
        local tcp_target
        if [ -n "$container_ip" ]; then
            tcp_target="${container_ip}:${cport}"
        else
            tcp_target="${svc_addr}:${reg_port}"
        fi

        local register_json
        register_json=$(jq -n \
            --arg id "$service_id" \
            --arg name "$port_name" \
            --arg addr "$svc_addr" \
            --argjson port "$reg_port" \
            --argjson tags "$port_tags" \
            --arg container_id "$container_id" \
            --arg container_name "$container_name" \
            --arg tcp_target "$tcp_target" \
            --arg check_interval "$check_interval" \
            '{
                ID: $id,
                Name: $name,
                Address: $addr,
                Port: $port,
                Tags: $tags,
                Meta: {
                    container_id: $container_id,
                    container_name: $container_name,
                    registrator: "self-hosted"
                },
                Check: {
                    TCP: $tcp_target,
                    Interval: $check_interval,
                    Timeout: "5s"
                }
            }')

        local result
        result=$(consul_put "/v1/agent/service/register" "$register_json")

        if [ -z "$result" ] || [ "$result" = "null" ]; then
            if track_register "$service_id"; then
                log_info "Registered service: $port_name ($service_id) addr=$svc_addr port=$reg_port check=tcp/$check_interval"
            fi
        else
            log_error "Registration failed: $port_name ($service_id): $result"
        fi

        i=$((i + 1))
    done

    return 0
}

# --- Deregister all services for a container ---
deregister_container() {
    local container_id="$1"
    local container_name="${2:-unknown}"

    # Find all services registered for this container in Consul
    local services
    services=$(consul_get "/v1/agent/services")

    if [ -z "$services" ]; then
        return 0
    fi

    # Match services by Meta.container_id
    local service_ids
    service_ids=$(echo "$services" | jq -r --arg cid "$container_id" '
        to_entries[] |
        select(.value.Meta.container_id == $cid) |
        .key' 2>/dev/null)

    if [ -z "$service_ids" ]; then
        # Fallback: match by container_name in service ID
        service_ids=$(echo "$services" | jq -r --arg cname "$container_name" --arg host "$HOSTNAME_OVERRIDE" '
            to_entries[] |
            select(.key | startswith($host + ":" + $cname + ":")) |
            .key' 2>/dev/null)
    fi

    echo "$service_ids" | while IFS= read -r sid; do
        [ -z "$sid" ] && continue
        local result
        result=$(consul_put "/v1/agent/service/deregister/$sid")
        if [ -z "$result" ]; then
            track_deregister "$sid"
            log_info "Deregistered service: $sid"
        else
            log_error "Deregistration failed: $sid: $result"
        fi
    done
}

# --- Full sync ---
sync_all() {
    log_debug "Starting full sync..."

    # Get all running containers
    local containers
    containers=$(docker_api "/containers/json")

    if [ -z "$containers" ] || [ "$containers" = "null" ]; then
        log_warn "Failed to list containers"
        return 1
    fi

    # Collect container IDs that should be registered
    local registered_container_ids=""

    local container_ids
    container_ids=$(echo "$containers" | jq -r '.[].Id')

    for cid in $container_ids; do
        # register_container returns 0 if CONSUL_LISTEN_ENABLE=true
        if register_container "$cid"; then
            registered_container_ids="$registered_container_ids $cid"
        fi
    done

    # Clean up orphaned services (registered by us but container no longer exists)
    cleanup_orphans "$registered_container_ids"

    log_debug "Full sync completed"
}

# --- Clean up orphaned services ---
cleanup_orphans() {
    local valid_container_ids="$1"

    local services
    services=$(consul_get "/v1/agent/services")

    if [ -z "$services" ]; then
        return 0
    fi

    # Find services registered by this registrator
    local registrator_services
    registrator_services=$(echo "$services" | jq -r '
        to_entries[] |
        select(.value.Meta.registrator == "self-hosted") |
        {key: .key, container_id: .value.Meta.container_id} |
        "\(.key)|\(.container_id)"' 2>/dev/null)

    echo "$registrator_services" | while IFS= read -r line; do
        [ -z "$line" ] && continue
        local sid cid
        sid=$(echo "$line" | cut -d'|' -f1)
        cid=$(echo "$line" | cut -d'|' -f2)

        # Check if the container is still running
        if ! echo "$valid_container_ids" | grep -q "$cid"; then
            track_deregister "$sid"
            log_info "Cleaning orphaned service: $sid (container $cid no longer exists)"
            consul_put "/v1/agent/service/deregister/$sid"
        fi
    done
}

# --- Watch Docker events ---
watch_events() {
    log_info "Watching Docker events..."

    # Filter by type=container only; action matching is done in the script
    # (Docker API action filter uses prefix matching, "start" would match "exec_start" etc.)
    local filters='{"type":["container"]}'
    local encoded_filters
    encoded_filters=$(printf '%s' "$filters" | jq -sRr @uri)

    curl -s -N --unix-socket "$DOCKER_SOCK" \
        "http://localhost/events?filters=${encoded_filters}" 2>/dev/null | \
    while IFS= read -r event_line; do
        [ -z "$event_line" ] && continue

        # Fast string pre-filter: avoid forking jq for every exec_* event
        case "$event_line" in
            *'"Action":"start"'*|*'"Action":"stop"'*|*'"Action":"die"'*) ;;
            *) continue ;;
        esac

        local event_action container_id container_name
        event_action=$(echo "$event_line" | jq -r '.Action // empty' 2>/dev/null)
        container_id=$(echo "$event_line" | jq -r '.id // empty' 2>/dev/null)
        container_name=$(echo "$event_line" | jq -r '.Actor.Attributes.name // empty' 2>/dev/null)

        [ -z "$container_id" ] && continue

        case "$event_action" in
            start)
                log_info "Container started: $container_name ($container_id)"
                register_container "$container_id"
                ;;
            stop|die)
                log_info "Container stopped [$event_action]: $container_name ($container_id)"
                deregister_container "$container_id" "$container_name"
                ;;
        esac
    done
}

# --- Periodic resync ---
resync_loop() {
    while true; do
        sleep "$RESYNC_INTERVAL"
        sync_all
    done
}

# --- Signal handler ---
cleanup() {
    log_info "Received shutdown signal, cleaning up..."

    # Stop background processes
    [ -n "$RESYNC_PID" ] && kill "$RESYNC_PID" 2>/dev/null
    [ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null

    # Wait for child processes
    wait 2>/dev/null

    log_info "Registrator stopped"
    exit 0
}

trap cleanup SIGTERM SIGINT SIGHUP

# --- Preflight checks ---
preflight_check() {
    # Check Docker socket
    if [ ! -S "$DOCKER_SOCK" ]; then
        log_error "Docker socket not found: $DOCKER_SOCK"
        exit 1
    fi

    # Check Docker API connectivity
    local docker_info
    docker_info=$(docker_api "/info" 2>/dev/null)
    if [ -z "$docker_info" ] || echo "$docker_info" | jq -e '.message' >/dev/null 2>&1; then
        log_error "Cannot connect to Docker API"
        exit 1
    fi

    # Check Consul connectivity
    local consul_leader
    consul_leader=$(consul_get "/v1/status/leader" 2>/dev/null)
    if [ -z "$consul_leader" ]; then
        log_error "Cannot connect to Consul: $CONSUL_ADDR"
        exit 1
    fi

    log_info "Preflight OK -- Docker API and Consul are reachable"
}

# --- Main ---
main() {
    resolve_host_ip

    log_info "========================================="
    log_info "Consul Registrator (shell) starting"
    log_info "Consul:   $CONSUL_ADDR"
    log_info "Docker:   $DOCKER_SOCK"
    log_info "Resync:   every ${RESYNC_INTERVAL}s"
    log_info "Hostname: $HOSTNAME_OVERRIDE"
    log_info "Host IP:  $HOST_IP"
    log_info "Mode:     opt-in (CONSUL_LISTEN_ENABLE=true)"
    log_info "========================================="

    preflight_check

    # Initial full sync
    sync_all

    # Start periodic resync (background)
    resync_loop &
    RESYNC_PID=$!

    # Start event watcher (background)
    watch_events &
    WATCH_PID=$!

    # Wait for any child to exit
    wait
}

main "$@"
