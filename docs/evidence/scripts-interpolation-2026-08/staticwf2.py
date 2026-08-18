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
def shiftv(a,b,dyr):
    best=(0,1e18); h=len(a); w=len(a[0])
    for dy in dyr:
        tot=0;cnt=0
        for ry in range(max(0,dy),min(h,h+dy)):
            ra=a[ry]; rb=b[ry-dy]
            for x in range(0,w):
                d=ra[x]-rb[x]; tot+= d if d>=0 else -d; cnt+=1
        m=tot/cnt
        if m<best[1]: best=(dy,m)
    return best
def sd(a):
    n=0;s=0;s2=0
    for r in a:
        for v in r: s+=v;s2+=v*v;n+=1
    m=s/n; return (s2/n-m*m)**0.5
print("PURE falls strip static (x100..165, y180..420):")
prev=None
for fr in range(50598,50606):
    px=load(fr); cur=gray(px,100,180,165,420)
    if prev is not None:
        dy,m=shiftv(prev,cur,range(-6,13))
        print(f"  {fr-1}->{fr}: dy={dy} resid={m:.2f} (sd={sd(cur):.1f})")
    prev=cur
