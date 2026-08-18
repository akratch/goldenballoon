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
def stats(px,x0,y0,x1,y1):
    n=0;s=0;s2=0; rs=0;gs=0;bs=0
    for y in range(y0,y1):
        b=(y*W+x0)*3
        row=px[b:b+(x1-x0)*3]
        for i in range(0,len(row),3):
            g=(row[i]*3+row[i+1]*6+row[i+2])//10
            s+=g;s2+=g*g;n+=1
            rs+=row[i];gs+=row[i+1];bs+=row[i+2]
    mean=s/n; var=s2/n-mean*mean
    return mean, var**0.5, rs/n, gs/n, bs/n
print("frame |  wfTop mean/sd  RGB          |  wfMid mean/sd |  rock mean/sd")
for fr in range(51390,51406):
    px=load(fr)
    m1,sd1,r,g,b = stats(px,240,110,330,240)   # suspicious upper waterfall window
    m2,sd2,_,_,_ = stats(px,180,300,300,420)   # mid waterfall body
    m3,sd3,_,_,_ = stats(px,420,180,560,300)   # rock control
    print(f"{fr} | {m1:6.1f} {sd1:5.1f}  {r:5.1f},{g:5.1f},{b:5.1f} | {m2:6.1f} {sd2:5.1f} | {m3:6.1f} {sd3:5.1f}")
