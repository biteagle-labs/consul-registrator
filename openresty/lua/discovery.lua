local cjson = require("cjson.safe")
local http  = require("resty.http")

local _M = {}

local CONSUL_ADDR
local CONSUL_TOKEN
local DOMAIN_SUFFIX
local POLL_INTERVAL
local FILTER_TAGS

-- state for incremental route updates and change detection
local prev_names    = {}
local prev_snapshot = ""

local function init_config()
    CONSUL_ADDR   = os.getenv("CONSUL_ADDR")   or "http://127.0.0.1:8500"
    CONSUL_TOKEN  = os.getenv("CONSUL_TOKEN")   or ""
    DOMAIN_SUFFIX = os.getenv("DOMAIN_SUFFIX")  or "svc.local"
    POLL_INTERVAL = tonumber(os.getenv("POLL_INTERVAL")) or 5

    local raw = os.getenv("FILTER_TAGS") or "web,prometheus"
    FILTER_TAGS = {}
    for tag in raw:gmatch("[^,]+") do
        FILTER_TAGS[tag:match("^%s*(.-)%s*$")] = true
    end
end

local function has_matching_tag(tags)
    if not tags then return false end
    for _, tag in ipairs(tags) do
        if FILTER_TAGS[tag] then return true end
    end
    return false
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

    -- group backends by service name
    local table_new = {}
    for _, svc in pairs(services) do
        if has_matching_tag(svc.Tags) and svc.Address and svc.Address ~= ""
           and svc.Port and svc.Port > 0 then
            local name = svc.Service
            if not table_new[name] then
                table_new[name] = {}
            end
            table_new[name][#table_new[name] + 1] = {
                addr = svc.Address,
                port = svc.Port,
            }
        end
    end

    -- incremental update: set new/updated routes, remove stale ones
    -- (preserves rr: counters for services that still exist)
    local curr_names = {}
    for name, backends in pairs(table_new) do
        routes:set(name, cjson.encode(backends))
        curr_names[name] = true
    end
    for name in pairs(prev_names) do
        if not curr_names[name] then
            routes:delete(name)
            routes:delete("rr:" .. name)
        end
    end
    prev_names = curr_names

    -- change-only logging: service name -> backend IPs
    local log_parts = {}
    for name, backends in pairs(table_new) do
        local addrs = {}
        for _, b in ipairs(backends) do
            addrs[#addrs + 1] = b.addr .. ":" .. b.port
        end
        table.sort(addrs)
        log_parts[#log_parts + 1] = name .. " -> " .. table.concat(addrs, ", ")
    end
    table.sort(log_parts)
    local snapshot = table.concat(log_parts, " | ")

    if snapshot ~= prev_snapshot then
        if #log_parts > 0 then
            ngx.log(ngx.NOTICE, "[discovery] ", snapshot)
        else
            ngx.log(ngx.NOTICE, "[discovery] no routable services")
        end
        prev_snapshot = snapshot
    end
end

function _M.start()
    init_config()

    ngx.log(ngx.NOTICE, "[discovery] polling every ", POLL_INTERVAL, "s")

    -- initial poll
    local ok, err = ngx.timer.at(0, poll_consul)
    if not ok then
        ngx.log(ngx.ERR, "[discovery] failed to start initial poll: ", err)
    end

    -- periodic poll
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

    -- strip port if present
    host = host:gsub(":%d+$", "")

    -- strip domain suffix to get service name
    local suffix = "." .. DOMAIN_SUFFIX
    if host:sub(-#suffix) ~= suffix then
        return ngx.exit(ngx.HTTP_NOT_FOUND)
    end

    local name = host:sub(1, #host - #suffix)
    if name == "" then
        return ngx.exit(ngx.HTTP_NOT_FOUND)
    end

    local routes = ngx.shared.routes
    local data = routes:get(name)
    if not data then
        ngx.log(ngx.WARN, "[discovery] no route: ", name)
        return ngx.exit(ngx.HTTP_SERVICE_UNAVAILABLE)
    end

    local backends = cjson.decode(data)
    if not backends or #backends == 0 then
        return ngx.exit(ngx.HTTP_SERVICE_UNAVAILABLE)
    end

    -- round-robin
    local idx = (routes:incr("rr:" .. name, 1, 0) or 1)
    local backend = backends[(idx - 1) % #backends + 1]

    ngx.var.target = backend.addr .. ":" .. backend.port
end

return _M
