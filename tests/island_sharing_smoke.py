"""Desktop sharing glow driven by real links in a private PipeWire graph."""
import json
import pathlib
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
media_gradient=true
track_preview_seconds=0
outer_progress_ring=false
split_activities=false''').replace('[shell]\n', '[shell]\noffline_mode=true\n'))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    path = cfg/'config.toml'
    original = path.read_text()
    captures = []

    def shot(name, output='TEST-1'):
        dest = out/(name+'.png'); run(['grim', '-o', output, str(dest)])
        image = Image.open(dest).convert('RGB')
        return image.resize((1280,720)) if output == 'TEST-2' else image

    def strength(image, bottom=72):
        # The first two bands immediately below the capsule, outside both its fill and artwork.
        return max(min(r-g,b-g) for r,g,b in image.crop((620,bottom,660,bottom+4)).getdata())

    def glowing(name, output='TEST-1', bottom=72):
        image = shot(name, output)
        value = strength(image, bottom)
        assert value > 12, (name, 'Sharing glow missing', value)
        return image

    def quiet(name):
        value = strength(shot(name))
        assert value < 8, (name, 'Sharing glow remained', value)

    def samples(name):
        values = []
        for i in range(10):
            values.append(strength(shot(f'{name}-{i}')))
            time.sleep(.22)
        assert min(values) > 12, (name, 'Pulse disappeared', values)
        assert max(values)-min(values)>10, (name, 'Pulse stopped', values)
        return values

    def capture(index, linked=True):
        # The classifier uses the node classes and source metadata. Audio DSP ports provide a
        # deterministic transport for these video-class nodes without a real camera or portal.
        source, consumer = f'sharing-output-{index}', f'sharing-input-{index}'
        proc = start(['pw-loopback', '-n', f'sharing-{index}', '-c', '1', '-m', 'MONO',
                      '--capture-props', f'node.name={consumer} media.class=Stream/Input/Video application.name="Sharing Test {index}"',
                      '--playback-props', f'node.name={source} media.class=Video/Source media.name="Screen capture"'],
                     f'sharing-{index}.log')
        captures.append(proc); time.sleep(.7)
        assert proc.poll() is None
        for node in json.loads(run(['pw-dump'])):
            name = node.get('info',{}).get('props',{}).get('node.name')
            if name not in (source,consumer):
                continue
            direction = 'Output' if name == source else 'Input'
            run(['pw-cli','set-param',str(node['id']),'PortConfig',
                 '{ direction = '+direction+' mode = dsp format = { mediaType = audio mediaSubtype = raw format = F32P rate = 48000 channels = 1 position = [ MONO ] } }'])
        time.sleep(.3)
        ports = (next(v.strip() for v in run(['pw-link','-o']).splitlines() if source+':' in v),
                 next(v.strip() for v in run(['pw-link','-i']).splitlines() if consumer+':' in v))
        if linked:
            run(['pw-link',*ports]); time.sleep(2.7)
        return proc, ports

    def stop(proc):
        proc.terminate(); proc.wait(timeout=5); time.sleep(2.7)

    def configure(extra='', hidden=False):
        text = original
        if hidden:
            text = text.replace('[island]\nenabled=true','[island]\nenabled=false')
            text += '''
[bar.sharing]
presentation="island"
reserve_space=false
auto_hide=true
show_on_workspace_switch=false
[bar.sharing.island]
hover_widgets=[]
track_preview_seconds=0
'''
        path.write_text(text+extra); msg('config-reload'); time.sleep(1)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        dispatch('hl.dsp.cursor.move({x=1100,y=600})')
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
        time.sleep(2); quiet('idle')
        first, ports = capture(1, linked=False)
        quiet('unlinked-screen-source')
        run(['pw-link',*ports]); time.sleep(2.7)
        samples('sharing-pulse')
        glowing('sharing-scaled','TEST-2')
        second, _ = capture(2)
        stop(first); glowing('one-viewer-remains')

        # A volume OSD retains the same outer glow without adding status icons to the slider.
        run(['pactl','set-sink-volume','@DEFAULT_SINK@','43%']); time.sleep(.4)
        glowing('volume-osd'); time.sleep(2.5)
        msg('panel-open','control-center','audio'); time.sleep(1)
        panel = shot('hosted-panel')
        # Find the horizontal glow under the larger panel, clear of its text and controls.
        last_glow_row = max(y for y in range(85,690)
                            if strength(panel,y)>12 and min(panel.getpixel((640,y)))<110)
        panel_bottom = max(range(last_glow_row-12,last_glow_row+1), key=lambda y:strength(panel,y))
        glowing('hosted-panel-glow',bottom=panel_bottom)
        stop(second)
        assert strength(shot('hosted-panel-stopped'),panel_bottom)<8, 'Hosted panel retained stopped capture'
        third, _ = capture(3)
        glowing('hosted-panel-started',bottom=panel_bottom)
        msg('panel-close'); time.sleep(1)
        samples('returned-pulse')

        configure('\n[shell.animation]\nenabled=false\n')
        still = [strength(glowing(f'reduced-motion-{i}')) for i in range(3) if not time.sleep(.5)]
        assert max(still)-min(still)<=2, ('Reduced-motion glow animated',still)
        msg('panel-open','control-center','audio'); time.sleep(.7)
        glowing('reduced-hosted',bottom=panel_bottom)
        stop(third)
        assert strength(shot('reduced-hosted-stopped'),panel_bottom)<8, 'Reduced-motion panel retained capture'
        fourth, _ = capture(4)
        glowing('reduced-hosted-started',bottom=panel_bottom)
        msg('panel-close'); time.sleep(.7)

        configure('\n[shell.privacy]\nscreen_filter_regex="Sharing Test"\n')
        quiet('filtered-sharing')
        configure(hidden=True)
        time.sleep(2.5); glowing('auto-hide-held-open')
        stop(fourth)
        quiet('last-viewer-stopped')
        hidden = shot('auto-hide-restored')
        configure()
        visible = shot('visible-idle')
        assert ImageChops.difference(hidden.crop((600,20,680,50)),visible.crop((600,20,680,50))).getbbox(), 'Sharing left auto-hide pinned'

        # The living media artwork remains visible and animated behind the sharing indicator.
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'player.log')
        fifth, _ = capture(5)
        a = glowing('media-sharing-before'); time.sleep(.5); b = glowing('media-sharing-after')
        region = (590,14,690,20)
        assert sum(max(p)>12 for p in b.crop(region).getdata())>300, 'Sharing hid the artwork'
        assert ImageChops.difference(a.crop(region),b.crop(region)).getbbox(), 'Sharing froze the artwork'
        stop(fifth); quiet('media-sharing-stopped')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: persistent sharing pulse, unlinked sources, last viewer, OSD, panels, auto-hide, filters, reduced motion, live artwork and 150% scaling',flush=True)
    finally:
        for proc in captures:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
