W,H=1280,720
def load(fr):
    with open(f"/tmp/interp-evidence/f9/frames/frame_{fr:04d}.ppm","rb") as f: data=f.read()
    idx=2; fields=[]
    while len(fields)<3:
        while data[idx:idx+1].isspace(): idx+=1
        if data[idx:idx+1]==b'#':
            while data[idx:idx+1] not in (b'\n',b''): idx+=1
            continue
        s=idx
        while not data[idx:idx+1].isspace(): idx+=1
        fields.append(int(data[s:idx]))
    idx+=1
    return data[idx:]
def gray_full(px):
    g=[]
    for y in range(H):
        b=y*W*3; row=px[b:b+W*3]
        g.append(bytes((row[i]*3+row[i+1]*6+row[i+2])//10 for i in range(0,W*3,3)))
    return g
def block_shift(a,b,x0,y0,bs,maxs=18):
    best=(99,1e18); w=W
    for s in range(-maxs,maxs+1):
        tot=0;cnt=0
        for y in range(y0,y0+bs,3):
            ra=a[y]; rb=b[y]
            for x in range(x0+maxs,x0+bs-maxs,2):
                d=ra[x]-rb[x-s]
                tot+= d if d>=0 else -d; cnt+=1
        m=tot/cnt
        if m<best[1]: best=(s,m)
    return best
import sys
frames=[51393,51394,51395,51396,51397,51398]
gs={fr:gray_full(load(fr)) for fr in frames}
BS=64
print("block motion dx per pair (row=block-y, col=block-x), region x0-640 y64-512; '.' = low-texture block (resid<1.5 skipped)")
pairs=[(frames[i],frames[i+1]) for i in range(len(frames)-1)]
labels={ (51393,51394):"ep->0.25 (group cross)", (51394,51395):"0.25->0.50", (51395,51396):"0.50->0.75", (51396,51397):"0.75->ep (group cross)", (51397,51398):"ep->0.25 STALL 16.6ms" }
for (f1,f2) in pairs:
    print(f"--- {f1}->{f2}  {labels[(f1,f2)]}")
    a=gs[f1]; b=gs[f2]
    for by in range(64,512,BS):
        rowtxt=""
        for bx in range(0,640,BS):
            s,m=block_shift(a,b,bx,by,BS)
            # variance check: skip flat blocks
            rowtxt += f"{s:4}" if m<90 else "   ?"
        print(rowtxt)
