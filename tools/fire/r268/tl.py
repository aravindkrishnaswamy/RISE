exec(open('parse.py').read().split("R=list")[0])
import numpy as np,collections
R=[r for r in rows('metal-gpu-intervals.xml') if 'FireSequenceTest' in (r['process'][1] or '') and r['channel-name'][0]=='Compute' and r['event-depth'][0]=='0']
s=np.array([int(r['start'][0]) for r in R]);d=np.array([int(r['duration'][0]) for r in R]);cb=[r['cmdbuffer-id'][0] for r in R]
o=np.argsort(s);s=s[o];d=d[o];cb=[cb[i] for i in o];e=s+d
print('encoders',len(s),'cmdbuffers',len(set(cb)),'span',(e.max()-s.min())/1e9,'busy',d.sum()/1e9)
# merge busy intervals
busy=0;cur_s,cur_e=s[0],e[0];gaps=[]
for a,b in zip(s[1:],e[1:]):
 if a>cur_e:busy+=cur_e-cur_s;gaps.append((cur_e,a-cur_e));cur_s,cur_e=a,b
 else:cur_e=max(cur_e,b)
busy+=cur_e-cur_s;print('union busy',busy/1e9)
g=np.array([x[1] for x in gaps]);print('gaps',len(g),'total',g.sum()/1e9,'>10ms',(g>1e7).sum(),'sum>10ms',g[g>1e7].sum()/1e9,'>100ms',(g>1e8).sum(),g[g>1e8].sum()/1e9)
print('big gaps (start s, len ms):',[(round((a-s.min())/1e9,2),round(l/1e6)) for a,l in gaps if l>1e8])
print('encoder duration pct ms',np.percentile(d,[10,50,90,99]).round(-3)/1e6,'max',d.max()/1e6)
h=np.histogram(d/1e6,bins=[0,0.1,1,5,20,40,60,100,1000])
for a,b,c in zip(h[1],h[1][1:],h[0]):print(f' {a}-{b} ms: {c} encoders, {d[(d/1e6>=a)&(d/1e6<b)].sum()/1e9:.2f}s')
# per cmdbuffer
cbt=collections.defaultdict(lambda:[0,0,None]);
for a,dd,c in zip(s,d,cb):
 x=cbt[c];x[0]+=dd;x[1]+=1;x[2]=a if x[2] is None else x[2]
cbs=sorted(cbt.values(),key=lambda x:x[2])
print('per cmdbuffer: n_enc / gpu ms (first 80)')
print(' '.join(f'{x[1]}/{x[0]/1e6:.0f}' for x in cbs[:80]))
