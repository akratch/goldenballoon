W,H=1280,720
import os
def load(fr):
    p=f"/tmp/interp-evidence/f9/frames/frame_{fr}.ppm"
    if not os.path.exists(p): return None
    with open(p,"rb") as f: data=f.read()
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
def stripmad(a,b,y):
    base=(y*W)*3; n=0;s=0
    ra=a[base:base+W*3]; rb=b[base:base+W*3]
    for i in range(0,W*3,9):
        d=ra[i]-rb[i]
        s+= d if d>=0 else -d; n+=1
    return s/n
results=[]
for fr in range(200,53000,200):
    a=load(fr); b=load(fr+1)
    if a is None or b is None: continue
    m=(stripmad(a,b,100)+stripmad(a,b,300)+stripmad(a,b,500))/3
    results.append((m,fr))
results.sort()
print("lowest-motion pairs (MAD, frame):")
for m,fr in results[:15]: print(f"  {m:6.2f}  {fr}")
print("highest:")
for m,fr in results[-5:]: print(f"  {m:6.2f}  {fr}")
