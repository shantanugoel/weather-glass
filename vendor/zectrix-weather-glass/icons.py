"""Smooth vector weather family. Shared geometry for hero and forecast icons."""
import json, math
from pathlib import Path
from functools import lru_cache
from PIL import Image,ImageDraw
import geometry as g
ROOT=Path(__file__).resolve().parents[1]
GROUPS=json.loads((ROOT/'data/groups.json').read_text())
CODE_KIND={c:k for k,cs in GROUPS.items() for c in cs}
CONDITIONS={c['code']:c for c in json.loads((ROOT/'data/weather_conditions.json').read_text())}
SHORT=json.loads((ROOT/'data/short-en.json').read_text())
ZH={1000:'晴',1003:'多云',1006:'阴云',1009:'阴天',1012:'霾',1015:'浮尘',1018:'扬尘',1021:'尘暴',1024:'沙尘暴',1027:'强沙尘暴',1030:'薄雾',1033:'烟雾',1036:'烟霾',1039:'雾霾',1042:'重度雾霾',1045:'沙尘',1048:'尘埃',1063:'局部阵雨',1066:'局部阵雪',1069:'局部雨夹雪',1072:'局部冻毛雨',1087:'雷电',1114:'风雪',1117:'暴风雪',1135:'雾',1147:'冻雾',1150:'局部毛毛雨',1153:'毛毛雨',1168:'冻毛毛雨',1171:'强冻毛雨',1180:'局部小雨',1183:'小雨',1186:'间歇中雨',1189:'中雨',1192:'间歇大雨',1195:'大雨',1198:'小冻雨',1201:'强冻雨',1204:'小雨夹雪',1207:'强雨夹雪',1210:'局部小雪',1213:'小雪',1216:'局部中雪',1219:'中雪',1222:'局部大雪',1225:'大雪',1237:'冰粒',1240:'阵雨',1243:'强阵雨',1246:'暴雨',1249:'阵性雨夹雪',1252:'强阵性雨夹雪',1255:'阵雪',1258:'强阵雪',1261:'阵性冰粒',1264:'强阵性冰粒',1273:'雷阵雨',1276:'强雷雨',1279:'雷阵雪',1282:'强雷雪'}
def kind(code):
 try:return CODE_KIND.get(int(code),'unknown')
 except (ValueError,TypeError):return 'unknown'
def label(code,day=True,lang='zh'):
 if code not in CONDITIONS:return '天气未知' if lang=='zh' else 'Unknown weather'
 if lang=='zh':return '晴夜' if code==1000 and not day else ZH[code]
 return SHORT.get(str(code),CONDITIONS[code]['day' if day else 'night']).strip()
def background(code,day=True):
 k=kind(code)
 if 'thunder' in k:return 'thunder'
 if k in ('clear','partly_cloudy') and not day:return 'night'
 if k=='clear':return 'clear'
 if any(x in k for x in ['snow','blizzard','sleet','ice']):return 'snow'
 if any(x in k for x in ['rain','drizzle']):return 'rain'
 if k in ['partly_cloudy','cloudy','overcast']:return 'cloud'
 return 'fog'
def snowflake(x,y,r=4):
 return [g.path(f'M {x-r*math.cos(a)} {y-r*math.sin(a)} L {x+r*math.cos(a)} {y+r*math.sin(a)}',width=2.2) for a in (0,math.pi/3,2*math.pi/3)]
def moon():
 out=g.shapes('clear_night')
 for sh in out:sh['transform']=(.50,.50,8,0)
 return out

def shapes(k,day=True):
 if k=='wind':return [g.path('M 8 22 L 35 22 C 48 22 48 8 40 8 C 35 8 34 12 35 15',width=3),g.path('M 8 32 L 48 32 C 60 32 60 19 52 19 C 48 19 47 22 48 24',width=3),g.path('M 8 42 L 31 42 C 43 42 43 55 35 55 C 30 55 29 51 30 49',width=3)]
 if k=='humidity':return [g.path('M 32 7 C 27 17 15 29 15 40 C 15 60 49 60 49 40 C 49 29 37 17 32 7 Z',width=3)]
 if k=='clear':return g.shapes('sunny' if day else 'clear_night')
 if k=='unknown':return [g.circle(32,32,23),g.path('M 24 25 C 24 13 44 13 43 25 C 43 32 32 31 32 38'),g.circle(32,46,1.4,fill=255)]
 if k=='partly_cloudy':return (g._sun(24,24,10) if day else moon())+[g.path(g.CLOUD,fill=0,transform=(1,1,0,7))]
 if k in ('cloudy','overcast'):return g.shapes('cloudy')
 obscured=k in {'haze','dust_haze','blowing_dust','dust_storm','sandstorm','severe_sandstorm','mist','smoke','smoky_haze','smog','severe_smog','saharan_dust','dust','fog','freezing_fog'}
 if obscured:
  out=([g.path(g.CLOUD,fill=0,transform=(.85,.85,4,-4))] if k in ('fog','mist','freezing_fog','smog','severe_smog') else (g._sun(32,20,9) if day else moon()))
  for i,y in enumerate([40,47,54]):
   out.append(g.path(f'M {10+i%2*5} {y} L {54-i%2*5} {y}',width=2.8))
  if any(x in k for x in ['dust','sand']):
   for x,y in [(14,36),(27,43),(42,50),(50,36)]:out.append(g.circle(x,y,1,fill=255))
  if 'freezing' in k:out+=snowflake(52,55)
  return out
 patchy='patchy' in k or 'showers' in k
 out=(g._sun(20,16,7) if day else moon()) if patchy else []
 out.append(g.path(g.CLOUD,fill=0,transform=(1,1,0,-6)))
 heavy=any(x in k for x in ['heavy','moderate','torrential','blizzard'])
 xs=[18,33,48] if heavy else [24,42]
 if 'thunder' in k:
  out.append(g.path('M 30 40 L 22 51 L 30 51 L 27 61 L 43 46 L 35 46 L 39 40 Z',fill=255,stroke=None));xs=[12,52]
 if 'snow' in k or k=='blizzard':
  for x in xs:out+=snowflake(x,51)
 elif 'sleet' in k:
  out.append(g._drop(20,45));out+=snowflake(44,51)
 elif 'ice' in k:
  for x in xs:out.append(g.path(f'M {x} 46 L {x+3} 50 L {x+2} 54 L {x-2} 54 L {x-3} 50 Z',fill=255,stroke=None))
 elif 'drizzle' in k:
  for x in xs:
   out+=[g.circle(x,47,1.4,fill=255,stroke=None),g.circle(x,54,1.4,fill=255,stroke=None)]
 elif 'rain' in k:
  for x in xs:out.append(g._drop(x,44))
 if 'freezing' in k:out+=snowflake(55,58,3)
 if k in ('blowing_snow','blizzard'):out+=[g.path('M 8 44 L 49 44 C 60 44 60 34 53 35',width=2.2)]
 return out

@lru_cache(maxsize=512)
def mask(k,day,size):
 # Geometry rasterizer accepts a shape list via a local wrapper, never resizes a small bitmap.
 scale=size*4/64;im=Image.new('L',(size*4,size*4),0);d=ImageDraw.Draw(im)
 for sh in shapes(k,day):
  sx,sy,tx,ty=sh.get('transform') or (1,1,0,0)
  w=max(1,round(sh['width']*scale*math.sqrt(sx*sy)))
  if sh['type']=='circle':
   x=(sh['cx']*sx+tx)*scale;y=(sh['cy']*sy+ty)*scale;r=sh['r']*scale*sx
   if sh['fill'] is not None:d.ellipse((x-r,y-r,x+r,y+r),fill=sh['fill'])
   if sh['stroke'] is not None:
    d.ellipse((x-r-w/2,y-r-w/2,x+r+w/2,y+r+w/2),fill=sh['stroke'])
    if r>w/2:d.ellipse((x-r+w/2,y-r+w/2,x+r-w/2,y+r-w/2),fill=sh['fill'] or 0)
  else:
   pts,closed=g._flatten(sh['d']);pts=[((x*sx+tx)*scale,(y*sy+ty)*scale) for x,y in pts]
   if sh['fill'] is not None:d.polygon(pts,fill=sh['fill'])
   if sh['stroke'] is not None:
    d.line(pts,fill=sh['stroke'],width=w,joint='curve')
    # Rounded joins at every sampled vertex prevent pinholes in wide cubic strokes.
    for x,y in pts:d.ellipse((x-w/2,y-w/2,x+w/2,y+w/2),fill=sh['stroke'])
 return im.resize((size,size),Image.Resampling.LANCZOS)

def svg(k,day=True):
 # Alpha mask handles occlusion of sun/moon by a cloud without white-filled boxes.
 out=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><mask id="ink"><rect width="64" height="64" fill="black"/>']
 col=lambda v:'none' if v is None else ('white' if v else 'black')
 for sh in shapes(k,day):
  attr=f'fill="{col(sh["fill"])}" stroke="{col(sh["stroke"])}" stroke-width="{sh["width"]}" stroke-linecap="round" stroke-linejoin="round"'
  if sh.get('transform'):
   sx,sy,tx,ty=sh['transform'];attr+=f' transform="matrix({sx} 0 0 {sy} {tx} {ty})"'
  if sh['type']=='circle':out.append(f'<circle cx="{sh["cx"]}" cy="{sh["cy"]}" r="{sh["r"]}" {attr}/>')
  else:out.append(f'<path d="{sh["d"]}" {attr}/>')
 return ''.join(out)+ '</mask></defs><rect width="64" height="64" fill="currentColor" mask="url(#ink)"/></svg>'
