"""Self-authored RGBA geometry for animation integration tests (no imports)."""
from pathlib import Path
import struct,zlib,json
ROOT=Path(__file__).parent

def png(path,pixels):
    h,w=len(pixels),len(pixels[0])
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    raw=b''.join(b'\0'+bytes(v for pixel in row for v in pixel) for row in pixels)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

def canvas(w,h,color=(0,0,0,0)):return [[color]*w for _ in range(h)]
def rect(p,x0,y0,x1,y1,color):
    for y in range(max(0,y0),min(len(p),y1)):
        for x in range(max(0,x0),min(len(p[0]),x1)):p[y][x]=(*color,255)

def hero(phase,base=False):
    p=canvas(16,16)
    rect(p,5,1,11,7,(240,196,150));rect(p,4,1,12,3,(42,45,67))
    rect(p,4,7,12,12,(128,139,146) if base else (63,167,203))
    rect(p,5,12,7,15-(1 if phase==1 else 0),(38,50,72))
    rect(p,9,12,11,15-(1 if phase==2 else 0),(38,50,72))
    rect(p,2,7+(phase==2),4,10+(phase==2),(240,196,150))
    rect(p,12,7+(phase==1),14,10+(phase==1),(240,196,150))
    if phase==3:rect(p,12,2,14,8,(238,224,174));rect(p,11,7,15,9,(187,133,65))
    if phase==4:rect(p,11,7,16,9,(244,210,107));rect(p,12,10,16,11,(222,110,79))
    return p
atlas=canvas(80,16)
for i in range(5):
    cell=hero(i)
    for y in range(16):atlas[y][i*16:(i+1)*16]=cell[y]
png(ROOT/'images/hero.png',atlas);png(ROOT/'images/base.png',hero(0,True))
floor=canvas(160,96,(29,43,52,255))
for y in range(96):
    for x in range(160):
        if x%16==15 or y%16==15:floor[y][x]=(39,58,65,255)
png(ROOT/'images/floor.png',floor)
asset={'version':1,'image':'hero.png','columns':5,'rows':1,'tile_width_px':16,'tile_height_px':16,'clips':[
 {'name':'idle','loop':True,'frames':[{'row':1,'column':1,'ticks':4,'event':'rest'}]},
 {'name':'walk','loop':True,'frames':[{'row':1,'column':2,'ticks':2,'event':'left'},{'row':1,'column':3,'ticks':2,'event':'right'}]},
 {'name':'attack','loop':False,'frames':[{'row':1,'column':4,'ticks':1,'event':'swing'},{'row':1,'column':5,'ticks':2,'event':'hit'}]}
]}
(ROOT/'animations/hero.animation.json').write_text(json.dumps(asset,indent=2)+'\n')
