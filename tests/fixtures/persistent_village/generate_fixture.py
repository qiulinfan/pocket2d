"""Generate the small native tilemap fixture; only authored geometry, no imports."""
from pathlib import Path
import json, struct
ROOT=Path(__file__).parent

def bmp(path,w,h,pixels):
    data=bytes(v for row in reversed(pixels) for r,g,b,a in row for v in (b,g,r,a))
    dib=struct.pack('<IiiHHIIiiII',108,w,h,1,32,3,len(data),2835,2835,0,0)
    dib+=struct.pack('<IIIII',0x00ff0000,0x0000ff00,0x000000ff,0xff000000,0x73524742)+bytes(48)
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(struct.pack('<2sIHHI',b'BM',122+len(data),0,0,122)+dib+data)

colors=[(42,83,58),(115,78,49),(115,78,49),(115,78,49),(102,111,130),(168,145,95),(48,105,155),(168,94,150)]
pixels=[[(0,0,0,0)]*64 for _ in range(32)]
for tile,color in enumerate(colors):
    ox=(tile%4)*16;oy=(tile//4)*16
    for y in range(16):
        for x in range(16):
            c=color
            if x==15 or y==15:c=tuple(max(0,v-18) for v in c)
            if tile==1 and y%4==1:c=(86,57,41)
            if tile==2 and ((x in (7,8) and 6<=y<=12) or (4<=x<=11 and 6<=y<=8)):c=(80,169,77)
            if tile==3 and ((5<=x<=10 and 3<=y<=12) or (3<=x<=12 and 6<=y<=9)):c=(234,187,69)
            if tile==4 and (y%8==0 or (x+(8 if y>=8 else 0))%16==0):c=(65,76,94)
            pixels[oy+y][ox+x]=(*c,255)
bmp(ROOT/'images/tiles/farm.bmp',64,32,pixels)
hero=[[(0,0,0,0)]*16 for _ in range(16)]
for y in range(16):
    for x in range(16):
        if 4<=x<=11 and 5<=y<=11:hero[y][x]=(72,174,222,255)
        if 5<=x<=10 and 1<=y<=6:hero[y][x]=(243,204,167,255)
        if 4<=x<=11 and y<=2:hero[y][x]=(39,45,66,255)
        if x in (5,6,9,10) and 12<=y<=14:hero[y][x]=(38,45,65,255)
bmp(ROOT/'images/player.bmp',16,16,hero)
# Small self-authored bitmap labels keep the fixture independent of fonts.
glyphs={
'A':['01110','10001','10001','11111','10001','10001','10001'],
'C':['01111','10000','10000','10000','10000','10000','01111'],
'D':['11110','10001','10001','10001','10001','10001','11110'],
'E':['11111','10000','10000','11110','10000','10000','11111'],
'H':['10001','10001','10001','11111','10001','10001','10001'],
'L':['10000','10000','10000','10000','10000','10000','11111'],
'M':['10001','11011','10101','10101','10001','10001','10001'],
'N':['10001','11001','10101','10011','10001','10001','10001'],
'O':['01110','10001','10001','10001','10001','10001','01110'],
'P':['11110','10001','10001','11110','10000','10000','10000'],
'R':['11110','10001','10001','11110','10100','10010','10001'],
'S':['01111','10000','10000','01110','00001','00001','11110'],
'T':['11111','00100','00100','00100','00100','00100','00100'],
'V':['10001','10001','10001','10001','10001','01010','00100'],
'W':['10001','10001','10001','10101','10101','10101','01010'],
}
def label(name,text):
    pixels=[[(0,0,0,0)]*(len(text)*12) for _ in range(14)]
    for i,ch in enumerate(text):
        if ch==' ':continue
        for y,row in enumerate(glyphs[ch]):
            for x,on in enumerate(row):
                if on=='1':
                    for yy in range(y*2,y*2+2):
                        for xx in range(i*12+x*2,i*12+x*2+2):pixels[yy][xx]=(224,234,216,255)
    bmp(ROOT/('images/'+name+'.bmp'),len(text)*12,14,pixels)
glyphs.update({
'B':['11110','10001','10001','11110','10001','10001','11110'],
'F':['11111','10000','10000','11110','10000','10000','10000'],
'G':['01111','10000','10000','10111','10001','10001','01111'],
'I':['11111','00100','00100','00100','00100','00100','11111'],
'5':['11111','10000','10000','11110','00001','00001','11110'],
'9':['01110','10001','10001','01111','00001','00001','01110'],
})
label('controls','ARROWS MOVE E FARM SPACE WALL F5 SAVE F9 LOAD B IRRIGATE')
label('save_label','SAVE');label('load_label','LOAD');label('irrigation_label','IRRIGATION')
pump=[[(0,0,0,0)]*16 for _ in range(16)]
for y in range(16):
    for x in range(16):
        if 3<=x<=12 and 3<=y<=12:pump[y][x]=(75,180,220,255)
        if 5<=x<=10 and y<=3:pump[y][x]=(170,190,195,255)
        if y>=12 and 1<=x<=14:pump[y][x]=(170,190,195,255)
bmp(ROOT/'images/pump.bmp',16,16,pump)
label('harvest_label','HARVEST');label('planted_label','PLANTED')
bmp(ROOT/'images/hud.bmp',960,86,[[(20,31,37,255)]*960 for _ in range(86)])
bmp(ROOT/'images/counter.bmp',6,10,[[(255,255,255,255)]*6 for _ in range(10)])
ground=[1]*(64*64);solid=[0]*(64*64)
for y in range(8,12):
    for x in range(8,12):ground[y*64+x]=2
    solid[y*64+16]=5
asset={'version':1,'width':64,'height':64,'cell_size':1.0,'atlas':{'image':'tiles/farm.bmp','columns':4,'rows':2,'tile_width_px':16,'tile_height_px':16},'layers':{'ground':{'sorting_order':-100,'cells':ground},'solid':{'sorting_order':0,'cells':solid}}}
(ROOT/'maps').mkdir(exist_ok=True)
(ROOT/'maps/farm.tilemap.json').write_text(json.dumps(asset,separators=(',',':'))+'\n')
