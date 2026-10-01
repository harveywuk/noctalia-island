"""Native keyboard/pointer/touchpad settings, reload persistence and independent restoration."""
import json
import os
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    hypr = base / 'hyprland.lua'
    hypr.write_text(hypr.read_text() + '\nhl.config({input={sensitivity=-0.2,accel_profile="adaptive",left_handed=false,natural_scroll=false,scroll_factor=1.2,touchpad={natural_scroll=false,scroll_factor=1.1,tap_to_click=true,tap_and_drag=true,drag_lock=0,tap_button_map="lrm",clickfinger_behavior=false,middle_button_emulation=false,disable_while_typing=true,drag_3fg=0}}})\n')
    hypr.write_text(hypr.read_text() + '\nhl.config({input={kb_layout="gb,us",kb_variant=",dvorak",kb_options="compose:ralt",repeat_rate=31,repeat_delay=410,numlock_by_default=true}})\n')
    ctl('reload'); time.sleep(.7)
    keys = ['input:sensitivity', 'input:accel_profile', 'input:left_handed', 'input:natural_scroll', 'input:scroll_factor']
    keys += ['input:touchpad:' + key for key in ['natural_scroll', 'scroll_factor', 'tap_to_click', 'tap_and_drag',
             'drag_lock', 'tap_button_map', 'clickfinger_behavior', 'middle_button_emulation', 'disable_while_typing', 'drag_3fg']]

    def option(key):
        data = json.loads(ctl('-j', 'getoption', key))
        return next(data[k] for k in ('float', 'int', 'bool', 'str') if k in data)
    keyboard_keys = ['input:' + key for key in ['kb_layout', 'kb_variant', 'kb_options', 'repeat_rate', 'repeat_delay', 'numlock_by_default']]
    original = {key: option(key) for key in keys + keyboard_keys}
    wanted = dict(zip(keys, [.35, 'flat', True, True, 1.8, True, .7, False, False, 2, 'lmr', True, True, False, 2]))
    wanted.update(dict(zip(keyboard_keys, ['us,gb,us', ',,dvorak', 'grp:alt_shift_toggle,caps:escape', 45, 250, False])))
    keys += keyboard_keys
    baseline = (cfg / 'config.toml').read_text()

    def configure(mouse, touchpad, keyboard=True):
        (cfg / 'config.toml').write_text(baseline + '\n[shell.hyprland_input]\nmouse_managed=' + str(mouse).lower() + '\ntouchpad_managed=' + str(touchpad).lower() + '\nkeyboard_managed=' + str(keyboard).lower() + '''
keyboard_layout="us"
keyboard_layout_2="gb"
keyboard_layout_3="us:dvorak"
keyboard_layout_4="us"
keyboard_layout_switch="alt-shift"
keyboard_caps_lock="escape"
keyboard_repeat_rate=45
keyboard_repeat_delay=250
keyboard_num_lock=false
pointer_sensitivity=0.35
pointer_acceleration="flat"
mouse_left_handed=true
mouse_natural_scroll=true
mouse_scroll_factor=1.8
touchpad_natural_scroll=true
touchpad_scroll_factor=0.7
touchpad_tap_to_click=false
touchpad_tap_and_drag=false
touchpad_drag_lock="sticky"
touchpad_tap_map="lmr"
touchpad_clickfinger=true
touchpad_middle_emulation=true
touchpad_disable_while_typing=false
touchpad_drag_fingers="four"
''')
        msg('config-reload'); time.sleep(.8)

    def check(mouse, touchpad, keyboard=True):
        for key in keys:
            managed = keyboard if key in keyboard_keys else touchpad if ':touchpad:' in key else mouse
            expected = wanted[key] if managed else original[key]
            value = option(key)
            assert (abs(value - expected) < .0001 if isinstance(expected, float) else value == expected), (key, value, expected)
        assert not ctl('configerrors').strip()

    configure(True, True); check(True, True)
    ctl('reload'); time.sleep(.8); check(True, True)
    # Clear optional layouts and shortcut, change Caps Lock, and disable repeat.
    path = cfg / 'config.toml'
    text = path.read_text().replace('keyboard_layout_2="gb"', 'keyboard_layout_2="none"').replace('keyboard_layout_3="us:dvorak"', 'keyboard_layout_3="none"').replace('keyboard_layout_4="us"', 'keyboard_layout_4="none"')
    text = text.replace('keyboard_layout_switch="alt-shift"', 'keyboard_layout_switch="none"').replace('keyboard_caps_lock="escape"', 'keyboard_caps_lock="control"').replace('keyboard_repeat_rate=45', 'keyboard_repeat_rate=0').replace('keyboard_repeat_delay=250', 'keyboard_repeat_delay=0')
    path.write_text(text); msg('config-reload'); time.sleep(.8)
    assert [option(key) for key in keyboard_keys] == ['us', '', 'ctrl:nocaps', 0, 0, False]
    configure(False, True); check(False, True)
    configure(True, False); check(True, False)
    configure(True, True, False); check(True, True, False)
    configure(False, False, True); check(False, False, True)
    configure(False, False, False); check(False, False, False)
    configure(True, True)
    # Keep pointer buttons conventional for UI inspection while retaining all other settings.
    path = cfg / 'config.toml'
    path.write_text(path.read_text().replace('mouse_left_handed=true', 'mouse_left_handed=false'))
    msg('config-reload'); time.sleep(.7)
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('settings-open', 'input-motion'); time.sleep(.8)
    run(['grim', '-o', 'TEST-1', str(out / 'native-input-settings.png')])
    (out / 'inspect-env.json').write_text(json.dumps(env))
    if os.environ.get('NOCTALIA_TEST_INPUT_INSPECT'):
        print('INSPECT: native input settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_INPUT_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists(): time.sleep(.25)
    assert shell.poll() is None
    print('PASS: all native keyboard, mouse and touchpad options, compositor reload persistence, '
          'independent group disable and restoration to Lua settings', flush=True)
