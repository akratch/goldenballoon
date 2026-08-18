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
def gray(px,x0,y0,x1,y1):
    rows=[]
    for y in range(y0,y1):
        b=(y*W+x0)*3; row=px[b:b+(x1-x0)*3]
        rows.append([(row[i]*3+row[i+1]*6+row[i+2])//10 for i in range(0,len(row),3)])
    return rows
def shift2d(a,b,dxr,dyr):
    best=(0,0,1e18); w=len(a[0]); h=len(a)
    for dy in dyr:
        for dx in dxr:
            tot=0;cnt=0
            for ry in range(max(0,dy),min(h,h+dy),3):
                ra=a[ry]; rb=b[ry-dy]
                lo=max(0,dx);hi=min(w,w+dx)
                for x in range(lo,hi,2):
                    d=ra[x]-rb[x-dx]; tot+= d if d>=0 else -d; cnt+=1
            m=tot/cnt
            if m<best[2]: best=(dx,dy,m)
    return best
# static stretch near 50600: locate waterfall first by brightness? Use same window as 50550 view: x100-560,y150-440 (falls were there ~50 frames earlier)
prev=None
print("STATIC stretch (~50598-50606), window x150..520 y160..420:")
for fr in range(50598,50607):
    px=load(fr); cur=gray(px,150,160,520,420)
    if prev is not None:
        dx,dy,m=shift2d(prev,cur,range(-4,5),range(-4,9))
        print(f"  {fr-1}->{fr}: dx={dx} dy={dy} resid={m:.2f}")
    prev=cur
