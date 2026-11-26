import xml.etree.ElementTree as ET,collections,sys
def rows(path):
 ids={};cols=None
 for ev,el in ET.iterparse(path,events=('end',)):
  if el.tag=='schema':cols=[c.findtext('mnemonic') for c in el.findall('col')]
  if el.tag=='row':
   vals=[]
   for c in el:
    if 'ref' in c.attrib:c2=ids[c.attrib['ref']]
    else:
     c2=c
     for d in c.iter():
      if 'id' in d.attrib:ids[d.attrib['id']]=d
    vals.append((c2.text,c2.attrib.get('fmt')))
   yield dict(zip(cols,vals));el.clear()
R=list(rows('metal-gpu-intervals.xml'))
print('rows',len(R))
ch=collections.Counter();n=collections.Counter()
for r in R:
 c=r.get('channel-name',(None,))[1] or r.get('channel-name',(None,))[0];ch[c]+=int(r['duration'][0]);n[c]+=1
for c,v in ch.most_common():print(f'channel {c}: {v/1e9:.3f} s over {n[c]} intervals')
t0=min(int(r['start'][0]) for r in R);t1=max(int(r['start'][0])+int(r['duration'][0]) for r in R);print('span',(t1-t0)/1e9)
lab=collections.Counter();ln=collections.Counter();dep=collections.Counter()
for r in R:
 c=r.get('channel-name',(None,None));
 l=(r.get('event-label') or (None,None));l=l[1] or l[0]
 d=r.get('event-depth',(None,None))[0]
 dep[(c[1] or c[0],d)]+=1
 lab[(c[1] or c[0],d,(l or '')[:90])]+=int(r['duration'][0]);ln[(c[1] or c[0],d,(l or '')[:90])]+=1
print(dep)
for k,v in lab.most_common(60):print(f'{v/1e9:8.3f}s {ln[k]:7d}  {k}')
