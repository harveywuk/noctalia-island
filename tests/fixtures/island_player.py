import os,time

from gi.repository import Gio,GLib
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
path='/org/mpris/MediaPlayer2'
root='org.mpris.MediaPlayer2'
player=root+'.Player'
props={
 root:{'Identity':GLib.Variant('s','Island Test Player'),'DesktopEntry':GLib.Variant('s','test'), 'CanQuit':GLib.Variant('b',False),'CanRaise':GLib.Variant('b',False),'HasTrackList':GLib.Variant('b',False),'SupportedUriSchemes':GLib.Variant('as',[]),'SupportedMimeTypes':GLib.Variant('as',[])},
 player:{'PlaybackStatus':GLib.Variant('s','Playing'),'LoopStatus':GLib.Variant('s','None'),'Rate':GLib.Variant('d',1),'Shuffle':GLib.Variant('b',False),'Volume':GLib.Variant('d',.7),'Position':GLib.Variant('x',74000000),'MinimumRate':GLib.Variant('d',1),'MaximumRate':GLib.Variant('d',1),**{k:GLib.Variant('b',True) for k in ['CanControl','CanPlay','CanPause','CanGoNext','CanGoPrevious','CanSeek']}}
}
def metadata(title):
 return GLib.Variant('a{sv}',{'mpris:trackid':GLib.Variant('o','/track/one'),'mpris:length':GLib.Variant('x',218000000),'xesam:title':GLib.Variant('s',title),'xesam:artist':GLib.Variant('as',['Orbit • Native Noctalia']),'mpris:artUrl':GLib.Variant('s',os.environ['ISLAND_TEST_ART'])})
props[player]['Metadata']=metadata(os.environ.get('ISLAND_TEST_TITLE','A little closer to home'))
if os.environ.get('ISLAND_TEST_TICK'):
 props[player]['Position']=GLib.Variant('x',0)
xml='<node>'
for interface,values in props.items():
 xml+=f'<interface name="{interface}">'
 for key,value in values.items():xml+=f'<property name="{key}" type="{value.get_type_string()}" access="read"/>'
 if interface==player:
  for method in ['PlayPause','Play','Pause','Stop','Next','Previous']:xml+=f'<method name="{method}"/>'
  xml+='<method name="Seek"><arg type="x" direction="in"/></method><method name="SetPosition"><arg type="o" direction="in"/><arg type="x" direction="in"/></method>'
 xml+='</interface>'
xml+='</node>'
def change(key,value):
 props[player][key]=value
 bus.emit_signal(None,path,'org.freedesktop.DBus.Properties','PropertiesChanged',GLib.Variant('(sa{sv}as)',(player,{key:value},[])))
def method(connection,sender,path,interface,name,parameters,invocation):
 with open(os.environ['ISLAND_TEST_EVENTS'],'a') as f:f.write(name+' '+str(parameters.unpack())+'\n')
 if name in ['PlayPause','Play','Pause']:
  playing=props[player]['PlaybackStatus'].unpack()=='Playing'
  state='Playing' if name=='Play' or name=='PlayPause' and not playing else 'Paused'
  change('PlaybackStatus',GLib.Variant('s',state))
 if name in ['Next','Previous']:change('Metadata',metadata('Another orbit'))
 if name=='SetPosition':change('Position',GLib.Variant('x',parameters.unpack()[1]))
 invocation.return_value(None)
for info in Gio.DBusNodeInfo.new_for_xml(xml).interfaces:
 bus.register_object(path,info,method,lambda c,s,p,i,k:props[i][k],None)
Gio.bus_own_name_on_connection(bus,'org.mpris.MediaPlayer2.islandtest',Gio.BusNameOwnerFlags.NONE,None,None)
if os.environ.get('ISLAND_TEST_TICK'):
 def tick():
  if props[player]['PlaybackStatus'].unpack()=='Playing':
   change('Position',GLib.Variant('x',props[player]['Position'].unpack()+1000000))
  return True
 GLib.timeout_add_seconds(1,tick)
GLib.MainLoop().run()
