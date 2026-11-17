import OpenEXR, Imath, numpy as np, sys
def load(p):
    f=OpenEXR.InputFile(p); dw=f.header()['dataWindow']; W=dw.max.x-dw.min.x+1; H=dw.max.y-dw.min.y+1
    ch=[np.frombuffer(f.channel(c,Imath.PixelType(Imath.PixelType.FLOAT)),dtype=np.float32).reshape(H,W) for c in 'RGB']
    return np.stack(ch,-1).astype(np.float64)
d=sys.argv[1]
for sc in ['lacquer','tidal']:
    b=[load(f"{d}/{sc}_base_{k}.exr") for k in (1,2)]; n=[load(f"{d}/{sc}_new_{k}.exr") for k in (1,2)]
    Y=lambda x:(x*[0.2126,0.7152,0.0722]).sum(-1)
    bm=[Y(x).mean() for x in b]; nm=[Y(x).mean() for x in n]
    print(sc,"mean base",bm,"new",nm,"rel",np.mean(nm)/np.mean(bm)-1)
    # noise: base1 vs base2 diff; signal: avg new - avg base
    B=(Y(b[0])+Y(b[1]))/2; N=(Y(n[0])+Y(n[1]))/2
    noise=np.abs(Y(b[0])-Y(b[1])); sig=N-B
    # blurred 8x8 blocks
    h,w=B.shape; k=8; hb,wb=h//k,w//k
    blk=lambda X:X[:hb*k,:wb*k].reshape(hb,k,wb,k).mean((1,3))
    Bb,Nb=blk(B),blk(N); nb=blk(Y(b[0]))-blk(Y(b[1]))
    rel=(Nb-Bb)/np.maximum(Bb,1e-4)
    sd=np.std(nb/np.maximum(Bb,1e-4))/np.sqrt(2)
    print("  8x8 block rel change: min %.3f max %.3f p1 %.3f p99 %.3f ; noise sd(rel of avg) ~%.3f"%(rel.min(),rel.max(),np.percentile(rel,1),np.percentile(rel,99),sd))
    idx=np.argsort(rel.ravel())
    for i in list(idx[:4])+list(idx[-4:]):
        y,x=divmod(i,wb); print("   block(%d,%d) base %.4f new %.4f rel %+.3f"%(x*k,y*k,Bb[y,x],Nb[y,x],rel[y,x]))
    # max pixel values (fireflies)
    print("  max px base",[Y(x).max() for x in b],"new",[Y(x).max() for x in n], "p99.99 base",[np.percentile(Y(x),99.99) for x in b],"new",[np.percentile(Y(x),99.99) for x in n])
