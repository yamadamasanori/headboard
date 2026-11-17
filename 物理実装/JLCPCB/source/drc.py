import re,math,sys
s=open(sys.argv[1]).read()
items=[] # (kind, layer-set, geom, net, width/radius)
# pads (absolute since footprint at 0 0 except mounting holes)
for fp in re.finditer(r'\(footprint "([^"]+)".*?\(at ([\d.\-]+) ([\d.\-]+)\)(.*?)\n\)',s,re.S):
    name,fx,fy,body=fp.group(1),float(fp.group(2)),float(fp.group(3)),fp.group(4)
    for p in re.finditer(r'\(pad "([^"]*)" (\w+) (\w+) \(at ([\d.\-]+) ([\d.\-]+)\) \(size ([\d.]+) [\d.]+\) \(drill ([\d.]+)\)(.*?)\(tstamp',body):
        x=fx+float(p.group(4)); y=fy+float(p.group(5)); d=float(p.group(6)); kind=p.group(2)
        m=re.search(r'\(net (\d+)',p.group(8)); net=int(m.group(1)) if m else -1 if kind!='np_thru_hole' else -2
        r=d/2 if kind!='np_thru_hole' else float(p.group(7))/2
        items.append(('pad',{'F.Cu','B.Cu'},((x,y),(x,y)),net,r,name+':'+p.group(1)))
for m in re.finditer(r'\(segment \(start ([\d.\-]+) ([\d.\-]+)\) \(end ([\d.\-]+) ([\d.\-]+)\) \(width ([\d.]+)\) \(layer "([^"]+)"\) \(net (\d+)\)',s):
    a=(float(m.group(1)),float(m.group(2))); b=(float(m.group(3)),float(m.group(4)))
    items.append(('seg',{m.group(6)},(a,b),int(m.group(7)),float(m.group(5))/2,'seg'))
for m in re.finditer(r'\(via \(at ([\d.\-]+) ([\d.\-]+)\) \(size ([\d.]+)\) .*?\(net (\d+)\)',s):
    x,y=float(m.group(1)),float(m.group(2)); items.append(('via',{'F.Cu','B.Cu'},((x,y),(x,y)),int(m.group(4)),float(m.group(3))/2,'via'))
def segdist(p,q,r,t):
    # distance between segments pq and rt
    def pd(a,b,c):
        ax,ay=a;bx,by=b;cx,cy=c;dx,dy=cx-bx,cy-by;L=dx*dx+dy*dy
        if L==0: return math.hypot(ax-bx,ay-by)
        u=max(0,min(1,((ax-bx)*dx+(ay-by)*dy)/L)); return math.hypot(ax-bx-u*dx,ay-by-u*dy)
    return min(pd(p,r,t),pd(q,r,t),pd(r,p,q),pd(t,p,q))
worst=99;bad=[]
for i in range(len(items)):
    for j in range(i+1,len(items)):
        A,B=items[i],items[j]
        if A[3]==B[3] and A[3]>=0: continue
        if not (A[1]&B[1]): continue
        d=segdist(*A[2],*B[2])-A[4]-B[4]
        if d<worst: worst=d
        if d<0.2: bad.append((round(d,3),A[5],A[3],A[2],B[5],B[3],B[2]))
print('min clearance between different nets: %.3f mm'%worst)
for b in sorted(bad)[:20]: print(b)
# edge clearance
W,H=72,47
emin=min(min(x,y,W-x,H-y)-it[4] for it in items if it[3]!=-2 for (x,y) in it[2])
print('min copper-to-edge: %.3f mm'%emin)
