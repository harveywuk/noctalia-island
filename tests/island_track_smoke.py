"""Track/artist announcements with staged and reused browser MPRIS metadata."""
from difflib import SequenceMatcher
import json
import os
import pathlib
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path=cfg/'config.toml'
    path.write_text(path.read_text().replace('[island]\nenabled=true','[island]\nenabled=false')
                    .replace('[shell]\n','[shell]\noffline_mode=true\n').replace('[osd.kinds]\n','[osd.kinds]\nkeyboard_layout=false\n')+'''
[bar.music]
presentation="island"
reserve_space=false
[bar.music.island]
height=64
clock_size=24
clock_seconds=true
hover_widgets=[]
media_gradient=true
split_activities=false
[accessibility]
ui_scale=1.1
''')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo=pathlib.Path(__file__).resolve().parents[1]
    tessdata=os.environ.get('NOCTALIA_TEST_TESSDATA',str(repo/'build-rishot/test-data/tessdata'))
    path=cfg/'config.toml'
    original=path.read_text()

    def shot(name,output='TEST-1',normalize=True):
        dest=out/(name+'.png');run(['grim','-o',output,str(dest)])
        im=Image.open(dest).convert('RGB')
        return im.resize((1280,720)) if output=='TEST-2' and normalize else im

    def words(name,output='TEST-1',box=(480,16,800,90)):
        im=shot(name,output,normalize=False)
        scale=im.width/1280
        im=im.crop(tuple(round(v*scale) for v in box))
        dest=out/(name+'-text.png')
        # Separate white/muted lettering from the blue artwork glow before recognition.
        im.resize((im.width*3,im.height*3)).getchannel('G').point(lambda v:0 if v>130 else 255).save(dest)
        return run(['tesseract',str(dest),'stdout','--tessdata-dir',tessdata,'--psm','6'])

    def announced(name,title,output='TEST-1'):
        text=words(name,output)
        # The animated artwork can soften a glyph; still require a complete artist line.
        artist=any(SequenceMatcher(None,'island ensemble',line.strip().casefold()).ratio()>.86 for line in text.splitlines())
        assert title.casefold() in text.casefold() and artist,(name,text)

    def quiet(name):
        text=words(name)
        assert 'Amber' not in text and 'Another' not in text and 'Ensemble' not in text,(name,text)

    def media(method):
        run(['gdbus','call','--session','--dest','org.mpris.MediaPlayer2.islandtest',
             '--object-path','/org/mpris/MediaPlayer2','--method','org.mpris.MediaPlayer2.Player.'+method])
        expected={'Play': "'playback_status': <'Playing'>", 'Pause': "'playback_status': <'Paused'>",
                  'Next':'Another orbit','Previous':'Amber Skies'}.get(method)
        if expected:
            wait(lambda:expected in players(),'MPRIS update: '+method)

    def until(deadline):
        time.sleep(max(0,deadline-time.monotonic()))

    ctl('dismissnotify');dispatch('hl.dsp.focus({monitor="TEST-1"})');dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    msg('color-scheme-set','community','macOS');msg('theme-mode-set','dark');time.sleep(2.5)
    env.update(ISLAND_TEST_ART=(repo/'assets/noctalia-wallpaper.png').as_uri(),
               ISLAND_TEST_EVENTS=str(out/'player-actions.log'),ISLAND_TEST_TICK='1',
               ISLAND_TEST_STATUS='Paused',ISLAND_TEST_TITLE='Amber Skies',
               ISLAND_TEST_ARTIST='Island Ensemble',ISLAND_TEST_URL='https://example.com/radio')
    player=start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'player.log')
    def players():
        return run(['gdbus','call','--session','--dest','dev.noctalia.Mpris','--object-path','/dev/noctalia/Mpris',
                    '--method','dev.noctalia.Mpris.GetPlayers'])
    wait(lambda:'Amber Skies' in players(),'Player discovery')
    time.sleep(.4);quiet('queued-first')
    media('Play');began=time.monotonic()
    wait(lambda:"'playback_status': <'Playing'>" in players(),'Playback state')
    time.sleep(.65)
    announced('first','Amber Skies');announced('first-scaled','Amber Skies','TEST-2')
    a=shot('art-before');time.sleep(.35);b=shot('art-after');strip=(590,15,690,21)
    assert sum(max(p)>12 for p in b.crop(strip).getdata())>300,'Preview covered artwork'
    assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(),'Preview froze artwork'
    msg('config-reload');until(began+6.2);quiet('expired-despite-position-and-clock')
    media('Pause');time.sleep(.4);media('Play');time.sleep(.7);quiet('resume')

    # Track identity and URL stay constant, including while a download owns the capsule.
    msg('island-activity-start',json.dumps({'id':'background','title':'Background Transfer','progress':25}))
    media('Pause');time.sleep(.4);media('Next');time.sleep(.5);quiet('queued-next')
    media('Play');began=time.monotonic();time.sleep(.6);announced('next-over-download','Another orbit')
    msg('panel-open','control-center','media');time.sleep(.4);msg('panel-close');time.sleep(.6)
    announced('panel-return','Another orbit')
    until(began+6.2);quiet('returned-to-download')
    assert 'Background' in words('download-restored'),'Preview did not restore the previous activity'

    # Urgent foreground events retain priority, with no stale announcement replay afterwards.
    run(['notify-send','-a','Track test','-u','critical','-t','0','Urgent alert','Keep visible'])
    time.sleep(.6);media('Previous');began=time.monotonic();time.sleep(.6)
    text=words('urgent-over-track',box=(425,44,805,83));assert 'Urgent' in text and 'Amber' not in text,text
    until(began+6.2);msg('notification-clear-active');msg('notification-clear-history');time.sleep(.5)
    quiet('no-replay')
    msg('island-activity-end','background');time.sleep(5.5)

    path.write_text(original.replace('media_gradient=true','media_gradient=true\ntrack_preview_seconds=0'))
    msg('config-reload');media('Next');time.sleep(.7);quiet('disabled')
    path.write_text(original+'\n[shell.animation]\nenabled=false\n');msg('config-reload')
    media('Previous');time.sleep(.7);announced('reduced-motion','Amber Skies')
    media('Next');time.sleep(.8);announced('latest-track','Another orbit')
    player.terminate();player.wait(timeout=5);time.sleep(.8);quiet('player-removed')
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: title and artist, staged playback, reused browser metadata, expiry, resume, download priority, panels, alerts, artwork, disabled previews, reduced motion and 150% scaling',flush=True)
