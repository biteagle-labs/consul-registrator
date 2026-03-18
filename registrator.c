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

/* -- Constants ------------------------------------------------------------ */

#define MAX_PORTS              32
#define DEFAULT_CONSUL_ADDR    "http://localhost:8500"
#define DEFAULT_DOCKER_SOCK    "/var/run/docker.sock"
#define DEFAULT_RESYNC_INTERVAL 30
#define DEFAULT_PORT_NUM       8080

/* -- Types ---------------------------------------------------------------- */

typedef struct {
    char consul_addr[256];
    char consul_token[256];
    char docker_sock[256];
    int  resync_interval;
    int  default_port;
    char hostname[64];
} Config;

typedef struct {
    int  port;
    char name[128];
    char tags[512];   /* comma-separated */
} PortEntry;

typedef struct {
    char      container_id[128];
    char      container_name[128];
    PortEntry ports[MAX_PORTS];
    int       port_count;
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
    Config *cfg;
} EventCtx;

/* -- Globals -------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t       g_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* Event queue (write callback -> event_processor_thread) */
static QEvent          *g_eq_head  = NULL;
static QEvent          *g_eq_tail  = NULL;
static pthread_mutex_t  g_eq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_eq_cond  = PTHREAD_COND_INITIALIZER;

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
    struct tm *tm = localtime(&t);
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

/* -- Port detection: CONSUL_SERVICE_PORT > HostPort > EXPOSE > default ---- */

static void detect_ports(cJSON *inspect, const Config *cfg, ServiceDef *svc)
{
    svc->port_count = 0;

    /* 1. CONSUL_SERVICE_PORT */
    const char *manual = get_config(inspect, "CONSUL_SERVICE_PORT", NULL);
    if (manual && manual[0]) {
        int port = atoi(manual);
        if (port > 0) {
            svc->ports[0].port = port;
            svc->port_count    = 1;
        }
        return;
    }

    /* 2. Port mapping HostPort (deduplicate: IPv4 + IPv6 binding same port counted once) */
    cJSON *netset    = cJSON_GetObjectItem(inspect, "NetworkSettings");
    cJSON *ports_obj = cJSON_GetObjectItem(netset, "Ports");
    if (cJSON_IsObject(ports_obj)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, ports_obj) {
            if (!cJSON_IsArray(entry)) continue;
            cJSON *binding;
            cJSON_ArrayForEach(binding, entry) {
                cJSON      *hp     = cJSON_GetObjectItem(binding, "HostPort");
                const char *hp_str = cJSON_GetStringValue(hp);
                if (!hp_str || !hp_str[0]) continue;
                int port = atoi(hp_str);
                if (port <= 0 || svc->port_count >= MAX_PORTS) continue;
                /* check for duplicates */
                int dup = 0;
                for (int k = 0; k < svc->port_count; k++) {
                    if (svc->ports[k].port == port) { dup = 1; break; }
                }
                if (!dup) svc->ports[svc->port_count++].port = port;
            }
        }
    }
    if (svc->port_count > 0) return;

    /* 3. EXPOSE */
    cJSON *config  = cJSON_GetObjectItem(inspect, "Config");
    cJSON *exposed = cJSON_GetObjectItem(config, "ExposedPorts");
    if (cJSON_IsObject(exposed)) {
        cJSON *ep;
        cJSON_ArrayForEach(ep, exposed) {
            if (ep->string && svc->port_count < MAX_PORTS) {
                int port = atoi(ep->string);
                if (port > 0) svc->ports[svc->port_count++].port = port;
            }
        }
    }
    if (svc->port_count > 0) return;

    /* 4. Default */
    svc->ports[0].port = cfg->default_port;
    svc->port_count    = 1;
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

    char *tok = strtok(tmp, ",");
    while (tok) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';
        if (*tok) cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
        tok = strtok(NULL, ",");
    }
    char *json = cJSON_PrintUnformatted(arr);
    snprintf(out, out_sz, "%s", json ? json : "[]");
    free(json);
    cJSON_Delete(arr);
}

/* -- Register a single container ------------------------------------------ */

static void register_container(const char *container_id, const Config *cfg)
{
    char  path[256];
    char *body = NULL;

    snprintf(path, sizeof(path), "/containers/%s/json", container_id);
    if (docker_request(cfg, path, &body) != 0 || !body) {
        log_warn("Failed to inspect container: %.12s", container_id);
        free(body);
        return;
    }

    cJSON *inspect = cJSON_Parse(body);
    free(body);
    if (!inspect) return;

    /* opt-in check */
    const char *enabled = get_config(inspect, "CONSUL_LISTEN_ENABLE", "false");
    if (strcmp(enabled, "true") != 0) {
        cJSON_Delete(inspect);
        return;
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
    detect_ports(inspect, cfg, &svc);

    for (int i = 0; i < svc.port_count; i++) {
        int port = svc.ports[i].port;

        /* per-port override */
        char key_name[64], key_tags_k[64];
        snprintf(key_name,   sizeof(key_name),   "CONSUL_SERVICE_%d_NAME", port);
        snprintf(key_tags_k, sizeof(key_tags_k), "CONSUL_SERVICE_%d_TAGS", port);
        const char *port_name = get_config(inspect, key_name,   svc_name);
        const char *port_tags = get_config(inspect, key_tags_k, svc_tags);

        char tags_json[1024];
        tags_to_json(port_tags, tags_json, sizeof(tags_json));

        char service_id[256];
        snprintf(service_id, sizeof(service_id), "%s:%s:%d",
                 cfg->hostname, container_name, port);

        /* build registration JSON */
        cJSON *reg  = cJSON_CreateObject();
        cJSON *tags_arr = cJSON_Parse(tags_json);
        cJSON *meta = cJSON_CreateObject();

        cJSON_AddStringToObject(reg, "ID",      service_id);
        cJSON_AddStringToObject(reg, "Name",    port_name);
        cJSON_AddStringToObject(reg, "Address", cfg->hostname);
        cJSON_AddNumberToObject(reg, "Port",    (double)port);
        cJSON_AddItemToObject(reg, "Tags", tags_arr ? tags_arr : cJSON_CreateArray());

        cJSON_AddStringToObject(meta, "container_id",   container_id);
        cJSON_AddStringToObject(meta, "container_name", container_name);
        cJSON_AddStringToObject(meta, "registrator",    "self-hosted");
        cJSON_AddItemToObject(reg, "Meta", meta);

        char *reg_str = cJSON_PrintUnformatted(reg);
        cJSON_Delete(reg);

        char *resp = NULL;
        int   rc   = consul_request(cfg, "PUT",
                                    "/v1/agent/service/register", reg_str, &resp);

        /* Consul returns empty body or "null" on success */
        int ok = (rc == 0 && (!resp || resp[0] == '\0' ||
                               strcmp(resp, "null") == 0));
        if (ok)
            log_info("Registered service: %s (%s) port=%d", port_name, service_id, port);
        else
            log_error("Registration failed: %s (%s): %s",
                      port_name, service_id, resp ? resp : "curl error");

        free(resp);
        free(reg_str);
    }

    cJSON_Delete(inspect);
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
            log_info("Deregistered service: %s", svc->string);
        }
    }
    cJSON_Delete(services);
}

/* -- Clean up orphaned services ------------------------------------------- */

static void cleanup_orphans(char **valid_ids, int n_ids, const Config *cfg)
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

        /* only process services registered by this registrator */
        cJSON      *meta  = cJSON_GetObjectItem(svc, "Meta");
        cJSON      *reg_j = cJSON_GetObjectItem(meta, "registrator");
        if (!cJSON_IsString(reg_j) ||
            strcmp(cJSON_GetStringValue(reg_j), "self-hosted") != 0) continue;

        cJSON      *cid_j = cJSON_GetObjectItem(meta, "container_id");
        const char *cid   = cJSON_GetStringValue(cid_j);
        if (!cid) continue;

        int found = 0;
        for (int i = 0; i < n_ids; i++) {
            if (strcmp(valid_ids[i], cid) == 0) { found = 1; break; }
        }

        if (!found) {
            log_info("Cleaning orphaned service: %s (container %.12s no longer exists)",
                     svc->string, cid);
            char path[512];
            snprintf(path, sizeof(path),
                     "/v1/agent/service/deregister/%s", svc->string);
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

    int    n         = cJSON_GetArraySize(containers);
    char **valid_ids = calloc((size_t)n, sizeof(char *));
    int    valid_cnt = 0;

    for (int i = 0; i < n; i++) {
        cJSON      *c   = cJSON_GetArrayItem(containers, i);
        cJSON      *id_j = cJSON_GetObjectItem(c, "Id");
        const char *cid  = cJSON_GetStringValue(id_j);
        if (!cid) continue;

        register_container(cid, cfg);

        /* track enabled containers for orphan detection */
        char  ipath[256];
        char *istr = NULL;
        snprintf(ipath, sizeof(ipath), "/containers/%s/json", cid);
        if (docker_request(cfg, ipath, &istr) == 0 && istr) {
            cJSON *ins = cJSON_Parse(istr);
            free(istr);
            if (ins) {
                const char *en = get_config(ins, "CONSUL_LISTEN_ENABLE", "false");
                if (strcmp(en, "true") == 0 && valid_cnt < n)
                    valid_ids[valid_cnt++] = strdup(cid);
                cJSON_Delete(ins);
            }
        }
    }
    cJSON_Delete(containers);

    cleanup_orphans(valid_ids, valid_cnt, cfg);

    for (int i = 0; i < valid_cnt; i++) free(valid_ids[i]);
    free(valid_ids);

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

static void watch_events(Config *cfg)
{
    log_info("Watching Docker events...");

    /* No filter params in URL: avoids libcurl 8.3+ re-encoding already-encoded
     * '%' in URLs. Type+action filtering is done in process_event_line() via strstr. */
    const char url[] = "http://localhost/events";

    EventCtx ctx;
    buf_init(&ctx.buf);
    ctx.cfg = cfg;

    while (g_running) {
        CURL *curl = curl_easy_init();
        if (!curl) { sleep(5); continue; }

        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, cfg->docker_sock);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, event_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);        /* no timeout */
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        CURLcode rc = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (g_running) {
            log_warn("Event stream disconnected (rc=%d), reconnecting in 5s...", (int)rc);
            sleep(5);
        }
    }

    buf_free(&ctx.buf);
}

/* -- Periodic resync thread ----------------------------------------------- */

static void *resync_thread(void *arg)
{
    Config *cfg = (Config *)arg;
    while (g_running) {
        sleep((unsigned int)cfg->resync_interval);
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

    v = getenv("DEFAULT_PORT");
    cfg.default_port = v ? atoi(v) : DEFAULT_PORT_NUM;
    if (cfg.default_port <= 0) cfg.default_port = DEFAULT_PORT_NUM;

    gethostname(cfg.hostname, sizeof(cfg.hostname) - 1);
    v = getenv("HOSTNAME_OVERRIDE");
    if (v) strncpy(cfg.hostname, v, sizeof(cfg.hostname) - 1);

    v = getenv("LOG_LEVEL");
    if (v && (strcmp(v, "debug") == 0 || strcmp(v, "DEBUG") == 0))
        g_log_debug = 1;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGHUP,  sig_handler);

    log_info("=========================================");
    log_info("Consul Registrator (C) starting");
    log_info("Consul:   %s", cfg.consul_addr);
    log_info("Docker:   %s", cfg.docker_sock);
    log_info("Resync:   every %ds", cfg.resync_interval);
    log_info("Default:  port %d", cfg.default_port);
    log_info("Hostname: %s", cfg.hostname);
    log_info("Mode:     opt-in (CONSUL_LISTEN_ENABLE=true)");
    log_info("=========================================");

    if (preflight_check(&cfg) != 0) {
        curl_global_cleanup();
        return 1;
    }

    sync_all(&cfg);

    pthread_t resync_tid, evproc_tid;
    pthread_create(&resync_tid, NULL, resync_thread, &cfg);
    pthread_create(&evproc_tid, NULL, event_processor_thread, &cfg);

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
