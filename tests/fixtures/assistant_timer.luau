-- The existing timer state contract, with an observable command log for native tests.
noctalia.setUpdateInterval(100)
noctalia.state.set("timer.state", "IDLE")
noctalia.state.set("timer.remaining", 0)
noctalia.state.set("timer.duration", 0)
local starts = 0
local commands = 0
noctalia.state.watch("timer.cmd", function(cmd)
    commands += 1
    local state = noctalia.state.get("timer.state")
    if cmd == "START" and (state == "IDLE" or state == "PAUSED") then
        if noctalia.state.get("timer.remaining") > 0 then
            if state == "IDLE" then
                starts += 1
                noctalia.state.set("timer.duration", noctalia.state.get("timer.remaining"))
            end
            noctalia.state.set("timer.state", "RUNNING")
        end
    elseif cmd == "PAUSE" then
        noctalia.state.set("timer.state", "PAUSED")
    elseif cmd == "RESET" then
        noctalia.state.set("timer.state", "IDLE")
        noctalia.state.set("timer.remaining", 0)
        noctalia.state.set("timer.duration", 0)
    end
end)
function update()
    noctalia.writeFile(noctalia.pluginDir() .. "/test-state.json", noctalia.json.encode({
        state = noctalia.state.get("timer.state"),
        remaining = noctalia.state.get("timer.remaining"),
        duration = noctalia.state.get("timer.duration"),
        starts = starts,
        commands = commands,
    }))
end
function onIpc(event, payload)
    if event == "reset" then noctalia.state.set("timer.cmd", "RESET") end
    if event == "finish" then
        noctalia.state.set("timer.remaining", 0)
        noctalia.state.set("timer.state", "NOTIFY")
    end
    if event == "remaining" then noctalia.state.set("timer.remaining", tonumber(payload)) end
end
