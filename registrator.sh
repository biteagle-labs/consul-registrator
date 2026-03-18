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
DEFAULT_PORT="${DEFAULT_PORT:-8080}"
HOSTNAME_OVERRIDE="${HOSTNAME_OVERRIDE:-$(hostname)}"

# --- Global state ---
RESYNC_PID=""
WATCH_PID=""

# --- Logging ---
log_info()  { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [INFO]  $*"; }
log_warn()  { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [WARN]  $*"; }
log_error() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [ERROR] $*" >&2; }

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

# --- Detect ports ---
# Priority: CONSUL_SERVICE_PORT > HostPort mapping > EXPOSE > 8080
detect_ports() {
    local inspect_json="$1"

    # 1. Manual port override
    local manual_port
    manual_port=$(get_container_config "$inspect_json" "CONSUL_SERVICE_PORT" "")
    if [ -n "$manual_port" ]; then
        echo "$manual_port"
        return
    fi

    # 2. Port mappings (HostPort)
    local mapped_ports
    mapped_ports=$(echo "$inspect_json" | jq -r '
        [.NetworkSettings.Ports // {} | to_entries[] |
         select(.value != null) |
         .value[] | select(.HostPort != null and .HostPort != "") |
         .HostPort] | unique | .[]' 2>/dev/null)

    if [ -n "$mapped_ports" ]; then
        echo "$mapped_ports"
        return
    fi

    # 3. EXPOSE ports
    local exposed_ports
    exposed_ports=$(echo "$inspect_json" | jq -r '
        [.Config.ExposedPorts // {} | keys[] | split("/")[0]] | unique | .[]' 2>/dev/null)

    if [ -n "$exposed_ports" ]; then
        echo "$exposed_ports"
        return
    fi

    # 4. Default
    echo "$DEFAULT_PORT"
}

# --- Register a single container ---
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
        return 0
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

    # Get port list
    local ports
    ports=$(detect_ports "$inspect_json")

    if [ -z "$ports" ]; then
        ports="$DEFAULT_PORT"
    fi

    # Register a service for each port
    echo "$ports" | while IFS= read -r port; do
        [ -z "$port" ] && continue

        # Check for port-specific config overrides
        local port_name
        port_name=$(get_container_config "$inspect_json" "CONSUL_SERVICE_${port}_NAME" "$service_name")

        local port_tags_str
        port_tags_str=$(get_container_config "$inspect_json" "CONSUL_SERVICE_${port}_TAGS" "")
        local port_tags="$service_tags"
        if [ -n "$port_tags_str" ]; then
            port_tags=$(echo "$port_tags_str" | jq -R 'split(",") | map(gsub("^\\s+|\\s+$"; "")) | map(select(length > 0))')
        fi

        local service_id="${HOSTNAME_OVERRIDE}:${container_name}:${port}"

        local register_json
        register_json=$(jq -n \
            --arg id "$service_id" \
            --arg name "$port_name" \
            --arg addr "$HOSTNAME_OVERRIDE" \
            --argjson port "$port" \
            --argjson tags "$port_tags" \
            --arg container_id "$container_id" \
            --arg container_name "$container_name" \
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
                }
            }')

        local result
        result=$(consul_put "/v1/agent/service/register" "$register_json")

        if [ -z "$result" ]; then
            log_info "Registered service: $port_name ($service_id) port=$port"
        else
            log_error "Registration failed: $port_name ($service_id): $result"
        fi
    done
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
            log_info "Deregistered service: $sid"
        else
            log_error "Deregistration failed: $sid: $result"
        fi
    done
}

# --- Full sync ---
sync_all() {
    log_info "Starting full sync..."

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
        register_container "$cid"

        # Check if this container has registration enabled
        local inspect_json
        inspect_json=$(docker_api "/containers/$cid/json")
        local enabled
        enabled=$(get_container_config "$inspect_json" "CONSUL_LISTEN_ENABLE" "false")
        if [ "$enabled" = "true" ]; then
            registered_container_ids="$registered_container_ids $cid"
        fi
    done

    # Clean up orphaned services (registered by us but container no longer exists)
    cleanup_orphans "$registered_container_ids"

    log_info "Full sync completed"
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
                log_info "Container stopped: $container_name ($container_id)"
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
    log_info "========================================="
    log_info "Consul Registrator starting"
    log_info "Consul:   $CONSUL_ADDR"
    log_info "Docker:   $DOCKER_SOCK"
    log_info "Resync:   every ${RESYNC_INTERVAL}s"
    log_info "Default:  port $DEFAULT_PORT"
    log_info "Hostname: $HOSTNAME_OVERRIDE"
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
