local c = require "shaco.c"
local socket = require "socket.c"
local serialize = require "serialize.c"
local error = error
local pairs = pairs
local ipairs = ipairs
local tostring = tostring
local tonumber = tonumber
local assert = assert
local sformat = string.format
local tunpack = table.unpack
local tremove = table.remove
local tinsert = table.insert
local tconcat = table.concat
local cocreate = coroutine.create
local coresume = coroutine.resume
local coyield = coroutine.yield
local corunning = coroutine.running
local traceback = debug.traceback

local _co_pool = {}
local _call_session = {}
local _yield_session_co = {}
local _sleep_co = {}
local _response_co_session = {}
local _response_co_address = {}

local _session_id = 0
local _fork_queue = {}
local _error_queue = {}
local _wakeup_co = {}

-- proto type
local proto = {}
local shaco = {
    TTEXT = 1,
    TLUA = 2,
    TMONITOR = 3,
    TLOG = 4,
    TCMD = 5,
    TRESPONSE = 6,
    TSOCKET = 7,
    TTIME = 8,
    TREMOTE = 9,
    TERROR = 10,
}

-- log
local LOG_DEBUG   =0
local LOG_TRACE   =1
local LOG_INFO    =2
local LOG_WARNING =3
local LOG_ERROR   =4

function shaco.log(level, ...) 
    local argv = {...}
    local t = {}
    for _, v in ipairs(argv) do
        tinsert(t, tostring(v))
    end
    c.log(level, tconcat(t, ' '))
end

shaco.error   = function(...) shaco.log(LOG_ERROR, ...) end
shaco.warning = function(...) shaco.log(LOG_WARNING, ...) end
shaco.info    = function(...) shaco.log(LOG_INFO, ...) end
shaco.trace   = function(...) shaco.log(LOG_TRACE, ...) end
shaco.debug   = function(...) shaco.log(LOG_DEBUG, ...) end

shaco.now = c.now
shaco.command = c.command
shaco.handle = c.handle
shaco.tostring = c.tostring
shaco.topointstring = c.topointstring
shaco.packstring = serialize.serialize_string
shaco.unpackstring = serialize.deserialize_string

function shaco.pack(...)
    return serialize.serialize(serialize.pack(...))
end
function shaco.unpack(p,sz)
    return serialize.deserialize(p)
end

local function gen_session()
    _session_id = _session_id + 1
    if _session_id > 0xffffffff then
        _session_id = 1
    end
    return _session_id
end

local suspend

local function co_create(func)
    local co = tremove(_co_pool)
    if co == nil then
        co = cocreate(function(...)
            func(...)
            while true do
                func = nil
                _co_pool[#_co_pool+1] = co
                func = coyield('EXIT')
                func(coyield())
            end
        end)
    else
        coresume(co, func)
    end
    return co
end

function shaco.send(dest, typename, ...)
    local p = proto[typename]
    return c.send(dest, 0, p.id, p.pack(...))
end

function shaco.call(dest, typename, ...)
    local p = proto[typename]
    local session = gen_session()
    c.send(dest, session, p.id, p.pack(...))
    local ok, ret, sz = coyield('CALL', session)
    _sleep_co[corunning()] = nil
    _call_session[session] = nil
    if not ok then
        -- BREAK can use for timeout call
        error('call error')
    end
    return p.unpack(ret, sz)
end

function shaco.ret(msg, sz)
    return coyield('RETURN', msg, sz)
end

function shaco.response()
    return coyield('RESPONSE')
end

local function dispatch_wakeup()
    local co = next(_wakeup_co)
    if co then
        _wakeup_co[co] = nil
        local session = _sleep_co[co]
        if session then
            -- _yield_session_co if tag _sleep_co can break by wakeup
            _yield_session_co[session] = 'BREAK' 
            return suspend(co, coresume(co, false, 'BREAK'))
        end
    end
end

local function dispatch_error_queue()
    local session = tremove(_error_queue, 1)
    if session then
        if _call_session[session] then
            local co = _yield_session_co[session]
            _yield_session_co[session] = nil
            return suspend(co, coresume(co, false))
        end
    end
end

local function dispatch_error(source, session)
    if _call_session[session] then
        tinsert(_error_queue, session)
    end
end

function suspend(co, result, command, param, sz)
    if not result then
        local session = _response_co_session[co]
        if session then
            local address = _response_co_address[co]
            _response_co_session[co] = nil
            _response_co_address[co] = nil
            c.send(address, session, shaco.TERROR, "")
        end
        error(traceback(co, command))
    end
    if command == 'SLEEP' then
        _yield_session_co[param] = co
        _sleep_co[co] = param
    elseif command == 'CALL' then
        _call_session[param] = true
        _yield_session_co[param] = co
        --_sleep_co[co] = param -- no support BREAK yet
    elseif command == 'RETURN' then
        local session = _response_co_session[co]
        local address = _response_co_address[co]
        if not session then
            error('No session to response')
        end
        local ret = c.send(address, session, shaco.TRESPONSE, param, sz)
        _response_co_session[co] = nil
        _response_co_address[co] = nil
        return suspend(co, coresume(co, ret))
    elseif command == 'RESPONSE' then
        local session = _response_co_session[co]
        local address = _response_co_address[co]
        if not session then
            error(traceback(co, 'Already responsed or No session to response'))
        end
        local function response(msg, sz)
            if _response_co_session[co] == nil then
                error(traceback(co, 'Try response repeat'))
            end
            local ret = c.send(address, session, shaco.TRESPONSE, msg, sz)
            _response_co_session[co] = nil
            _response_co_address[co] = nil
            return ret
        end
        return suspend(co, coresume(co, response))
    elseif command == 'EXIT' then
        local session = _response_co_session[co]
        if session then
            local address = _response_co_address[co]
            _response_co_session[co] = nil
            _response_co_address[co] = nil
            c.send(address, session, shaco.TERROR, "")
        end
    else
        error(traceback(co, 'Suspend unknown command '..command))
    end
    dispatch_wakeup()
    dispatch_error_queue()
end

local function dispatch_message(source, session, typeid, msg, sz)
    local p = proto[typeid]
    if typeid == 8 or -- shaco.TTIME
       typeid == 6 then -- shaco.TRESPONSE 
        local co = _yield_session_co[session] 
        if co == 'BREAK' then -- BREAK by wakeup yet
            _yield_session_co[session] = nil 
        elseif co == nil then
            error(sformat('unknown response %d session %d from %04x', typeid, session, source))
        else
            _yield_session_co[session] = nil
            suspend(co, coresume(co, true, msg, sz))
        end
    else
        local co = co_create(p.dispatch)
        if session > 0 then
            _response_co_session[co] = session
            _response_co_address[co] = source
        end
        suspend(co, coresume(co, source, session, p.unpack(msg, sz)))
    end
end

local function dispatchcb(source, session, typeid, msg, sz)
    local ok, err = xpcall(dispatch_message, traceback, source, session, typeid, msg, sz)
    while true do
        local key, co = next(_fork_queue)
        if not key then
            break
        end
        _fork_queue[key] = nil
        local fok, ferr = xpcall(suspend, traceback, co, coresume(co))
        if not fok then
            if ok then
                ok = false
                err = ferr
            else
                err = err..'\n'..ferr 
            end
        end
    end
    if not ok then
        error(err)
    end
end

function shaco.fork(func, ...)
    local args = {...}
    local co = co_create(function()
        func(tunpack(args))
    end)
    tinsert(_fork_queue, co)
    return co
end

function shaco.wakeup(co)
    if _sleep_co[co] then
        if _wakeup_co[co] == nil then
            _wakeup_co[co] = true
        end
    else
        error('Try wakeup untag sleep coroutine')
    end
end

function shaco.wait()
    local session = gen_session()
    coyield('SLEEP', session)
    _sleep_co[corunning()] = nil
    _yield_session_co[session] = nil
end

function shaco.sleep(interval)
    local session = gen_session()
    c.timer(session, interval)
    local ok, ret = coyield('SLEEP', session)
    _sleep_co[corunning()] = nil
    if ok then
        return
    else
        if ret == 'BREAK' then
            return ret
        else
            error(ret)
        end
    end
end

function shaco.timeout(interval, func)
    local co = co_create(func)
    local session = gen_session()
    assert(_yield_session_co[session] == nil, 'Repeat session '..session)
    _yield_session_co[session] = co
    c.timer(session, interval)
end

function shaco.dispatch(protoname, fun)
    assert(fun)
    local p = proto[protoname]
    p.dispatch = fun
end

function shaco.start(func)
    c.callback(dispatchcb)
    shaco.timeout(0, func)
end

function shaco.register_protocol(class)
    if proto[class.id] then
        error("Repeat protocol id "..tostring(class.id))
    end
    if proto[class.name] then
        error("Repeat protocol name "..tostring(class.name))
    end
    proto[class.id] = class
    proto[class.name] = class
end

shaco.register_protocol {
    id = shaco.TTIME,
    name = "time",
}

shaco.register_protocol {
    id = shaco.TRESPONSE,
    name = "response",
}

shaco.register_protocol {
    id = shaco.TTEXT,
    name = "text",
    pack = function(...) return ... end,
    unpack = shaco.tostring
}

shaco.register_protocol {
    id = shaco.TLUA,
    name = "lua",
    pack = shaco.pack,
    unpack = shaco.unpack
}

shaco.register_protocol  {
    id = shaco.TERROR,
    name = "error",
    unpack = function(...) return ... end,
    dispatch = dispatch_error,
}

function shaco.getenv(key)
    return shaco.command('GETENV', key)
end

function shaco.launch(name)
    return tonumber(shaco.command('LAUNCH', name))
end

function shaco.luaservice(name)
    return shaco.launch('lua '..name)
end

function shaco.queryservice(name)
    return assert(shaco.call('.service', 'lua', 'QUERY', name))
end

function shaco.register(name)
    shaco.call('.service', 'lua', 'REG', name..' '..shaco.handle())
end

function shaco.exit(info)
    shaco.command('EXIT', info or 'in lua')
end

return shaco
