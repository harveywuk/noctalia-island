-- Deterministic timer state for testing the Island outline, not a timer engine.
noctalia.state.set("timer.state", "IDLE")
noctalia.state.set("timer.remaining", 0)
noctalia.state.set("timer.duration", 100)
function onIpc(event, payload)
    noctalia.state.set("timer.remaining", tonumber(payload) or 0)
    noctalia.state.set("timer.state", event)
end
