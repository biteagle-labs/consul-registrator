local cjson = require("cjson.safe")
local http  = require("resty.http")

local _M = {}

local CONSUL_ADDR
local CONSUL_TOKEN
local POLL_INTERVAL
local NODE_NAME       -- consul node name (env override or agent/self)
local CONFIG_KV_KEY   -- "gateway/<node>/configuration", set once node is known

-- runtime config loaded from Consul KV (per-worker)
local GROUPS        = {}   -- { {name=, suffixes={...}, tags={tag=true}}, ... }
local SUFFIX_INDEX  = {}   -- { {suffix=, gname=}, ... } sorted by #suffix desc
local config_loaded = false

local prev_names      = {} -- set of full keys "<gname>:<svc>"
local prev_snapshot   = ""
local prev_config_raw = "" -- last KV value, for change detection

local function init_config()
    CONSUL_ADDR   = os.getenv("CONSUL_ADDR")  or "http://127.0.0.1:8500"
    CONSUL_TOKEN  = os.getenv("CONSUL_TOKEN") or ""
    POLL_INTERVAL = tonumber(os.getenv("POLL_INTERVAL")) or 5

    -- explicit override; otherwise resolved from the local agent (agent/self)
    local n = os.getenv("NODE_NAME")
    if n and n ~= "" then NODE_NAME = n end
end

-- Resolve the local Consul node name and derive the per-node config key
-- "gateway/<node>/configuration". NODE_NAME env wins; otherwise ask the local
-- agent (GET /v1/agent/self -> Config.NodeName). Returns true once the key is
-- known; on failure the next poll retries.
local function resolve_node(httpc, headers)
    if CONFIG_KV_KEY then return true end

    if not NODE_NAME then
        local res = httpc:request_uri(CONSUL_ADDR .. "/v1/agent/self", {
            method = "GET", headers = headers,
        })
        if not res then
            ngx.log(ngx.ERR, "[discovery] agent/self unreachable; node unknown")
            return false
        end
        if res.status ~= 200 then
            ngx.log(ngx.ERR, "[discovery] agent/self returned ", res.status,
                " (token may lack agent:read); node unknown")
            return false
        end
        local info = cjson.decode(res.body)
        local node = info and info.Config and info.Config.NodeName
        if type(node) ~= "string" or node == "" then
            ngx.log(ngx.ERR, "[discovery] agent/self missing Config.NodeName")
            return false
        end
        NODE_NAME = node
    end

    CONFIG_KV_KEY = "gateway/" .. NODE_NAME .. "/configuration"
    ngx.log(ngx.NOTICE, "[discovery] node '", NODE_NAME,
        "', config key '", CONFIG_KV_KEY, "'")
    return true
end

-- Build GROUPS + SUFFIX_INDEX from a decoded JSON array. Constructs into
-- locals and swaps them in atomically at the end, so route() never observes
-- a half-built index.
local function rebuild_groups(arr)
    local groups, index, seen, reachable = {}, {}, {}, {}
    for _, g in ipairs(arr) do
        if type(g.name) == "string" and g.name ~= ""
           and type(g.suffixes) == "table" and type(g.tags) == "table" then
            local tagset = {}
            for _, t in ipairs(g.tags) do
                tagset[t] = true
            end
            groups[#groups + 1] = {
                name     = g.name,
                suffixes = g.suffixes,
                tags     = tagset,
            }
            for _, suf in ipairs(g.suffixes) do
                if seen[suf] then
                    ngx.log(ngx.WARN, "[discovery] duplicate suffix '", suf,
                        "' ignored (already owned by group '", seen[suf], "')")
                else
                    seen[suf] = g.name
                    reachable[g.name] = true
                    index[#index + 1] = { suffix = suf, gname = g.name }
                end
            end
        else
            ngx.log(ngx.ERR, "[discovery] skipping malformed group entry")
        end
    end
    -- flag groups whose suffixes were all shadowed by earlier groups: unroutable
    for _, g in ipairs(groups) do
        if not reachable[g.name] then
            ngx.log(ngx.ERR, "[discovery] group '", g.name,
                "' has no reachable suffix (all shadowed by earlier groups); unroutable")
        end
    end

    -- longest suffix first, so route() matches the most specific suffix
    table.sort(index, function(a, b) return #a.suffix > #b.suffix end)
    GROUPS, SUFFIX_INDEX = groups, index
end

local function has_matching_tag(tags, group_tags)
    if not tags then return false end
    for _, tag in ipairs(tags) do
        if group_tags[tag] then return true end
    end
    return false
end

-- Pull route config from Consul KV. On any failure the previously loaded
-- config is kept (pure-KV: never fall back to env). 403 / 404 / other are
-- logged distinctly so ACL vs missing-key issues are obvious.
local function fetch_config(httpc, headers)
    local res, err = httpc:request_uri(
        CONSUL_ADDR .. "/v1/kv/" .. CONFIG_KV_KEY .. "?raw", {
            method  = "GET",
            headers = headers,
        })

    if not res then
        ngx.log(ngx.ERR, "[discovery] KV unreachable: ", err, " (keep cached)")
        return
    end
    if res.status == 403 then
        ngx.log(ngx.ERR, "[discovery] KV 403: token lacks read on '",
            CONFIG_KV_KEY, "' (keep cached)")
        return
    end
    if res.status == 404 then
        ngx.log(ngx.WARN, "[discovery] KV key absent: '", CONFIG_KV_KEY,
            "' (keep cached)")
        return
    end
    if res.status ~= 200 then
        ngx.log(ngx.ERR, "[discovery] KV returned ", res.status, " (keep cached)")
        return
    end

    if res.body == prev_config_raw then
        return -- unchanged, skip rebuild
    end

    local arr = cjson.decode(res.body)
    if type(arr) ~= "table" then
        ngx.log(ngx.ERR, "[discovery] KV value is not valid JSON (keep cached)")
        return
    end

    rebuild_groups(arr)
    prev_config_raw = res.body
    config_loaded   = true
    ngx.log(ngx.NOTICE, "[discovery] config reloaded: ", #GROUPS, " group(s)")
end

local function poll_consul(premature)
    if premature then return end

    local routes = ngx.shared.routes

    local httpc = http.new()
    httpc:set_timeout(5000)

    local headers = { ["Accept"] = "application/json" }
    if CONSUL_TOKEN ~= "" then
        headers["X-Consul-Token"] = CONSUL_TOKEN
    end

    -- resolve this node's per-node config key, then refresh config;
    -- pure-KV: no node or no config => no routes
    if not resolve_node(httpc, headers) then
        return
    end
    fetch_config(httpc, headers)
    if not config_loaded then
        ngx.log(ngx.ERR, "[discovery] no config loaded from KV; all routes 404")
        return
    end

    local res, err = httpc:request_uri(CONSUL_ADDR .. "/v1/agent/services", {
        method  = "GET",
        headers = headers,
    })

    if not res then
        ngx.log(ngx.ERR, "[discovery] consul poll failed: ", err)
        return
    end

    if res.status ~= 200 then
        ngx.log(ngx.ERR, "[discovery] consul returned ", res.status)
        return
    end

    local services, decode_err = cjson.decode(res.body)
    if not services then
        ngx.log(ngx.ERR, "[discovery] json decode error: ", decode_err)
        return
    end

    -- group backends by (group, service); shared-dict key = "<gname>:<service>"
    local table_new = {} -- key -> backends array
    local meta      = {} -- key -> { svc=, suffixes= } for change-only logging
    for _, svc in pairs(services) do
        if svc.Address and svc.Address ~= "" and svc.Port and svc.Port > 0 then
            for _, g in ipairs(GROUPS) do
                if has_matching_tag(svc.Tags, g.tags) then
                    local key = g.name .. ":" .. svc.Service
                    if not table_new[key] then
                        table_new[key] = {}
                        meta[key] = { svc = svc.Service, suffixes = g.suffixes }
                    end
                    local arr = table_new[key]
                    arr[#arr + 1] = { addr = svc.Address, port = svc.Port }
                end
            end
        end
    end

    -- incremental update (preserves rr counters; vanished groups are dropped
    -- automatically, since their keys stop appearing in table_new)
    local curr_names = {}
    for key, backends in pairs(table_new) do
        routes:set(key, cjson.encode(backends))
        curr_names[key] = true
    end
    for key in pairs(prev_names) do
        if not curr_names[key] then
            routes:delete(key)
            routes:delete("rr:" .. key)
        end
    end
    prev_names = curr_names

    -- change-only logging: one line per (service, suffix) pair
    local log_lines = {}
    for key, backends in pairs(table_new) do
        local addrs = {}
        for _, b in ipairs(backends) do
            addrs[#addrs + 1] = b.addr .. ":" .. b.port
        end
        table.sort(addrs)
        local joined = table.concat(addrs, ", ")
        local m = meta[key]
        for _, suffix in ipairs(m.suffixes) do
            log_lines[#log_lines + 1] = m.svc .. "." .. suffix .. " -> " .. joined
        end
    end
    table.sort(log_lines)
    local snapshot = table.concat(log_lines, "\n")

    if snapshot ~= prev_snapshot then
        if #log_lines > 0 then
            for _, line in ipairs(log_lines) do
                ngx.log(ngx.NOTICE, "[discovery] ", line)
            end
        else
            ngx.log(ngx.NOTICE, "[discovery] no routable services")
        end
        prev_snapshot = snapshot
    end
end

function _M.start()
    init_config()
    ngx.log(ngx.NOTICE, "[discovery] polling every ", POLL_INTERVAL,
        "s (per-node config from Consul KV)")

    local ok, err = ngx.timer.at(0, poll_consul)
    if not ok then
        ngx.log(ngx.ERR, "[discovery] failed to start initial poll: ", err)
    end

    local ok2, err2 = ngx.timer.every(POLL_INTERVAL, poll_consul)
    if not ok2 then
        ngx.log(ngx.ERR, "[discovery] failed to start periodic poll: ", err2)
    end
end

function _M.route()
    local host = ngx.var.host
    if not host then
        return ngx.exit(ngx.HTTP_BAD_REQUEST)
    end

    host = host:gsub(":%d+$", "")

    -- Two host forms share the same suffix set. host == suffix is checked
    -- first, so an exact suffix always means the path form:
    --   path form:      "<suffix>/<service>/<rest>" -> service is the 1st path
    --                   segment, which is stripped before proxying
    --   subdomain form: "<service>.<suffix>"        -> service is in the host
    local name, gname
    local exact_suffix = false

    for _, e in ipairs(SUFFIX_INDEX) do
        if host == e.suffix then
            exact_suffix = true
            local svc = ngx.var.uri:match("^/([^/]+)")
            if svc then
                local rest = ngx.var.uri:sub(#svc + 2)
                if rest == "" then rest = "/" end
                ngx.req.set_uri(rest)          -- strip the "/<service>" prefix
                name  = svc
                gname = e.gname
            end
            break
        end
    end

    if not exact_suffix then
        -- longest-suffix-first (SUFFIX_INDEX is sorted desc by #suffix)
        for _, e in ipairs(SUFFIX_INDEX) do
            local s = "." .. e.suffix
            if #host > #s and host:sub(-#s) == s then
                name  = host:sub(1, #host - #s)
                gname = e.gname
                break
            end
        end
    end

    if not name or name == "" then
        return ngx.exit(ngx.HTTP_NOT_FOUND)
    end

    local routes = ngx.shared.routes
    local key = gname .. ":" .. name
    local data = routes:get(key)
    if not data then
        ngx.log(ngx.WARN, "[discovery] no route: ", key)
        return ngx.exit(ngx.HTTP_SERVICE_UNAVAILABLE)
    end

    local backends = cjson.decode(data)
    if not backends or #backends == 0 then
        return ngx.exit(ngx.HTTP_SERVICE_UNAVAILABLE)
    end

    -- round-robin
    local idx = (routes:incr("rr:" .. key, 1, 0) or 1)
    local backend = backends[(idx - 1) % #backends + 1]

    ngx.var.target = backend.addr .. ":" .. backend.port
end

return _M
