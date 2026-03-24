/*
 * Consul Registrator -- C implementation
 *
 * Watches Docker container start/stop events and automatically
 * registers/deregisters services with Consul.
 * Opt-in mode: only containers with CONSUL_LISTEN_ENABLE=true are registered.
 * Supports dual-channel config (env vars > labels), env vars take priority.
 *
 * Dependencies: libcurl, cjson, pthreads (all from Alpine apk)
 */

#define _POSIX_C_SOURCE 200809L

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* -- Constants ------------------------------------------------------------ */

#define MAX_PORTS              32
#define DEFAULT_CONSUL_ADDR    "http://localhost:8500"
#define DEFAULT_DOCKER_SOCK    "/var/run/docker.sock"
#define DEFAULT_RESYNC_INTERVAL 30

/* -- Types ---------------------------------------------------------------- */

typedef struct {
    char consul_addr[256];
    char consul_token[256];
    char docker_sock[256];
    int  resync_interval;
    char hostname[64];
    char host_ip[64];     /* resolved IP for Consul Address field */
} Config;

typedef struct {
    int  host_port;       /* port for Consul registration (external access) */
    int  container_port;  /* internal container port (for label/env key lookup) */
} PortEntry;

typedef struct {
    char      container_id[128];
    char      container_name[128];
    PortEntry ports[MAX_PORTS];
    int       port_count;
    int       host_mapped;    /* 1 = ports from HostPort mapping, use host IP */
} ServiceDef;

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buffer;

/* Event queue node */
typedef struct QEvent {
    char         action[16];
    char         container_id[128];
    char         container_name[128];
    struct QEvent *next;
} QEvent;

typedef struct {
    Buffer  buf;
} EventCtx;

/* -- Globals -------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t       g_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* Event queue (write callback -> event_processor_thread) */
static QEvent          *g_eq_head  = NULL;
static QEvent          *g_eq_tail  = NULL;
static pthread_mutex_t  g_eq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_eq_cond  = PTHREAD_COND_INITIALIZER;

/* Service state tracking (for change-only logging) */
#define MAX_TRACKED        512
static char             g_tracked[MAX_TRACKED][256];
static int              g_tracked_count = 0;
static pthread_mutex_t  g_tracked_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -- Dynamic buffer ------------------------------------------------------- */

static void buf_init(Buffer *b)
{
    b->data = malloc(4096);
    b->len  = 0;
    b->cap  = 4096;
    if (b->data) b->data[0] = '\0';
}

static void buf_free(Buffer *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_append(Buffer *b, const char *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t new_cap = b->cap * 2 + n + 1;
        char  *nd      = realloc(b->data, new_cap);
        if (!nd) return -1;
        b->data = nd;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/* -- Logging -------------------------------------------------------------- */

static int g_log_debug = 0;   /* 1 = enable DEBUG output, set by LOG_LEVEL=debug */

static void log_msg(const char *level, const char *fmt, ...)
{
    time_t     t  = time(NULL);
    struct tm  tm_buf;
    struct tm *tm = localtime_r(&t, &tm_buf);
    char       ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    printf("[%s] [%s] ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#define log_debug(...) do { if (g_log_debug) log_msg("DEBUG", __VA_ARGS__); } while(0)
#define log_info(...)  log_msg("INFO ", __VA_ARGS__)
#define log_warn(...)  log_msg("WARN ", __VA_ARGS__)
#define log_error(...) log_msg("ERROR", __VA_ARGS__)

/* -- libcurl write callback (generic) ------------------------------------- */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buffer *buf   = (Buffer *)userdata;
    size_t  total = size * nmemb;
    return buf_append(buf, ptr, total) == 0 ? total : 0;
}

/* -- Docker API (unix socket) --------------------------------------------- */

static int docker_request(const Config *cfg, const char *path, char **out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[512];
    snprintf(url, sizeof(url), "http://localhost%s", path);

    Buffer resp;
    buf_init(&resp);

    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, cfg->docker_sock);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        buf_free(&resp);
        return -1;
    }
    *out = resp.data; /* caller is responsible for free() */
    return 0;
}

/* -- Consul API (HTTP) ---------------------------------------------------- */

static int consul_request(const Config *cfg, const char *method,
                          const char *path, const char *body, char **out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", cfg->consul_addr, path);

    Buffer resp;
    buf_init(&resp);

    struct curl_slist *headers = NULL;
    if (cfg->consul_token[0]) {
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "X-Consul-Token: %s", cfg->consul_token);
        headers = curl_slist_append(headers, hdr);
    }
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }

    pthread_mutex_lock(&g_mutex);
    CURLcode rc = curl_easy_perform(curl);
    pthread_mutex_unlock(&g_mutex);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        buf_free(&resp);
        return -1;
    }
    if (out) {
        *out = resp.data;
    } else {
        buf_free(&resp);
    }
    return 0;
}

/* -- Container config: env var > label > default -------------------------- */

static const char *get_config(cJSON *inspect, const char *key, const char *def)
{
    cJSON *config = cJSON_GetObjectItem(inspect, "Config");

    /* 1. Environment variable "KEY=VALUE" */
    cJSON *env_arr = cJSON_GetObjectItem(config, "Env");
    if (cJSON_IsArray(env_arr)) {
        size_t klen = strlen(key);
        cJSON *item;
        cJSON_ArrayForEach(item, env_arr) {
            const char *s = cJSON_GetStringValue(item);
            if (s && strncmp(s, key, klen) == 0 && s[klen] == '=') {
                return s + klen + 1;
            }
        }
    }

    /* 2. Labels */
    cJSON *labels = cJSON_GetObjectItem(config, "Labels");
    if (cJSON_IsObject(labels)) {
        cJSON *label = cJSON_GetObjectItem(labels, key);
        if (cJSON_IsString(label)) return cJSON_GetStringValue(label);
    }

    return def;
}

/* -- Extract service name from image (strip registry prefix and tag) ------ */

static void extract_image_name(const char *image, char *out, size_t out_sz)
{
    const char *slash = strrchr(image, '/');
    const char *base  = slash ? slash + 1 : image;
    const char *colon = strchr(base, ':');
    size_t      len   = colon ? (size_t)(colon - base) : strlen(base);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* -- Port detection: CONSUL_SERVICE_PORT > HostPort > Ports keys > EXPOSE - */

static void detect_ports(cJSON *inspect, ServiceDef *svc)
{
    svc->port_count  = 0;
    svc->host_mapped = 0;

    /* helper: port mappings object from NetworkSettings */
    cJSON *netset    = cJSON_GetObjectItem(inspect, "NetworkSettings");
    cJSON *ports_obj = cJSON_GetObjectItem(netset, "Ports");

    /* 1. CONSUL_SERVICE_PORT (always refers to internal/container port) */
    const char *manual = get_config(inspect, "CONSUL_SERVICE_PORT", NULL);
    if (manual && manual[0]) {
        int internal_port = atoi(manual);
        if (internal_port > 0) {
            int host_port     = internal_port;   /* default: same as internal */
            int found_mapping = 0;

            /* look up corresponding HostPort for this internal port */
            if (cJSON_IsObject(ports_obj)) {
                cJSON *entry;
                cJSON_ArrayForEach(entry, ports_obj) {
                    if (!entry->string) continue;
                    int cp = atoi(entry->string);    /* e.g. "3000/tcp" -> 3000 */
                    if (cp == internal_port && cJSON_IsArray(entry)) {
                        cJSON *binding;
                        cJSON_ArrayForEach(binding, entry) {
                            cJSON      *hp     = cJSON_GetObjectItem(binding, "HostPort");
                            const char *hp_str = cJSON_GetStringValue(hp);
                            if (hp_str && hp_str[0]) {
                                int hp_val = atoi(hp_str);
                                if (hp_val > 0) {
                                    host_port     = hp_val;
                                    found_mapping = 1;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }

            svc->ports[0].host_port      = host_port;
            svc->ports[0].container_port = internal_port;
            svc->port_count              = 1;
            svc->host_mapped             = found_mapping;
        }
        return;
    }

    /* 2. Port mapping HostPort (deduplicate: IPv4 + IPv6 binding same port counted once) */
    if (cJSON_IsObject(ports_obj)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, ports_obj) {
            if (!cJSON_IsArray(entry)) continue;
            int container_port = entry->string ? atoi(entry->string) : 0;
            cJSON *binding;
            cJSON_ArrayForEach(binding, entry) {
                cJSON      *hp     = cJSON_GetObjectItem(binding, "HostPort");
                const char *hp_str = cJSON_GetStringValue(hp);
                if (!hp_str || !hp_str[0]) continue;
                int host_port = atoi(hp_str);
                if (host_port <= 0 || svc->port_count >= MAX_PORTS) continue;
                /* check for duplicates by host port */
                int dup = 0;
                for (int k = 0; k < svc->port_count; k++) {
                    if (svc->ports[k].host_port == host_port) { dup = 1; break; }
                }
                if (!dup) {
                    svc->ports[svc->port_count].host_port      = host_port;
                    svc->ports[svc->port_count].container_port = container_port > 0 ? container_port : host_port;
                    svc->port_count++;
                }
            }
        }
    }
    if (svc->port_count > 0) {
        svc->host_mapped = 1;
        return;
    }

    /* 3. NetworkSettings.Ports keys (ports known to Docker but not host-mapped,
     *    e.g. compose "expose:" or compose network ports with null bindings) */
    if (cJSON_IsObject(ports_obj)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, ports_obj) {
            if (entry->string && svc->port_count < MAX_PORTS) {
                int port = atoi(entry->string);
                if (port > 0) {
                    int dup = 0;
                    for (int k = 0; k < svc->port_count; k++) {
                        if (svc->ports[k].container_port == port) { dup = 1; break; }
                    }
                    if (!dup) {
                        svc->ports[svc->port_count].host_port      = port;
                        svc->ports[svc->port_count].container_port = port;
                        svc->port_count++;
                    }
                }
            }
        }
    }
    if (svc->port_count > 0) return;

    /* 4. EXPOSE from Dockerfile (Config.ExposedPorts) */
    cJSON *config  = cJSON_GetObjectItem(inspect, "Config");
    cJSON *exposed = cJSON_GetObjectItem(config, "ExposedPorts");
    if (cJSON_IsObject(exposed)) {
        cJSON *ep;
        cJSON_ArrayForEach(ep, exposed) {
            if (ep->string && svc->port_count < MAX_PORTS) {
                int port = atoi(ep->string);
                if (port > 0) {
                    svc->ports[svc->port_count].host_port      = port;
                    svc->ports[svc->port_count].container_port = port;
                    svc->port_count++;
                }
            }
        }
    }
    if (svc->port_count > 0) return;

    /* 5. No port detected -- leave port_count = 0.
     * Services without any port (no CONSUL_SERVICE_PORT, no HostPort mapping,
     * no EXPOSE) should not be registered with Consul, as a fabricated default
     * port would create a health check that always fails. */
}

/* -- Comma-separated tags -> JSON array string ---------------------------- */

static void tags_to_json(const char *tags_str, char *out, size_t out_sz)
{
    if (!tags_str || !tags_str[0]) {
        snprintf(out, out_sz, "[]");
        return;
    }
    cJSON *arr = cJSON_CreateArray();
    char   tmp[512];
    strncpy(tmp, tags_str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(tmp, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';
        if (*tok) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
        tok = strtok_r(NULL, ",", &saveptr);
    }
    char *json = cJSON_PrintUnformatted(arr);
    snprintf(out, out_sz, "%s", json ? json : "[]");
    free(json);
    cJSON_Delete(arr);
}

/* -- Resolve host IP address ---------------------------------------------- */

/*
 * Get the host's outbound IP by creating a UDP socket "connected" to an
 * external address (no packet is actually sent). This returns the source IP
 * the kernel would choose for outbound traffic -- i.e. the host's primary
 * private/internal IP, which is what we want for Consul service Address.
 *
 * Priority: ADVERTISE_ADDR env > getaddrinfo(hostname) > UDP connect trick
 */
static int get_outbound_ip(char *ip_buf, size_t ip_buf_sz)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &serv.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &len) < 0) {
        close(sock);
        return -1;
    }

    inet_ntop(AF_INET, &local.sin_addr, ip_buf, (socklen_t)ip_buf_sz);
    close(sock);
    return 0;
}

static void resolve_host_ip(Config *cfg)
{
    /* 1. Explicit env var override */
    const char *adv = getenv("ADVERTISE_ADDR");
    if (adv && adv[0]) {
        strncpy(cfg->host_ip, adv, sizeof(cfg->host_ip) - 1);
        return;
    }

    /* 2. Try getaddrinfo on hostname (skip if resolves to 127.x.x.x) */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg->hostname, NULL, &hints, &res) == 0 && res) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        char tmp[64];
        inet_ntop(AF_INET, &addr->sin_addr, tmp, sizeof(tmp));
        freeaddrinfo(res);
        if (strncmp(tmp, "127.", 4) != 0) {
            strncpy(cfg->host_ip, tmp, sizeof(cfg->host_ip) - 1);
            return;
        }
    } else if (res) {
        freeaddrinfo(res);
    }

    /* 3. UDP connect trick -- get outbound IP */
    if (get_outbound_ip(cfg->host_ip, sizeof(cfg->host_ip)) == 0)
        return;

    /* 4. Fallback to hostname (old behavior) */
    strncpy(cfg->host_ip, cfg->hostname, sizeof(cfg->host_ip) - 1);
}

/* -- Extract container IP from inspect JSON -------------------------------- */

static void get_container_ip(cJSON *inspect, char *ip_buf, size_t ip_buf_sz)
{
    ip_buf[0] = '\0';
    cJSON *netset = cJSON_GetObjectItem(inspect, "NetworkSettings");
    if (!netset) return;

    /* 1. NetworkSettings.IPAddress (default bridge) */
    cJSON      *ip_j = cJSON_GetObjectItem(netset, "IPAddress");
    const char *ip   = cJSON_GetStringValue(ip_j);
    if (ip && ip[0]) {
        strncpy(ip_buf, ip, ip_buf_sz - 1);
        return;
    }

    /* 2. NetworkSettings.Networks.<name>.IPAddress (custom networks) */
    cJSON *networks = cJSON_GetObjectItem(netset, "Networks");
    if (cJSON_IsObject(networks)) {
        cJSON *net;
        cJSON_ArrayForEach(net, networks) {
            cJSON      *net_ip_j = cJSON_GetObjectItem(net, "IPAddress");
            const char *net_ip   = cJSON_GetStringValue(net_ip_j);
            if (net_ip && net_ip[0]) {
                strncpy(ip_buf, net_ip, ip_buf_sz - 1);
                return;
            }
        }
    }
}

/* -- Service state tracking (change-only logging) ------------------------- */

/* Returns 1 if service_id is newly registered (state change), 0 if already tracked */
static int track_register(const char *service_id)
{
    int is_new = 1;
    pthread_mutex_lock(&g_tracked_mutex);
    for (int i = 0; i < g_tracked_count; i++) {
        if (strcmp(g_tracked[i], service_id) == 0) { is_new = 0; break; }
    }
    if (is_new && g_tracked_count < MAX_TRACKED) {
        strncpy(g_tracked[g_tracked_count], service_id, 255);
        g_tracked[g_tracked_count][255] = '\0';
        g_tracked_count++;
    }
    pthread_mutex_unlock(&g_tracked_mutex);
    return is_new;
}

/* Remove service_id from tracked set */
static void track_deregister(const char *service_id)
{
    pthread_mutex_lock(&g_tracked_mutex);
    for (int i = 0; i < g_tracked_count; i++) {
        if (strcmp(g_tracked[i], service_id) == 0) {
            if (i < g_tracked_count - 1)
                memcpy(g_tracked[i], g_tracked[g_tracked_count - 1], 256);
            g_tracked_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mutex);
}

/* -- Register a single container ------------------------------------------ */

/* Returns: 1 = container had CONSUL_LISTEN_ENABLE=true, 0 = not enabled / error */
static int register_container(const char *container_id, const Config *cfg)
{
    char  path[256];
    char *body = NULL;

    snprintf(path, sizeof(path), "/containers/%s/json", container_id);
    if (docker_request(cfg, path, &body) != 0 || !body) {
        log_warn("Failed to inspect container: %.12s", container_id);
        free(body);
        return 0;
    }

    cJSON *inspect = cJSON_Parse(body);
    free(body);
    if (!inspect) return 0;

    /* opt-in check */
    const char *enabled = get_config(inspect, "CONSUL_LISTEN_ENABLE", "false");
    if (strcmp(enabled, "true") != 0) {
        cJSON_Delete(inspect);
        return 0;
    }

    /* container name */
    cJSON      *name_j        = cJSON_GetObjectItem(inspect, "Name");
    const char *raw_name      = cJSON_GetStringValue(name_j);
    const char *container_name =
        (raw_name && raw_name[0] == '/') ? raw_name + 1 : raw_name;
    if (!container_name) container_name = "unknown";

    /* default service name = image name */
    cJSON      *cfg_j    = cJSON_GetObjectItem(inspect, "Config");
    cJSON      *image_j  = cJSON_GetObjectItem(cfg_j, "Image");
    const char *image    = cJSON_GetStringValue(image_j);
    char        default_svc_name[128] = "unknown";
    if (image) extract_image_name(image, default_svc_name, sizeof(default_svc_name));

    const char *svc_name = get_config(inspect, "CONSUL_SERVICE_NAME", default_svc_name);
    const char *svc_tags = get_config(inspect, "CONSUL_SERVICE_TAGS", "");

    ServiceDef svc;
    memset(&svc, 0, sizeof(svc));
    strncpy(svc.container_id,   container_id,   sizeof(svc.container_id) - 1);
    strncpy(svc.container_name, container_name, sizeof(svc.container_name) - 1);
    detect_ports(inspect, &svc);

    /* Skip registration if no port was detected -- the container does not
     * expose any port (no CONSUL_SERVICE_PORT, no HostPort, no EXPOSE).
     * Registering it would create a health check on a fabricated port. */
    if (svc.port_count == 0) {
        log_debug("Skipping %s (%.12s): no port detected, service not registered",
                  container_name, container_id);
        cJSON_Delete(inspect);
        return 1;  /* enabled but no port -- still counts as enabled for orphan tracking */
    }

    /* Resolve container IP and detect host network mode */
    char container_ip[64] = "";
    get_container_ip(inspect, container_ip, sizeof(container_ip));
    int is_host_network = (!container_ip[0]);

    /* global check interval and POD_IP */
    const char *check_interval = get_config(inspect, "CONSUL_SERVICE_CHECK_INTERVAL", "10s");
    const char *global_pod_ip  = get_config(inspect, "CONSUL_SERVICE_POD_IP", "true");
    int global_use_pod_ip = (strcmp(global_pod_ip, "true") == 0);

    if (is_host_network && global_use_pod_ip)
        log_debug("POD_IP ignored for %s: container is in host network mode",
                  container_name);

    for (int i = 0; i < svc.port_count; i++) {
        int port  = svc.ports[i].host_port;       /* for Consul registration */
        int cport = svc.ports[i].container_port;   /* for label/env key lookup */

        /* per-port override: labels/env port keys always refer to internal port */
        char key_name[64], key_tags_k[64], key_pod_ip[64];
        snprintf(key_name,   sizeof(key_name),   "CONSUL_SERVICE_%d_NAME",   cport);
        snprintf(key_tags_k, sizeof(key_tags_k), "CONSUL_SERVICE_%d_TAGS",   cport);
        snprintf(key_pod_ip, sizeof(key_pod_ip), "CONSUL_SERVICE_%d_POD_IP", cport);
        const char *port_name   = get_config(inspect, key_name,   svc_name);
        const char *port_tags   = get_config(inspect, key_tags_k, svc_tags);
        const char *port_pod_ip = get_config(inspect, key_pod_ip, NULL);

        /* POD_IP: per-port > global */
        int use_pod_ip = global_use_pod_ip;
        if (port_pod_ip)
            use_pod_ip = (strcmp(port_pod_ip, "true") == 0);

        /* Address/port resolution priority:
         * 1. host network mode → always host IP, POD_IP meaningless
         * 2. POD_IP=true (force) → container IP + internal port
         * 3. port mapped → host IP + host port
         * 4. EXPOSE only → container IP + container port */
        const char *svc_addr;
        int         reg_port;

        if (is_host_network) {
            svc_addr = cfg->host_ip;
            reg_port = cport;
        } else if (use_pod_ip) {
            svc_addr = container_ip;
            reg_port = cport;
        } else if (svc.host_mapped) {
            svc_addr = cfg->host_ip;
            reg_port = port;
        } else {
            svc_addr = container_ip;
            reg_port = cport;
        }

        char tags_json[1024];
        tags_to_json(port_tags, tags_json, sizeof(tags_json));

        char service_id[256];
        snprintf(service_id, sizeof(service_id), "%s:%s:%d",
                 cfg->hostname, container_name, reg_port);

        /* build registration JSON */
        cJSON *reg  = cJSON_CreateObject();
        cJSON *tags_arr = cJSON_Parse(tags_json);
        cJSON *meta = cJSON_CreateObject();

        cJSON_AddStringToObject(reg, "ID",      service_id);
        cJSON_AddStringToObject(reg, "Name",    port_name);
        cJSON_AddStringToObject(reg, "Address", svc_addr);
        cJSON_AddNumberToObject(reg, "Port",    (double)reg_port);
        cJSON_AddItemToObject(reg, "Tags", tags_arr ? tags_arr : cJSON_CreateArray());

        cJSON_AddStringToObject(meta, "container_id",   container_id);
        cJSON_AddStringToObject(meta, "container_name", container_name);
        cJSON_AddStringToObject(meta, "registrator",    "self-hosted");
        cJSON_AddStringToObject(meta, "by",             cfg->hostname);
        cJSON_AddItemToObject(reg, "Meta", meta);

        /* TCP health check -- always target container IP:internal port when
         * available, since the Consul agent on the host can reach all Docker
         * network subnets via their bridge interfaces. */
        cJSON *check = cJSON_CreateObject();
        char tcp_target[128];
        if (container_ip[0])
            snprintf(tcp_target, sizeof(tcp_target), "%s:%d", container_ip, cport);
        else
            snprintf(tcp_target, sizeof(tcp_target), "%s:%d", svc_addr, reg_port);
        cJSON_AddStringToObject(check, "TCP",      tcp_target);
        cJSON_AddStringToObject(check, "Interval", check_interval);
        cJSON_AddStringToObject(check, "Timeout",  "5s");
        cJSON_AddItemToObject(reg, "Check", check);

        char *reg_str = cJSON_PrintUnformatted(reg);
        cJSON_Delete(reg);

        char *resp = NULL;
        int   rc   = consul_request(cfg, "PUT",
                                    "/v1/agent/service/register", reg_str, &resp);

        /* Consul returns empty body or "null" on success */
        int ok = (rc == 0 && (!resp || resp[0] == '\0' ||
                               strcmp(resp, "null") == 0));
        if (ok) {
            if (track_register(service_id))
                log_info("Registered service: %s (%s) addr=%s port=%d check=tcp/%s",
                         port_name, service_id, svc_addr, reg_port, check_interval);
        } else {
            log_error("Registration failed: %s (%s): %s",
                      port_name, service_id, resp ? resp : "curl error");
        }

        free(resp);
        free(reg_str);
    }

    cJSON_Delete(inspect);
    return 1;
}

/* -- Deregister all services for a container ------------------------------ */

static void deregister_container(const char *container_id,
                                 const char *container_name,
                                 const Config *cfg)
{
    char *resp = NULL;
    if (consul_request(cfg, "GET", "/v1/agent/services", NULL, &resp) != 0 || !resp) {
        free(resp);
        return;
    }

    cJSON *services = cJSON_Parse(resp);
    free(resp);
    if (!services) return;

    cJSON *svc;
    cJSON_ArrayForEach(svc, services) {
        if (!svc->string) continue;

        /* match by Meta.container_id */
        cJSON      *meta  = cJSON_GetObjectItem(svc, "Meta");
        cJSON      *cid_j = cJSON_GetObjectItem(meta, "container_id");
        int         match = 0;

        if (cJSON_IsString(cid_j) &&
            strcmp(cJSON_GetStringValue(cid_j), container_id) == 0) {
            match = 1;
        }
        /* fallback: match by service_id prefix "hostname:name:" */
        if (!match && container_name) {
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "%s:%s:",
                     cfg->hostname, container_name);
            if (strncmp(svc->string, prefix, strlen(prefix)) == 0) match = 1;
        }

        if (match) {
            char path[512];
            snprintf(path, sizeof(path),
                     "/v1/agent/service/deregister/%s", svc->string);
            consul_request(cfg, "PUT", path, NULL, NULL);
            track_deregister(svc->string);
            log_info("Deregistered service: %s", svc->string);
        }
    }
    cJSON_Delete(services);
}

/* -- Clean up orphaned services ------------------------------------------- */

static void cleanup_orphans(char **all_ids, int n_all, const Config *cfg)
{
    char *resp = NULL;
    if (consul_request(cfg, "GET", "/v1/agent/services", NULL, &resp) != 0 || !resp) {
        free(resp);
        return;
    }

    cJSON *services = cJSON_Parse(resp);
    free(resp);
    if (!services) return;

    size_t hlen = strlen(cfg->hostname);

    cJSON *svc;
    cJSON_ArrayForEach(svc, services) {
        if (!svc->string) continue;
        const char *sid = svc->string;

        cJSON      *meta  = cJSON_GetObjectItem(svc, "Meta");
        cJSON      *reg_j = meta ? cJSON_GetObjectItem(meta, "registrator") : NULL;
        int is_self_hosted = (cJSON_IsString(reg_j) &&
                              strcmp(cJSON_GetStringValue(reg_j), "self-hosted") == 0);

        /* Primary: match by "by" meta (set by current version) */
        cJSON      *by_j = meta ? cJSON_GetObjectItem(meta, "by") : NULL;
        int node_match = (cJSON_IsString(by_j) &&
                          strcmp(cJSON_GetStringValue(by_j), cfg->hostname) == 0);

        /* Fallback: hostname prefix in service ID (for older versions
         * that have registrator=self-hosted but no registrator_node) */
        int hostname_match = (is_self_hosted &&
                              strncmp(sid, cfg->hostname, hlen) == 0 &&
                              sid[hlen] == ':');

        if (!node_match && !hostname_match) continue;

        cJSON      *cid_j = meta ? cJSON_GetObjectItem(meta, "container_id") : NULL;
        const char *cid   = cJSON_GetStringValue(cid_j);

        int found = 0;
        if (cid) {
            for (int i = 0; i < n_all; i++) {
                if (strcmp(all_ids[i], cid) == 0) { found = 1; break; }
            }
        }
        /* No container_id in meta but hostname matches → cannot verify,
         * treat as orphaned since it was likely registered by us. */

        if (!found) {
            track_deregister(sid);
            log_info("Cleaning orphaned service: %s (container %.12s no longer exists)",
                     sid, cid ? cid : "unknown");
            char path[512];
            snprintf(path, sizeof(path),
                     "/v1/agent/service/deregister/%s", sid);
            consul_request(cfg, "PUT", path, NULL, NULL);
        }
    }
    cJSON_Delete(services);
}

/* -- Full sync ------------------------------------------------------------ */

static void sync_all(const Config *cfg)
{
    log_debug("Starting full sync...");

    char *resp = NULL;
    if (docker_request(cfg, "/containers/json", &resp) != 0 || !resp) {
        log_warn("Failed to list containers");
        free(resp);
        return;
    }

    cJSON *containers = cJSON_Parse(resp);
    free(resp);
    if (!cJSON_IsArray(containers)) {
        cJSON_Delete(containers);
        return;
    }

    int    n       = cJSON_GetArraySize(containers);
    char **all_ids = calloc((size_t)n, sizeof(char *));
    int    all_cnt = 0;

    for (int i = 0; i < n; i++) {
        cJSON      *c   = cJSON_GetArrayItem(containers, i);
        cJSON      *id_j = cJSON_GetObjectItem(c, "Id");
        const char *cid  = cJSON_GetStringValue(id_j);
        if (!cid) continue;

        if (all_cnt < n)
            all_ids[all_cnt++] = strdup(cid);

        /* register_container returns 1 if CONSUL_LISTEN_ENABLE=true */
        register_container(cid, cfg);
    }
    cJSON_Delete(containers);

    cleanup_orphans(all_ids, all_cnt, cfg);

    for (int i = 0; i < all_cnt; i++) free(all_ids[i]);
    free(all_ids);

    log_debug("Full sync completed");
}

/* -- Enqueue event (called from curl write callback, no I/O allowed) ------ */

static void enqueue_event(const char *action, const char *container_id,
                          const char *container_name)
{
    QEvent *ev = calloc(1, sizeof(QEvent));
    if (!ev) return;
    strncpy(ev->action,         action,         sizeof(ev->action) - 1);
    strncpy(ev->container_id,   container_id,   sizeof(ev->container_id) - 1);
    strncpy(ev->container_name,
            container_name ? container_name : "",
            sizeof(ev->container_name) - 1);

    pthread_mutex_lock(&g_eq_mutex);
    if (g_eq_tail) { g_eq_tail->next = ev; g_eq_tail = ev; }
    else           { g_eq_head = g_eq_tail = ev; }
    pthread_cond_signal(&g_eq_cond);
    pthread_mutex_unlock(&g_eq_mutex);
}

/* -- Parse event line (enqueue only, no I/O) ------------------------------ */

static void process_event_line(const char *line)
{
    /* fast string pre-filter: only handle type=container with action in {start,stop,die} */
    if (!strstr(line, "\"Type\":\"container\"")) return;
    if (!strstr(line, "\"Action\":\"start\"") &&
        !strstr(line, "\"Action\":\"stop\"")  &&
        !strstr(line, "\"Action\":\"die\""))
        return;

    cJSON *event = cJSON_Parse(line);
    if (!event) return;

    cJSON      *action_j     = cJSON_GetObjectItem(event, "Action");
    cJSON      *actor        = cJSON_GetObjectItem(event, "Actor");
    cJSON      *id_j         = cJSON_GetObjectItem(actor, "ID");   /* Actor.ID */
    cJSON      *attrs        = cJSON_GetObjectItem(actor, "Attributes");
    cJSON      *name_j       = cJSON_GetObjectItem(attrs, "name");

    const char *action         = cJSON_GetStringValue(action_j);
    const char *container_id   = cJSON_GetStringValue(id_j);
    const char *container_name = cJSON_GetStringValue(name_j);

    if (action && container_id &&
        (strcmp(action, "start") == 0 ||
         strcmp(action, "stop")  == 0 ||
         strcmp(action, "die")   == 0)) {
        enqueue_event(action, container_id, container_name);
    }

    cJSON_Delete(event);
}

/* -- Event processor thread (dequeue and call Consul API) ----------------- */

static void *event_processor_thread(void *arg)
{
    Config *cfg = (Config *)arg;

    while (g_running) {
        pthread_mutex_lock(&g_eq_mutex);
        while (!g_eq_head && g_running)
            pthread_cond_wait(&g_eq_cond, &g_eq_mutex);
        QEvent *ev = g_eq_head;
        if (ev) {
            g_eq_head = ev->next;
            if (!g_eq_head) g_eq_tail = NULL;
        }
        pthread_mutex_unlock(&g_eq_mutex);

        if (!ev) continue;

        if (strcmp(ev->action, "start") == 0) {
            log_info("Container started: %s (%.12s...)",
                     ev->container_name[0] ? ev->container_name : "?",
                     ev->container_id);
            register_container(ev->container_id, cfg);
        } else {
            log_info("Container stopped [%s]: %s (%.12s...)", ev->action,
                     ev->container_name[0] ? ev->container_name : "?",
                     ev->container_id);
            deregister_container(ev->container_id, ev->container_name, cfg);
        }
        free(ev);
    }
    return NULL;
}

/* -- Event stream write callback ------------------------------------------ */

static size_t event_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    EventCtx *ctx   = (EventCtx *)userdata;
    size_t    total = size * nmemb;

    if (buf_append(&ctx->buf, ptr, total) != 0) return 0;

    /* process complete JSON lines */
    char *start = ctx->buf.data;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start && *(nl - 1) == '\r') *(nl - 1) = '\0';
        if (*start) process_event_line(start);  /* enqueue only, no I/O */
        start = nl + 1;
    }

    /* remove processed data */
    size_t remaining = ctx->buf.len - (size_t)(start - ctx->buf.data);
    if (remaining > 0 && start != ctx->buf.data)
        memmove(ctx->buf.data, start, remaining);
    ctx->buf.len          = remaining;
    ctx->buf.data[remaining] = '\0';

    return total;
}

/* -- Event watch main loop ------------------------------------------------ */

/* Progress callback: abort curl when shutdown is requested */
static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return g_running ? 0 : 1;   /* non-zero aborts the transfer */
}

static void watch_events(Config *cfg)
{
    log_info("Watching Docker events...");

    /* No filter params in URL: avoids libcurl 8.3+ re-encoding already-encoded
     * '%' in URLs. Type+action filtering is done in process_event_line() via strstr. */
    const char url[] = "http://localhost/events";

    EventCtx ctx;
    buf_init(&ctx.buf);

    while (g_running) {
        CURL *curl = curl_easy_init();
        if (!curl) { sleep(1); continue; }

        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, cfg->docker_sock);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, event_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);        /* no timeout */
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);       /* thread-safe */
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);     /* enable progress cb */

        CURLcode rc = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (g_running) {
            log_warn("Event stream disconnected (rc=%d), reconnecting in 5s...", (int)rc);
            for (int i = 0; i < 5 && g_running; i++)
                sleep(1);
        }
    }

    buf_free(&ctx.buf);
}

/* -- Periodic resync thread ----------------------------------------------- */

static void *resync_thread(void *arg)
{
    Config *cfg = (Config *)arg;
    while (g_running) {
        /* Sleep in 1-second increments for quick shutdown response */
        for (int i = 0; i < cfg->resync_interval && g_running; i++)
            sleep(1);
        if (g_running) sync_all(cfg);
    }
    return NULL;
}

/* -- Signal handler ------------------------------------------------------- */

static void sig_handler(int signum)
{
    (void)signum;
    g_running = 0;
}

/* -- Preflight checks ----------------------------------------------------- */

static int preflight_check(const Config *cfg)
{
    char *resp = NULL;

    if (docker_request(cfg, "/info", &resp) != 0 || !resp) {
        log_error("Cannot connect to Docker API: %s", cfg->docker_sock);
        free(resp);
        return -1;
    }
    free(resp);

    resp = NULL;
    if (consul_request(cfg, "GET", "/v1/status/leader", NULL, &resp) != 0
        || !resp || resp[0] == '\0') {
        log_error("Cannot connect to Consul: %s", cfg->consul_addr);
        free(resp);
        return -1;
    }
    free(resp);

    log_info("Preflight OK -- Docker API and Consul are reachable");
    return 0;
}

/* -- main ----------------------------------------------------------------- */

int main(void)
{
    Config      cfg;
    const char *v;

    memset(&cfg, 0, sizeof(cfg));

    v = getenv("CONSUL_ADDR");
    strncpy(cfg.consul_addr, v ? v : DEFAULT_CONSUL_ADDR,
            sizeof(cfg.consul_addr) - 1);

    v = getenv("REGISTRATOR_TOKEN");
    if (v) strncpy(cfg.consul_token, v, sizeof(cfg.consul_token) - 1);

    v = getenv("DOCKER_SOCK");
    strncpy(cfg.docker_sock, v ? v : DEFAULT_DOCKER_SOCK,
            sizeof(cfg.docker_sock) - 1);

    v = getenv("RESYNC_INTERVAL");
    cfg.resync_interval = v ? atoi(v) : DEFAULT_RESYNC_INTERVAL;
    if (cfg.resync_interval <= 0) cfg.resync_interval = DEFAULT_RESYNC_INTERVAL;

    gethostname(cfg.hostname, sizeof(cfg.hostname) - 1);
    v = getenv("HOSTNAME_OVERRIDE");
    if (v) strncpy(cfg.hostname, v, sizeof(cfg.hostname) - 1);

    resolve_host_ip(&cfg);

    v = getenv("LOG_LEVEL");
    if (v && (strcmp(v, "debug") == 0 || strcmp(v, "DEBUG") == 0))
        g_log_debug = 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Use sigaction WITHOUT SA_RESTART so signals interrupt blocking syscalls
     * (e.g. curl_easy_perform's underlying recv()). signal() sets SA_RESTART
     * on Linux/glibc, which would prevent SIGTERM from interrupting curl. */
    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* no SA_RESTART */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    log_info("=========================================");
    log_info("Consul Registrator (C) starting");
    log_info("Consul:   %s", cfg.consul_addr);
    log_info("Docker:   %s", cfg.docker_sock);
    log_info("Resync:   every %ds", cfg.resync_interval);
    log_info("Hostname: %s", cfg.hostname);
    log_info("Host IP:  %s", cfg.host_ip);
    log_info("Mode:     opt-in (CONSUL_LISTEN_ENABLE=true)");
    log_info("=========================================");

    if (preflight_check(&cfg) != 0) {
        curl_global_cleanup();
        return 1;
    }

    sync_all(&cfg);

    /* Block signals in child threads so SIGTERM is delivered only to the
     * main thread (which runs curl_easy_perform and can be interrupted). */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    pthread_t resync_tid, evproc_tid;
    pthread_create(&resync_tid, NULL, resync_thread, &cfg);
    pthread_create(&evproc_tid, NULL, event_processor_thread, &cfg);

    /* Unblock signals in main thread */
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);

    watch_events(&cfg); /* blocks until g_running = 0 */

    /* wake event_processor_thread so it exits */
    pthread_mutex_lock(&g_eq_mutex);
    pthread_cond_broadcast(&g_eq_cond);
    pthread_mutex_unlock(&g_eq_mutex);

    pthread_join(evproc_tid, NULL);
    pthread_join(resync_tid, NULL);

    log_info("Registrator stopped");
    curl_global_cleanup();
    return 0;
}
