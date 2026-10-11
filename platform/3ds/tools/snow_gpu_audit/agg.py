import re,sys,collections
sys.argv=[sys.argv[0],sys.argv[1]]
exec(open(sys.argv[0].replace('agg.py','summ.py')).read().split("keys=['draws'")[0])
keys=['draws','tris','px','zpass','blend','invis','texchg','progsw']
for v,bl in blocks.items():
    if v=='end': continue
    use=bl[-2:]
    def avg(n,k): return sum(b.get(n,{}).get(k,0) for b in use)/len(use)
    names=set().union(*[set(b) for b in use])-{'top'}
    grp=collections.defaultdict(lambda: collections.Counter())
    models=[]
    for n in names:
        g = 'merc(all models)' if n.startswith('m:') else ('merc-envmap' if n.endswith('-envmap') else (n.split('/')[1].rstrip('0123456789')+'-'+n[:2] if '/' in n else n))
        for k in keys: grp[g][k]+=avg(n,k)
        if n.startswith('m:') and not n.endswith('-envmap'): models.append((avg(n,'tris'),avg(n,'draws'),avg(n,'px')/96000,n[2:]))
    print(f'== {v}')
    for g in sorted(grp, key=lambda g:-grp[g]['tris']):
        d=grp[g]; print(f'  {g:<18} draws {d["draws"]:6.1f} tris {d["tris"]:8.0f} px {d["px"]/96000:5.2f}scr zpass {d["zpass"]/96000:5.2f} blend {d["blend"]/96000:5.2f} invis {d["invis"]/96000:5.2f} texchg {d["texchg"]:5.1f} progsw {d["progsw"]:4.1f}')
    models.sort(reverse=True)
    agg=collections.Counter(); cnt=collections.Counter()
    for t,dr,px,n in models:
        base=re.sub(r'-lod\d$','',n); agg[base]+=t
    print('   merc models (tris,draws,scr):', '; '.join(f'{n} {t:.0f}/{dr:.0f}/{px:.2f}' for t,dr,px,n in models[:12]))
