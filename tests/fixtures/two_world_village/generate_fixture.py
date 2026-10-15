"""Reproduce this fixture's original geometry and authored data, using stdlib only.

Glyph patterns reuse the repository's self-authored inventory_village fixture.
No Unity assets, external images, fonts or downloaded source are used. Component
scripts and CONTRACT.md have separate owners and are never written by this tool.
"""
from pathlib import Path
import json
import struct
import zlib

ROOT = Path(__file__).resolve().parent
IMAGES = ROOT / "images"

def canvas(width, height, color=(0, 0, 0, 0)):
    return [[color for _ in range(width)] for _ in range(height)]

def rect(pixels, x0, y0, x1, y1, color):
    for y in range(max(0, y0), min(len(pixels), y1)):
        for x in range(max(0, x0), min(len(pixels[0]), x1)):
            pixels[y][x] = color

def disk(pixels, cx, cy, radius, color, inner=-1):
    for y in range(len(pixels)):
        for x in range(len(pixels[0])):
            distance = (x-cx)**2 + (y-cy)**2
            if distance <= radius**2 and (inner < 0 or distance >= inner**2):
                pixels[y][x] = color

def png(name, pixels):
    height, width = len(pixels), len(pixels[0])
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data)&0xffffffff)
    raw = b"".join(b"\0"+bytes(value for pixel in row for value in pixel) for row in pixels)
    path = IMAGES / (name+".png")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB",width,height,8,6,0,0,0))+chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b""))

def data(name, value):
    path = ROOT / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, separators=(",", ":"))+"\n")

GLYPHS = {'+': '00000 00100 00100 11111 00100 00100 00000',
 '-': '00000 00000 00000 11111 00000 00000 00000',
 '.': '00000 00000 00000 00000 00000 00100 00100',
 '/': '00001 00010 00010 00100 01000 01000 10000',
 '0': '01110 10001 10011 10101 11001 10001 01110',
 '1': '00100 01100 00100 00100 00100 00100 01110',
 '2': '01110 10001 00001 00010 00100 01000 11111',
 '3': '11110 00001 00001 01110 00001 00001 11110',
 '4': '00010 00110 01010 10010 11111 00010 00010',
 '5': '11111 10000 10000 11110 00001 00001 11110',
 '6': '01111 10000 10000 11110 10001 10001 01110',
 '7': '11111 00001 00010 00100 01000 01000 01000',
 '8': '01110 10001 10001 01110 10001 10001 01110',
 '9': '01110 10001 10001 01111 00001 00001 01110',
 ':': '00000 00100 00100 00000 00100 00100 00000',
 'A': '01110 10001 10001 11111 10001 10001 10001',
 'B': '11110 10001 10001 11110 10001 10001 11110',
 'C': '01111 10000 10000 10000 10000 10000 01111',
 'D': '11110 10001 10001 10001 10001 10001 11110',
 'E': '11111 10000 10000 11110 10000 10000 11111',
 'F': '11111 10000 10000 11110 10000 10000 10000',
 'G': '01111 10000 10000 10111 10001 10001 01111',
 'H': '10001 10001 10001 11111 10001 10001 10001',
 'I': '11111 00100 00100 00100 00100 00100 11111',
 'J': '00111 00010 00010 00010 10010 10010 01100',
 'K': '10001 10010 10100 11000 10100 10010 10001',
 'L': '10000 10000 10000 10000 10000 10000 11111',
 'M': '10001 11011 10101 10101 10001 10001 10001',
 'N': '10001 11001 10101 10011 10001 10001 10001',
 'O': '01110 10001 10001 10001 10001 10001 01110',
 'P': '11110 10001 10001 11110 10000 10000 10000',
 'Q': '01110 10001 10001 10001 10101 10010 01101',
 'R': '11110 10001 10001 11110 10100 10010 10001',
 'S': '01111 10000 10000 01110 00001 00001 11110',
 'T': '11111 00100 00100 00100 00100 00100 00100',
 'U': '10001 10001 10001 10001 10001 10001 01110',
 'V': '10001 10001 10001 10001 10001 01010 00100',
 'W': '10001 10001 10001 10101 10101 10101 01010',
 'X': '10001 10001 01010 00100 01010 10001 10001',
 'Y': '10001 10001 01010 00100 00100 00100 00100',
 'Z': '11111 00001 00010 00100 01000 10000 11111',
 '_': '00000 00000 00000 00000 00000 00000 11111'}
PUNCTUATION = {":":"colon", "/":"slash", "+":"plus", ".":"dot", "-":"dash", "_":"under"}

def generate_images():
    for char, pattern in GLYPHS.items():
        pixels = canvas(12,14)
        for y, row in enumerate(pattern.split()):
            for x, bit in enumerate(row):
                if bit == "1": rect(pixels,x*2,y*2,x*2+2,y*2+2,(224,232,225,255))
        png("glyph_"+PUNCTUATION.get(char,char),pixels)
    for name,height in (("hud_top",96),("hud_bottom",108)):
        pixels=canvas(960,height,(17,24,34,255));rect(pixels,0,0,960,2,(55,73,87,255));png(name,pixels)
    for name,color in (("world_village",(42,101,66,255)),("world_underworld",(84,53,122,255))):
        png(name,canvas(156,28,color))
    for name,color in (("route_countdown",(112,74,36,255)),("route_active",(133,43,49,255)),("route_closed",(28,112,104,255))):
        png(name,canvas(344,30,color))
    pixels=canvas(160,66,(27,38,49,255));rect(pixels,0,0,160,2,(73,92,110,255));rect(pixels,0,64,160,66,(73,92,110,255));png("item_card",pixels)
    png("fade",canvas(960,720,(255,255,255,255)))
    pixels=canvas(24,24)
    rect(pixels,3,6,21,22,(125,87,45,255));rect(pixels,5,4,19,8,(174,128,63,255));rect(pixels,7,10,10,13,(239,197,100,255));rect(pixels,14,15,17,18,(239,197,100,255));png("seed",pixels)
    pixels=canvas(24,24);disk(pixels,12,14,8,(112,194,89,255));rect(pixels,11,2,14,8,(52,112,56,255));rect(pixels,14,3,21,6,(142,219,104,255));rect(pixels,7,11,10,15,(183,232,134,255));png("food",pixels)
    pixels=canvas(24,24);rect(pixels,10,2,14,15,(220,230,238,255));rect(pixels,5,15,19,18,(196,151,67,255));rect(pixels,10,18,14,23,(125,79,43,255));png("sword",pixels)
    pixels=canvas(16,16);rect(pixels,5,2,11,7,(240,196,150,255));rect(pixels,4,7,12,12,(63,167,203,255));rect(pixels,4,1,12,3,(36,44,62,255));rect(pixels,5,12,7,16,(38,50,72,255));rect(pixels,9,12,11,16,(38,50,72,255));png("player",pixels)
    pixels=canvas(16,16);rect(pixels,2,11,14,15,(56,99,101,255));rect(pixels,5,5,11,12,(67,184,172,255));disk(pixels,8,5,3,(157,242,215,255));png("altar",pixels)
    pixels=canvas(16,16);rect(pixels,6,3,10,15,(114,51,62,255));rect(pixels,2,5,14,9,(163,65,81,255));rect(pixels,3,2,5,8,(163,65,81,255));rect(pixels,11,0,14,8,(163,65,81,255));disk(pixels,8,7,2,(242,120,70,255));png("source",pixels)
    pixels=canvas(16,16);rect(pixels,4,10,12,15,(89,74,87,255));rect(pixels,2,13,14,15,(65,64,78,255));rect(pixels,6,9,10,11,(150,122,105,255));png("source_closed",pixels)
    pixels=canvas(16,16);disk(pixels,7.5,7.5,7,(102,220,218,255),5);rect(pixels,6,4,10,12,(189,245,231,220));png("portal",pixels)
    for name,color in (("invader",(201,83,64,255)),("guardian",(150,91,191,255))):
        pixels=canvas(16,16);rect(pixels,4,4,12,13,color);rect(pixels,2,1,5,6,color);rect(pixels,11,1,14,6,color);rect(pixels,5,7,7,9,(249,211,102,255));rect(pixels,9,7,11,9,(249,211,102,255));rect(pixels,4,13,7,16,color);rect(pixels,9,13,12,16,color);png(name,pixels)
    pixels=canvas(32,32);disk(pixels,15.5,15.5,15,(255,255,255,210),13);png("attack",pixels)
    palette=[(38,77,48),(101,64,38),(101,64,38),(101,64,38),(80,88,105),(37,28,53),(118,100,61),(63,49,76)]
    atlas=canvas(128,16)
    for tile,color in enumerate(palette):
        for y in range(16):
            for x in range(16):
                c=color
                if x==15 or y==15:c=tuple(max(0,value-12) for value in c)
                if tile in (1,2,3) and y%4==1:c=(74,47,29)
                if tile==2 and ((x in (7,8) and 5<=y<=12) or (4<=x<=11 and 5<=y<=7)):c=(88,178,81)
                if tile==3 and ((5<=x<=10 and 3<=y<=12) or (3<=x<=12 and 6<=y<=9)):c=(234,187,69)
                if tile==4 and (y%8==0 or (x+(8 if y>=8 else 0))%16==0):c=(52,62,80)
                if tile==5 and (x*7+y*3)%29==0:c=(53,41,72)
                atlas[y][tile*16+x]=(*c,255)
    png("tiles/worlds",atlas)

def component(type_name, **fields):
    return {"type":type_name,**fields}

def actor(uid,name,x,y,extras=None,sprite=None,scale=3.125,visible=True):
    components={"transform":component("Transform",x=x,y=y)}
    if sprite:components["sprite"]=component("SpriteRenderer",sprite=sprite,scale_x=scale,scale_y=scale,enabled=visible,sorting_order=20)
    components.update(extras or {})
    return {"uid":uid,"name":name,"components":components}

def body(x,y,active=True,collider=False):
    return component("Rigidbody",x=x,y=y,gravity_scale=0,enabled=active,collider_type="circle",width=0.5,height=0.5,radius=0.25,has_collider=collider,has_trigger=False,friction=0,bounciness=0)

def generate_data():
    data("game.config",{"game_title":"Two World Village | Arrows move, E farm, Space attack, Q eat, 1 sword, F5 save, F9 load","initial_scene":"village","save_namespace":"two_world_village"})
    data("rendering.config",{"x_resolution":960,"y_resolution":720,"clear_color_r":12,"clear_color_g":17,"clear_color_b":25,"zoom_factor":0.5})
    data("items/village.items.json",{"version":1,"items":[
        {"id":"seed","display_name":"Village seed","description":"Plant in village soil with E.","kind":"material","max_stack":99},
        {"id":"food","display_name":"Harvest food","description":"Eat with Q to restore three health.","kind":"food","max_stack":9,"health_restore":3},
        {"id":"sword","display_name":"Island sword","description":"Toggle with 1. Adds two attack.","kind":"weapon","equipment_slot":"subweapon","max_stack":1,"attack":2}]})
    for name,tile in (("village",1),("underworld",6)):
        ground=[tile]*160;solid=[0]*160
        for y in range(10):
            for x in range(16):
                if x in (0,15) or y in (0,9):solid[y*16+x]=5
        if name=="village":
            for y in range(2,6):
                for x in range(3,7):ground[y*16+x]=2
            for x in range(7,14):ground[7*16+x]=7
        else:
            for x in range(2,13):ground[7*16+x]=8
        data("maps/"+name+".tilemap.json",{"version":1,"width":16,"height":10,"cell_size":1.0,
            "atlas":{"image":"tiles/worlds.png","columns":8,"rows":1,"tile_width_px":16,"tile_height_px":16},
            "layers":{"ground":{"sorting_order":-100,"cells":ground},"solid":{"sorting_order":0,"cells":solid}}})
    actors=[actor(1,"village",0,0,{"state":component("VillageState"),"farm":component("FarmRules"),"route":component("InvasionRoute"),"save":component("VillageSave"),"view":component("VillageView")}),
        actor(2,"player",-4.5,2.5,{"body":body(-4.5,2.5,collider=True),"combat":component("Combatant")},"player.png"),
        actor(3,"altar",2.5,0.5,sprite="altar.png",scale=5),
        actor(4,"source",27.5,-0.5,{"combat":component("Combatant")},"source.png",6.25),
        actor(5,"village_portal",5.5,2.5,{"portal":component("PortalPair")},"portal.png",6.25),
        actor(6,"underworld_portal",18.5,2.5,{"portal":component("PortalPair")},"portal.png",6.25),
        actor(10,"village_map",-8,-5,{"map":component("Tilemap",map_resource="village.tilemap.json")}),
        actor(11,"underworld_map",16,-5,{"map":component("Tilemap",map_resource="underworld.tilemap.json")})]
    for index,y in enumerate((-2.5,-1.5,-0.5),1):
        actors.append(actor(100+index,"invader_"+str(index),5.5,y,{"combat":component("Combatant"),"body":body(5.5,y,active=False)},"invader.png",visible=False))
    actors.append(actor(110,"guardian",24.5,1.5,{"combat":component("Combatant"),"body":body(24.5,1.5,active=False)},"guardian.png",visible=False))
    for index in range(16):
        x=-8+3+index%4+0.5;y=-5+2+index//4+0.5
        actors.append(actor(201+index,"crop_pickup_"+str(index+1),x,y,sprite="food.png",scale=1.5,visible=False))
    data("scenes/village.scene",{"actors":actors})

if __name__ == "__main__":
    generate_images()
    generate_data()
