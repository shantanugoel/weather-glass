"""Original 64-unit vector primitives shared by all weather icon sizes."""
from __future__ import annotations

from pathlib import Path
import math
import re
from PIL import Image, ImageDraw, ImageFont, ImageOps

KINDS = ('partly_cloudy', 'sunny', 'rain', 'cloudy', 'snow', 'thunder', 'clear_night')
STROKE = 3.1
SUPERSAMPLE = 8

# These cubic curves were authored for this collection. The cloud has one
# continuous outline and an intentionally open junction at the right dome.
CLOUD = ('M 17 46 C 10.8 46 6.7 42.1 6.7 37.1 '
         'C 6.7 32.2 10.6 28.5 15.6 28.4 '
         'C 16.3 21.7 21.5 17.2 28.1 17.2 '
         'C 34.8 17.2 40.1 22.4 40.3 29.1 '
         'C 42.4 27 45.1 26 48 26.2 '
         'C 54.3 26.5 58.7 31.2 58.7 36.7 '
         'C 58.7 42.1 54.5 46 48.6 46 Z')


def path(d, *, fill=None, stroke=255, width=STROKE, transform=None):
    return {'type':'path', 'd':d, 'fill':fill, 'stroke':stroke,
            'width':width, 'transform':transform}


def circle(cx, cy, r, *, fill=None, stroke=255, width=STROKE):
    return {'type':'circle', 'cx':cx, 'cy':cy, 'r':r,
            'fill':fill, 'stroke':stroke, 'width':width}


def _triangle(cx, cy, angle, inner, outer, base):
    ux, uy = math.cos(angle), math.sin(angle)
    vx, vy = -uy, ux
    pts=[(cx+outer*ux, cy+outer*uy),
         (cx+inner*ux+base*vx/2, cy+inner*uy+base*vy/2),
         (cx+inner*ux-base*vx/2, cy+inner*uy-base*vy/2)]
    return path('M '+' L '.join(f'{x:.4f} {y:.4f}' for x,y in pts)+' Z',
                fill=255, stroke=None)


def _sun(cx,cy,r, rays=True):
    shapes=[]
    if rays:
        for i in range(8):
            shapes.append(_triangle(cx,cy,math.pi*i/4,r+5,r+10,4.7))
    shapes.append(circle(cx,cy,r))
    return shapes


def _drop(x,y):
    return path(f'M {x} {y} C {x-1.4} {y+2.7} {x-3.1} {y+5.2} {x-3.1} {y+6.7} '
                f'C {x-3.1} {y+10.9} {x+3.1} {y+10.9} {x+3.1} {y+6.7} '
                f'C {x+3.1} {y+5.2} {x+1.4} {y+2.7} {x} {y} Z',
                fill=255,stroke=None)


def _star(x,y,r):
    pts=[]
    for i in range(10):
        a=-math.pi/2+i*math.pi/5
        rr=r if i%2==0 else r*.43
        pts.append((x+rr*math.cos(a), y+rr*math.sin(a)))
    return path('M '+' L '.join(f'{a:.4f} {b:.4f}' for a,b in pts)+' Z',fill=255,stroke=None)


def shapes(kind):
    if kind == 'sunny':
        return _sun(32,32,15.2)
    if kind == 'partly_cloudy':
        return _sun(24,24,10) + [path(CLOUD,fill=0,transform=(1,1,0,7))]
    if kind == 'cloudy':
        return [path(CLOUD,fill=0,transform=(.74,.74,2,0)),
                path(CLOUD,fill=0,transform=(.93,.93,2,12))]
    if kind == 'rain':
        return [path(CLOUD,fill=0,transform=(1,1,0,-6))] + [_drop(x,45) for x in (20,33,46)]
    if kind == 'snow':
        s=[path(CLOUD,fill=0,transform=(1,1,0,-6))]
        for cx in (18,33,48):
            for a in (0,math.pi/3,math.pi*2/3):
                dx,dy=4.2*math.cos(a),4.2*math.sin(a)
                s.append(path(f'M {cx-dx:.4f} {50-dy:.4f} L {cx+dx:.4f} {50+dy:.4f}',width=2.4))
        return s
    if kind == 'thunder':
        return [path(CLOUD,fill=0,transform=(1,1,0,-7)),
                path('M 31 42 L 24 51 L 31.5 51 L 28.5 59 L 43 46 L 35 46 L 38 42 Z',
                     fill=255,stroke=None)]
    if kind == 'clear_night':
        return [path('M 38.5 9 C 25 6.9 14 17 14 29.9 '
                     'C 14 43.2 25 53 37.5 53 '
                     'C 46 53 53.1 48.1 56.4 41.3 '
                     'C 48 44.2 39 41.3 34.4 34.4 '
                     'C 28.9 26.1 30.4 15.5 38.5 9 Z',fill=None),
                _star(48.5,16,4.5),_star(54,28,2.8),_star(8,44,2.6)]
    raise ValueError(f'Unknown weather icon: {kind!r}; supported: {KINDS}')


def _flatten(d):
    toks=re.findall(r'[MLCZ]|-?\d+(?:\.\d+)?',d)
    i=0; points=[]; p=(0,0); first=None; closed=False
    while i<len(toks):
        cmd=toks[i];i+=1
        if cmd in ('M','L'):
            p=(float(toks[i]),float(toks[i+1]));i+=2
            points.append(p)
            if first is None:first=p
        elif cmd=='C':
            v=list(map(float,toks[i:i+6]));i+=6
            p0=p;p1=(v[0],v[1]);p2=(v[2],v[3]);p3=(v[4],v[5])
            # Geometry is sampled independently of output size. The 8x raster
            # stage smooths both large and small instances of these same paths.
            for j in range(1,65):
                t=j/64;s=1-t
                points.append((s**3*p0[0]+3*s*s*t*p1[0]+3*s*t*t*p2[0]+t**3*p3[0],
                               s**3*p0[1]+3*s*s*t*p1[1]+3*s*t*t*p2[1]+t**3*p3[1]))
            p=p3
        elif cmd=='Z':
            closed=True
            if points[-1]!=first:points.append(first)
        else:raise ValueError(cmd)
    return points,closed

