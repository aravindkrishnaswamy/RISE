import xml.etree.ElementTree as ET,collections,re
ids={};self_=collections.Counter();incl=collections.Counter();thr=collections.Counter();n=0
for ev,el in ET.iterparse('time-profile.xml',events=('end',)):
 if el.tag!='row':continue
 def res(c):
  if 'ref' in c.attrib:return ids[c.attrib['ref']]
  for d in c.iter():
   if 'id' in d.attrib:ids[d.attrib['id']]=d
  return c
 t=None;bt=None;w=1
 for c in el:
  c=res(c)
  if c.tag=='thread':t=c.attrib.get('fmt')
  if c.tag in('backtrace','tagged-backtrace'):bt=c
  if c.tag=='weight':w=int(c.text)
  if c.tag=='thread-state':st=c.attrib.get('fmt')
 n+=1;thr[t]+=w
 if 'Main' not in (t or ''):continue
 if bt is None:continue
 fr=[]
 for f in bt.findall('frame'):
  f=ids[f.attrib['ref']] if 'ref' in f.attrib else f
  if 'id' in f.attrib:ids[f.attrib['id']]=f
  fr.append(re.sub(r'\((?!anonymous).*','',f.attrib.get('name') or '?')[:100])
 if fr:self_[fr[0]]+=w
 for x in set(fr):incl[x]+=w
 el.clear()
tot=sum(thr.values());print('samples',n,'total ms',tot/1e6)
for k,v in thr.most_common(6):print(f'{v/1e6:9.0f} ms thread {k}')
print('--- self');[print(f'{v/1e6:8.0f} ms {k}') for k,v in self_.most_common(30)]
print('--- inclusive');[print(f'{v/1e6:8.0f} ms {k}') for k,v in incl.most_common(70)]
