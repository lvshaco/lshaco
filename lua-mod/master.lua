local shaco = require "shaco"
local socket = require "socket"

local _slaves = {}
local _global_names = {}

local function pack(...)
    local msg = shaco.packstring(...)
    return string.char(#msg)..msg
end

local function read_package(id)
    local sz  = string.byte(assert(socket.read(id, 1)))
    local msg = assert(socket.read(id, sz))
    return shaco.unpackstring(msg)
end

local command = {}

function command.R(slave, name, handle)
    local slaveid = slave.id
    local sock = slave.sock
    if handle <= 0 or handle >= 256 then
        assert(string.format('Invalid handle %02x register by slave %02x', 
        handle, slaveid))
    end
    handle = handle|(slaveid<<8)
    if not _global_names[name] then
        _global_names[name] = handle
    end
    local msg = pack('N', name, handle)
    for k, v in pairs(_slaves) do
        if v.sock ~= sock then
            socket.send(v.sock, msg)
        end
    end
end

function command.Q(slave, name)
    local sock = slave.sock
    local handle = _global_names[name]
    if handle then
        assert(socket.send(sock, pack('N', name, handle)))
    end
end

local function dispatch_slave(slave)
    local sock = slave.sock
    local t, p1, p2 = read_package(sock)
    local func = command[t]
    if func then
        func(slave, p1, p2)
    else
        shaco.error('Invalid slave message type '..t)
    end
end

local function monitor_slave(slaveid)
    local ok, err
    local slave = _slaves[slaveid]
    local sock  = slave.sock
    while true do
        ok, err = pcall(dispatch_slave, slave)
        if not ok then
            break
        end
    end
    socket.close(sock)
    _slaves[slaveid] = nil
    for k, v in pairs(_slaves) do
        socket.send(v.sock, pack('D', slaveid))
    end
    shaco.info(string.format('Slave %02x#%s exit: %s', 
        slaveid, slave.addr, err))
end

local function accept_slave(sock)
    socket.start(sock)
    socket.readon(sock)
    local t, slaveid, addr = read_package(sock)
    assert(t=='H' and
        type(slaveid)=='number' and
        type(addr)=='string', 'Handshake fail')
    assert(slaveid > 0 and slaveid < 256, 'Invalid slaveid')
    if _slaves[slaveid] then
        error(string.format('Slave %02x already register on %s', slaveid, addr))
    end
    
    local n = 0
    for k, v in pairs(_slaves) do
        n = n + 1
    end
    assert(socket.send(sock, pack('W', n)))
    for k, v in pairs(_slaves) do
        assert(socket.send(sock, pack('S', v.id, v.addr)))
    end

    for k, v in pairs(_slaves) do
        socket.send(v.sock, pack('C', slaveid, addr))
    end
    _slaves[slaveid] = {id=slaveid, sock=sock, addr = addr}
    shaco.info(string.format('Slave %02x#%s register', slaveid, addr))
    return slaveid
end

shaco.start(function()
    local addr = assert(shaco.getenv('standalone'))
    shaco.info('Master listen on '..addr)
    local sock = assert(
    socket.listen(addr, function(id)
        local ok, info = pcall(accept_slave, id)
        if ok then
            local slaveid = info
            monitor_slave(slaveid)
        else
            shaco.error(info)
            socket.close(id)
        end
    end))
end)
