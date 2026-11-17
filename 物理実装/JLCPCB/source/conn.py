import json,math,sys
L=json.load(open(sys.argv[1]))
pads=[(p['ref'],q['p'],q['x'],q['y'],q['net']) for p in L['parts'] for q in p['pins'] if q['net']]
par={}
def f(k):
    par.setdefault(k,k)
    while par[k]!=k: par[k]=par[par[k]]; k=par[k]
    return k
def u(a,b): par[f(a)]=f(b)
def onseg(px,py,a,b):
    (ax,ay),(bx,by)=a,b;dx,dy=bx-ax,by-ay;L2=dx*dx+dy*dy
    t=0 if L2==0 else max(0,min(1,((px-ax)*dx+(py-ay)*dy)/L2))
    return math.hypot(px-ax-t*dx,py-ay-t*dy)<0.01
segs=[]
for i,w in enumerate(L['wires']):
    for a,b in zip(w['pts'],w['pts'][1:]): segs.append((w['net'],w['layer'],tuple(a),tuple(b),('W',i)))
for s in segs: u(s[4],s[4])
# segments of same wire connected; segments touching each other (endpoint on other) same layer
for i,s in enumerate(segs):
    for t in segs[i+1:]:
        if s[1]!=t[1]: continue
        if any(onseg(*pt,t[2],t[3]) for pt in (s[2],s[3])) or any(onseg(*pt,s[2],s[3]) for pt in (t[2],t[3])):
            if s[0]!=t[0]: print('SHORT seg',s,t)
            u(s[4],t[4])
for pd in pads:
    k=('P',pd[0],pd[1],pd[2],pd[3]); f(k)
    for s in segs:
        if onseg(pd[2],pd[3],s[2],s[3]):
            if s[0]!=pd[4]: print('SHORT pad',pd,s)
            u(k,s[4])
# J2 internal connections
for net in ('V12','GND'):
    js=[('P',*p[:4]) for p in pads if p[0]=='J2' and p[4]==net]
    for a in js[1:]: u(a,js[0])
nets={}
for pd in pads: nets.setdefault(pd[4],set()).add(f(('P',*pd[:4])))
bad=[n for n,s in nets.items() if len(s)>1]
print('nets:',len(nets),'disconnected:',bad if bad else 'none')
