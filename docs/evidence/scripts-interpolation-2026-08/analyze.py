import sys, struct

W,H = 1280,720
def load(fr):
    with open(f"/tmp/interp-evidence/f9/frames/frame_{fr}.ppm","rb") as f:
        data=f.read()
    # parse header
    assert data[:2]==b'P6'
    idx=2; fields=[]
    while len(fields)<3:
        # skip whitespace/comments
        while data[idx:idx+1].isspace(): idx+=1
        if data[idx:idx+1]==b'#':
            while data[idx:idx+1] not in (b'\n',b''): idx+=1
            continue
        start=idx
        while not data[idx:idx+1].isspace(): idx+=1
        fields.append(int(data[start:idx]))
    idx+=1
    w,h,maxv=fields
    return data[idx:idx+w*h*3], w, h

def region_gray(px, x0,y0,x1,y1):
    # returns list of rows, each row a list of gray ints
    rows=[]
    for y in range(y0,y1):
        base=(y*W+x0)*3
        row=px[base:base+(x1-x0)*3]
        rows.append([ (row[i]*3+row[i+1]*6+row[i+2])//10 for i in range(0,len(row),3) ])
    return rows

def mad_and_flash(a,b,thresh=40):
    n=0; s=0; big=0
    for ra,rb in zip(a,b):
        for va,vb in zip(ra,rb):
            d=va-vb
            if d<0: d=-d
            s+=d; n+=1
            if d>thresh: big+=1
    return s/n, big/n

def best_shift(a,b,maxs=14):
    # horizontal shift of b relative to a minimizing SAD (subsample rows)
    best=(None,1e18)
    w=len(a[0])
    for s in range(-maxs,maxs+1):
        tot=0;cnt=0
        for ri in range(0,len(a),4):
            ra=a[ri]; rb=b[ri]
            lo=max(0,s); hi=min(w,w+s)
            for x in range(lo,hi,2):
                d=ra[x]-rb[x-s]
                tot+= d if d>=0 else -d
                cnt+=1
        m=tot/cnt
        if m<best[1]: best=(s,m)
    return best

def run(frames, wf, rock, sky, label):
    print(f"== {label} ==")
    print(f"{'pair':>13} | {'wfMAD':>6} {'wfFl%':>6} {'wfShift':>7} {'wfRes':>6} | {'rkMAD':>6} {'rkFl%':>6} {'rkShift':>7} {'rkRes':>6} | {'skMAD':>5}")
    prev=None
    for fr in frames:
        px,_,_=load(fr)
        cur = { 'wf':region_gray(px,*wf), 'rk':region_gray(px,*rock), 'sk':region_gray(px,*sky) }
        if prev is not None:
            wm,wfl = mad_and_flash(prev['wf'],cur['wf'])
            rm,rfl = mad_and_flash(prev['rk'],cur['rk'])
            sm,_   = mad_and_flash(prev['sk'],cur['sk'])
            ws,wres = best_shift(prev['wf'],cur['wf'])
            rs,rres = best_shift(prev['rk'],cur['rk'])
            print(f"{pf:>6}->{fr:<6} | {wm:6.2f} {wfl*100:6.2f} {ws:>7} {wres:6.2f} | {rm:6.2f} {rfl*100:6.2f} {rs:>7} {rres:6.2f} | {sm:5.2f}")
        prev=cur; pf=fr

# stall bracket: stall at 51398 per log
run(range(51388,51408), (10,100,300,430), (380,150,640,350), (900,10,1200,80), "around stall @51398 (waterfall left)")
