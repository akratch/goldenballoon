W,H=1280,720
def load(fr):
    with open(f"/tmp/interp-evidence/f9/frames/frame_{fr}.ppm","rb") as f: data=f.read()
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
def diffmap(f1,f2,dx,dy,out):
    a=load(f1); b=load(f2)
    # motion-compensated abs diff (gray), amplified 3x, full frame
    res=bytearray(W*H*3)
    for y in range(H):
        ys=y+dy
        if ys<0 or ys>=H: continue
        arow=(y*W)*3; brow=(ys*W)*3
        for x in range(W):
            xs=x+dx
            if xs<0 or xs>=W: continue
            ai=arow+x*3; bi=brow+xs*3
            d=(abs(a[ai]-b[bi])*3+abs(a[ai+1]-b[bi+1])*6+abs(a[ai+2]-b[bi+2]))//10
            v=min(255,d*3)
            o=ai
            res[o]=v;res[o+1]=v;res[o+2]=v
    with open(out,"wb") as f:
        f.write(b"P6\n%d %d\n255\n"%(W,H)); f.write(res)
# feature at (x,y) in frameN found at (x-dx,y-dy) in frameN+1 with dx=-11,dy=-3
# so compensate: compare a[y][x] to b[y+3][x+11]... map: b index = (x - dx, y - dy) = x+11,y+3
diffmap(51397,51398,11,3,"/tmp/interp-evidence/f9/png/d_97_98.ppm")
diffmap(51396,51397,11,3,"/tmp/interp-evidence/f9/png/d_96_97.ppm")
diffmap(51398,51399,22,5,"/tmp/interp-evidence/f9/png/d_98_99.ppm")
