-- Test-only observer/controller placed beside unmodified upstream plugin entries.
local pomodoro = __POMODORO__
noctalia.setUpdateInterval(100)
function update()
    local snapshot = if pomodoro then noctalia.state.get("pomodoro.state") else {
        state = noctalia.state.get("timer.state"),
        remaining = noctalia.state.get("timer.remaining"),
        duration = noctalia.state.get("timer.duration"),
    }
    noctalia.writeFile(noctalia.pluginDir() .. "/test-state.json", noctalia.json.encode(snapshot))
end
function onIpc(event, payload)
    if pomodoro then
        noctalia.state.set("pomodoro.nextCommand", event)
    elseif event == "start" then
        noctalia.state.set("timer.remaining", tonumber(payload))
        noctalia.state.set("timer.cmd", "START")
    else
        noctalia.state.set("timer.cmd", event)
    end
end
