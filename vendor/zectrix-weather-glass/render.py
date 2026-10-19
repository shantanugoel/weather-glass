"""Deterministic 400x300 liquid-glass compositor; outputs continuous grayscale."""
import argparse,json,io
from pathlib import Path
from functools import lru_cache
from datetime import datetime
from PIL import Image,ImageDraw,ImageFont
import icons
from model import normalize
ROOT=Path(__file__).resolve().parents[1]
S=3;W=400*S;H=300*S

@lru_cache(maxsize=128)
def font(size,bold=False,zh=False):
 f=ImageFont.truetype(str(ROOT/'assets/fonts'/('NotoSansSC.ttf' if zh else 'Inter.ttf')),round(size*S))
 axes=f.get_variation_axes()
 f.set_variation_by_axes([700 if bold and a['name']==b'Weight' else 450 if a['name']==b'Weight' else a['default'] for a in axes])
 return f

@lru_cache(maxsize=16)
def plate(name,night=False):
 # These photographic glass plates are edited directly from the user-approved
 # reference. Never simulate or redraw their rim lighting with procedural borders.
 variant=name+'-night' if night and name not in ('night','thunder') else name
 return Image.open(ROOT/'assets/glass-plates'/f'{variant}.png').convert('L').resize((W,H),Image.Resampling.LANCZOS)

def render(model,scale=1):
 m=model;zh=m['lang']=='zh';bg=icons.background(m['code'],m['day']);im=plate(bg,not m['day']).copy();d=ImageDraw.Draw(im)
 night=not m['day'];foreground=246 if night else 12
 def text(x,y,t,size=11,bold=False,width=None,align='left',color=None):
  if color is None:color=foreground
  while True:
   f=font(size,bold,zh)
   length=d.textlength(str(t),font=f)
   if width is None or length<=width*S or size<=7:break
   size-=.25
  if width is not None and length>width*S:
   while t and d.textlength(t+'…',font=f)>width*S:t=t[:-1]
   t+='…';length=d.textlength(t,font=f)
  if align=='right':x-=length/S
  elif align=='center':x-=length/(2*S)
  d.text((round(x*S),round(y*S)),str(t),font=f,fill=color,anchor='lt')
 def paint_icon(mask,x,y):
  pos=(round(x*S),round(y*S))
  im.paste(246 if night else 20,pos,mask)
 def icon(code,day,x,y,sz):
  paint_icon(icons.mask(icons.kind(code),day,round(sz*S)),x,y)
 def line(x0,y0,x1,y1):
  points=(x0*S,y0*S,x1*S,y1*S)
  d.line(points,fill=205 if night else 115,width=2)
 head=246 if bg in ('night','thunder') or not m['day'] else 12
 text(16,15,m['city'],17,True,width=205,color=head)
 text(384,13,m['date'],10,width=157,align='right',color=head)
 text(384,29,m['updated'],8.6,width=174,align='right',color=head)
 size=90 if len(m['temp'])<=2 else 73 if len(m['temp'])==3 else 55
 text(51,66,m['temp'],size,True,width=112)
 text(165,119,'°C',23,width=44)
 if zh:text(123,151,m['condition']+' · '+m['feels'],13,True,width=182,align='center')
 else:
  text(124,145,m['condition'],12,True,width=179,align='center')
  text(124,162,m['feels'],9.5,width=170,align='center')
 icon(m['code'],m['day'],230,62,103)
 hilo=(f'最高 {m["high"]}° / 最低 {m["low"]}°' if zh else f'H {m["high"]}° / L {m["low"]}°')
 text(286,158,hilo,10,width=143,align='center')
 # Metric icons use the same smooth vector strokes, rendered at final supersampling.
 for x in [158,268]:line(x,190,x,204)
 paint_icon(icons.mask('wind',True,20*S),43,187)
 text(67,192,m['wind'],10.2,True,width=87)
 paint_icon(icons.mask('humidity',True,20*S),173,187)
 text(192,192,m['humidity'],10.2,True,width=71)
 paint_icon(icons.mask('clear',True,20*S),280,187)
 text(308,192,m['uv'],10.5,True,width=59)
 for x in [140,261]:line(x,228,x,274)
 for i,f in enumerate(m['days']):
  x=28+i*122
  text(x,229,f['label'],10.2,True,width=100)
  icon(f['code'],True,x-1,245,33)
  text(x+39,246,f['high']+'° / '+f['low']+'°',12.2,True,width=77)
  text(x+39,266,f['precip'],9.4,width=77)
 if scale==S:return im
 return im.resize((400*scale,300*scale),Image.Resampling.LANCZOS)

def png(model):
 b=io.BytesIO();render(model).save(b,format='PNG');return b.getvalue()
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--input',required=True);a.add_argument('--output',required=True);a.add_argument('--lang',default='zh',choices=['zh','en']);a.add_argument('--city');a.add_argument('--now');o=a.parse_args()
 m=normalize(json.loads(Path(o.input).read_text()),o.lang,o.city,datetime.fromisoformat(o.now) if o.now else None)
 Path(o.output).parent.mkdir(parents=True,exist_ok=True);render(m).save(o.output)
