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
def crop(fr,x0,y0,x1,y1,scale,out):
    px=load(fr); w=x1-x0; h=y1-y0
    rows=[]
    for y in range(y0,y1):
        b=(y*W+x0)*3
        rows.append(px[b:b+w*3])
    with open(out,"wb") as f:
        f.write(b"P6\n%d %d\n255\n"%(w*scale,h*scale))
        for r in rows:
            line=b"".join(r[i:i+3]*scale for i in range(0,len(r),3))
            for _ in range(scale): f.write(line)
# montage strip: 6 consecutive frames around stall, waterfall region
for fr in range(51395,51402):
    crop(fr,0,80,320,460,2,f"/tmp/interp-evidence/f9/png/wf_{fr}.ppm")
# far-from-stall pair set
for fr in range(50552,50557):
    crop(fr,100,150,560,440,1,f"/tmp/interp-evidence/f9/png/wf2_{fr}.ppm")
