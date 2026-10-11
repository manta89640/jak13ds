import re,sys,collections
txt=re.sub(r'\x1b\[[0-9;]*m','',open(sys.argv[1]).read()).split('\n')
view=None; blocks=collections.defaultdict(list); i=0
while i<len(txt):
    l=txt[i]
    m=re.search(r'view (\S+)',l)
    if m and 'pad script' in l: view=m.group(1)
    if '[softstats]' in l and view:
        b={}; j=i+1
        while j<len(txt) and txt[j].startswith('  '):
            t=txt[j].split()
            if t[0]=='top': b['top']=txt[j]
            else:
                d={}; k=1
                while k<len(t)-1:
                    if t[k] in('draws','tris','px','atest','zpass','blend','invis','proc','texchg','stchg','progsw'): d[t[k]]=float(t[k+1]); k+=2
                    else: k+=1
                b[t[0]]=d
            j+=1
        blocks[view].append(b); i=j; continue
    i+=1
keys=['draws','tris','px','zpass','blend','invis','texchg','progsw']
for v,bl in blocks.items():
    if v=='end': continue
    use=bl[-3:-1] if len(bl)>3 else bl[-2:]
    print(f'== {v} ({len(bl)} windows, avg of last {len(use)})')
    names=sorted(set().union(*[set(b) for b in use])-{'top'})
    for n in names:
        avg={k:sum(b.get(n,{}).get(k,0) for b in use)/len(use) for k in keys}
        print(f'  {n:<16}'+' '.join(f'{k} {avg[k]:8.1f}' if k not in('px','zpass','blend','invis') else f'{k} {avg[k]/96000:5.2f}scr' for k in keys))
    print('  ',use[-1].get('top','')[:600])
