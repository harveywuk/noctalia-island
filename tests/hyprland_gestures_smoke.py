"""Native gesture registration, lifecycle and Hyprspace callback checks in private Hyprland."""
import json
import os
import pathlib
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    hypr = base / 'hyprland.lua'
    original_hypr = hypr.read_text()
    # Observe calls while retaining real native registration and unrelated Lua bindings.
    instrument = '''
hl.config({gestures={workspace_swipe_distance=430,workspace_swipe_invert=false}})
_G.test_gestures={}; _G.test_gesture_adds=0
local nativeGesture=hl.gesture
hl.gesture=function(rule)
  local key=rule.fingers .. ':' .. rule.direction .. ':' .. string.format('%g',rule.scale or 1)
  if rule.action=='unset' then
    assert(_G.test_gestures[key], 'Removed an unowned or missing gesture')
    _G.test_gestures[key]=nil
  else
    assert(not _G.test_gestures[key], 'Duplicate gesture')
    _G.test_gestures[key]=rule; _G.test_gesture_adds=_G.test_gesture_adds+1
  end
  nativeGesture(rule)
end
hl.gesture({fingers=5,direction='left',action=function() end})
'''
    hypr.write_text(original_hypr + instrument)
    ctl('reload'); time.sleep(.8)
    baseline = (cfg / 'config.toml').read_text()
    def lua(code):
        result = ctl('repl', code)
        assert 'error' not in result.lower(), result
    def configure(managed=True, workspace=True, overview=True, fingers='three', sensitivity=1.5, direction='up', extra=''):
        (cfg / 'config.toml').write_text(baseline + f'''
[shell.hyprland_input]
gestures_managed={str(managed).lower()}
workspace_gesture_enabled={str(workspace).lower()}
workspace_gesture_fingers="{fingers}"
workspace_gesture_sensitivity={sensitivity}
workspace_gesture_distance=450
workspace_gesture_invert=true
overview_gesture_enabled={str(overview).lower()}
overview_gesture_fingers="four"
overview_gesture_direction="{direction}"
overview_gesture_distance=120
{extra}
''')
        msg('config-reload'); time.sleep(.8)
        assert not ctl('configerrors').strip(), ctl('configerrors')
    def rules(keys):
        quoted = ','.join(json.dumps(key) for key in keys)
        lua('local expected={' + quoted + '}; local count=0; for _ in pairs(_G.test_gestures) do count=count+1 end; '
            'assert(count==#expected,tostring(count)); for _,k in ipairs(expected) do assert(_G.test_gestures[k],k) end')
    def option(key):
        result = json.loads(ctl('-j', 'getoption', key))
        return next(result[k] for k in ('int', 'bool', 'float') if k in result)

    configure(fingers='four')
    rules(['5:left:1', '4:horizontal:1.5'])  # Hyprspace is absent, workspace gesture still works.
    lua('_G.test_before_adds=_G.test_gesture_adds')
    configure(fingers='four', extra='mouse_managed=true\npointer_sensitivity=0.2')
    lua('assert(_G.test_before_adds==_G.test_gesture_adds)')
    configure(); rules(['5:left:1', '3:horizontal:1.5'])
    configure(sensitivity=2); rules(['5:left:1', '3:horizontal:2'])
    configure(workspace=False); rules(['5:left:1'])
    configure(managed=False); rules(['5:left:1'])
    assert option('gestures:workspace_swipe_distance') == 430
    assert not option('gestures:workspace_swipe_invert')

    plugin = pathlib.Path(os.environ.get('NOCTALIA_TEST_HYPRSPACE', '/home/mrpickles/.local/share/hyprland-plugins/0.56.2/Hyprspace.so'))
    assert plugin.exists(), 'Set NOCTALIA_TEST_HYPRSPACE to a plugin built for the test compositor'
    load = '\nhl.plugin.load(' + json.dumps(str(plugin)) + ')\nif hl.plugin.overview then hl.config({plugin={overview={disableGestures=false}}}) end\n'
    hypr.write_text(original_hypr + load + instrument)
    ctl('reload'); time.sleep(2)
    assert 'Hyprspace' in ctl('plugin', 'list')
    configure(); rules(['5:left:1', '3:horizontal:1.5', '4:up:1'])
    assert option('plugin:overview:disableGestures') == 1
    ctl('reload'); time.sleep(1); rules(['5:left:1', '3:horizontal:1.5', '4:up:1'])
    assert option('gestures:workspace_swipe_distance') == 450
    lua('_G.test_overview_calls=0; local toggle=hl.plugin.overview.toggle; hl.plugin.overview.toggle=function() _G.test_overview_calls=_G.test_overview_calls+1; return toggle() end')
    def swipe(delta, cancelled=False, direction='up', expected=0):
        lua("local action=_G.test_gestures['4:" + direction + ":1'].action; action.start(); "
            + f"action.update({{delta={{y={delta / 2}}}}}); action.update({{delta={{y={delta / 2}}}}}); "
            + f"action.finish({{cancelled={str(cancelled).lower()}}}); assert(_G.test_overview_calls=={expected},tostring(_G.test_overview_calls))")
    swipe(-40); swipe(-180, True); swipe(180)
    swipe(-120, expected=1); time.sleep(.5)
    run(['grim', '-o', 'TEST-1', str(out / 'gesture-overview-open.png')])
    swipe(-150, expected=2); time.sleep(.5)
    configure(direction='down'); rules(['5:left:1', '3:horizontal:1.5', '4:down:1'])
    swipe(-180, direction='down', expected=2)
    swipe(150, direction='down', expected=3)
    swipe(150, direction='down', expected=4)
    configure(overview=False); rules(['5:left:1', '3:horizontal:1.5'])
    configure(workspace=False, overview=True); rules(['5:left:1', '4:up:1'])
    configure(managed=False); rules(['5:left:1'])
    assert option('plugin:overview:disableGestures') == 0
    assert option('gestures:workspace_swipe_distance') == 430
    configure()
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('settings-open', 'input-motion'); time.sleep(.8)
    (out / 'inspect-env.json').write_text(json.dumps(env))
    run(['grim', '-o', 'TEST-1', str(out / 'gesture-settings.png')])
    if os.environ.get('NOCTALIA_TEST_GESTURE_INSPECT'):
        print('INSPECT: gesture settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_GESTURE_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists(): time.sleep(.25)
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: native gesture registration, no duplicate bindings, trigger replacement, independent toggles, '
          'reload and Lua restoration, plugin guard, real Hyprspace callback, distance and cancellation checks', flush=True)
