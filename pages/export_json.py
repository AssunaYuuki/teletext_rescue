# Собирает страницы из stream.t42 и пишет JSON: для каждой страницы — список
# снимков во времени (целые версии страницы), ряды восстановлены голосованием.
# Подстраницы (код S1–S4 заголовка: 0001, 0002 … у страницы, которая сама
# листается) собираются отдельно: копии рядов сравниваются и голосуют только
# внутри своей подстраницы, как в decode-orc (teletext_row_squasher: «copies are
# only combined within one run of one sub-page»). У версии поле 's' — код подстраницы.
# Пакет учитывается, только если адрес (MRAG) пришёл точным кодовым словом
# Хэмминга, а не исправленным (decode-orc address_attested).
import sys, json, collections
H8={}
for dd in range(16):
    b=[(dd>>i)&1 for i in range(4)]
    p1=1^b[0]^b[2]^b[3]; p2=1^b[0]^b[1]^b[3]; p3=1^b[0]^b[1]^b[2]
    p4=1^(p1^p2^p3^b[0]^b[1]^b[2]^b[3])
    H8[sum(v<<i for i,v in enumerate([p1,b[0],p2,b[1],p3,b[2],p4,b[3]]))]=dd
ham=H8.get
FPS_PKTS=26*25  # пакетов в секунду захвата

d=open(sys.argv[1],'rb').read()

# Границы полей. В записи соседние поля VBI не идут подряд по времени эфира
# (часы в заголовках идут ~2x быстрее записи), поэтому ряды магазина после
# границы поля относятся уже к другой странице. Фаза границы находится по
# пику разрывов последовательности рядов с периодом 13 строк (строк телетекста
# в поле); фаза дрейфует из-за выпавших строк, поэтому считается в окнах.
import numpy as np
_a=np.frombuffer(d,dtype=np.uint8).reshape(-1,42)
def _h(x): return ((x>>1)&1)|(((x>>3)&1)<<1)|(((x>>5)&1)<<2)|(((x>>7)&1)<<3)
_mag=_h(_a[:,0].astype(int))&7; _row=(_h(_a[:,0].astype(int))>>3)|(_h(_a[:,1].astype(int))<<1)
_ev=np.zeros(len(_a),bool); _l={}
for _i,(_m,_r) in enumerate(zip(_mag,_row)):
    if _r>24: continue
    if _m in _l and _r!=0 and _r!=_l[_m]+1: _ev[_i]=True
    _l[_m]=_r
boundary=np.zeros(len(_a),bool)
STEP,WIN=260,1040
for st in range(0,len(_a),STEP):
    w0=max(0,st+STEP//2-WIN//2); e=np.where(_ev[w0:w0+WIN])[0]+w0
    ph=np.bincount(e%13,minlength=13).argmax() if len(e) else 0
    for i in range(st,min(st+STEP,len(_a))):
        if i%13==ph: boundary[i]=True
# Точные границы: --lines line_pkt.npy (кадр x строка -> номер пакета, -1 нет),
# получено выравниванием пакетов по сырому capture.vbi (align_lines.py).
if '--lines' in sys.argv:
    lp=np.load(sys.argv[sys.argv.index('--lines')+1])
    boundary[:]=False
    FLD=int(sys.argv[sys.argv.index('--field')+1]) if '--field' in sys.argv else 16   # строк VBI на поле
    for fld in lp.reshape(-1,FLD):
        k=fld[fld>=0]
        if len(k): boundary[k.min()]=True
FIELD_RESET='--no-field-reset' not in sys.argv
# Пороги подтверждения: страница — не меньше MIN_TX заголовков, версия ряда —
# не меньше MIN_ROW копий. Для «сырого» потока 3/2; для уже очищенного .t42,
# где каждая страница записана один раз, нужно --sparse (1/1).
MIN_TX, MIN_ROW = (1, 1) if '--sparse' in sys.argv else (3, 2)
tx=collections.defaultdict(list)  # pid -> [(pkt_index, {row: bytes})]
# Сегмент: ряды одного магазина подряд внутри одного поля. Начинается с
# заголовка (страница известна) или с границы поля ("сирота": заголовок
# остался в пропущенном поле). Сирота, за которой в том же поле идёт
# заголовок страницы N, — хвост страницы, стоящей в цикле магазина перед N.
cur={}; last={}; orphans=[]; loose=[]; lasthdr={}; follow=collections.defaultdict(collections.Counter)
def close(mag, next_pid):
    seg=cur.get(mag)
    if seg is None: return
    if seg['pid'] is None:
        if next_pid and seg['rows']: orphans.append((next_pid,seg))
        elif seg['rows'] and seg.get('prev'): loose.append(seg)   # без заголовка до конца поля
    elif next_pid: follow[next_pid][seg['pid']]+=1
for i in range(0,len(d)-41,42):
    if FIELD_RESET and boundary[i//42]:
        for m in list(cur): close(m,None)
        cur={m:{'pid':None,'rows':{},'i':i//42,'prev':lasthdr.get(m)} for m in range(1,9)}; last={m:0 for m in range(1,9)}
    p=d[i:i+42]; a=ham(p[0]); b=ham(p[1])
    if a is None or b is None: continue
    mag=a&7 or 8; row=(a>>3)|(b<<1)
    if row==0:
        u=ham(p[2]); t=ham(p[3])
        if u is None or t is None or t>9 or u>9: cur[mag]=None; continue
        pid=f"{mag}{t}{u}"; close(mag,pid); lasthdr[mag]=pid
        sc=[ham(x) for x in p[4:8]]
        sub=f"{sc[3]&3:X}{sc[2]:X}{sc[1]&7:X}{sc[0]:X}" if None not in sc else None   # S4 S3 S2 S1
        rows={0:b'\x20'*8+p[10:42]}
        tx[pid].append((i//42,rows,sub)); cur[mag]={'pid':pid,'rows':rows,'i':i//42}; last[mag]=0
    elif 1<=row<=24 and cur.get(mag) is not None:
        if row<=last.get(mag,0): cur[mag]=None; continue  # заголовок новой страницы потерян
        cur[mag]['rows'][row]=p[2:42]; last[mag]=row
# хвосты-сироты: страницы в магазине идут по возрастанию номера (пары
# заголовков внутри одного поля это подтверждают), поэтому предшественник
# N — предыдущая существующая страница магазина. Часто повторяемые страницы
# (100 и т.п.) вставляются вне порядка — перед ними сирот не приписываем.
hdrs=collections.Counter({k:len(v) for k,v in tx.items()})
med=sorted(hdrs.values())[len(hdrs)//2] if hdrs else 0
known=sorted(k for k,n in hdrs.items() if n>=3)
def prev_page(p):
    same=[k for k in known if k[0]==p[0] and k<p]
    return same[-1] if same else None
for nxt,seg in orphans:
    if hdrs[nxt]>2*med: continue
    c=follow.get(nxt)
    prev=c.most_common(1)[0][0] if c else prev_page(nxt)
    if prev is None or hdrs[prev]>2*med: continue
    tx[prev].append((seg['i'],seg['rows'],None))
# Сироты без заголовка до конца поля: страница — A, A+1 или A+2 по циклу
# магазина (A — последний заголовок перед пропуском; скачок 1/2/3 бывает в
# 29/42/17% случаев). Выбираем по содержимому: ряд должен почти совпасть с
# уже надёжно привязанным рядом ровно одного кандидата. Строки, которые есть
# на многих страницах (подвалы, рамки), для сравнения не годятся.
def informative(b): return sum(c not in (0x20,0xa0) for c in b)>=12
CONTENT_ROWS=range(5,22)   # ряды 1-4 (логотип) и 22-24 (навигация) одинаковы на многих страницах
def informative_idx():
    idx=collections.defaultdict(list)          # (mag,row) -> [(bytes,pid)]
    for pid,T in tx.items():
        for _,rows,_s in T:
            for r,b in rows.items():
                if r in CONTENT_ROWS and informative(b): idx[(pid[0],r)].append((b,pid))
    return idx
def near(a,b): return sum(x!=y for x,y in zip(a,b))<=4
def cand_pages(A):
    same=[k for k in known if k[0]==A[0]]
    if A not in same: return []
    j=same.index(A); return same[j:j+3]
added=0
for _pass in range(4):                      # привязанные куски пополняют базу
    idx=informative_idx(); rest=[]; n0=added
    for seg in loose:
        C=cand_pages(seg['prev'])
        if not C: continue
        hits=collections.Counter()
        for r,b in seg['rows'].items():
            if r not in CONTENT_ROWS or not informative(b): continue
            owners={pid for bb,pid in idx[(seg['prev'][0],r)] if near(bb,b)}
            if len(owners)==1: hits.update(owners & set(C))
        if len(hits)==1 or (len(hits)>1 and hits.most_common(2)[1][1]*3<=hits.most_common(1)[0][1]):
            tx[hits.most_common(1)[0][0]].append((seg['i'],seg['rows'],None)); added+=1
        else: rest.append(seg)
    loose=rest
    if added==n0: break
print('orphans by content:',added,'remaining',len(loose))
# Оставшиеся куски группируем по совпадающим рядам (одна и та же часть
# страницы, принятая в разных передачах); у каждой копии свой набор
# кандидатов A..A+2, и их пересечение по группе обычно однозначно.
parent=list(range(len(loose)))
def find(x):
    while parent[x]!=x: parent[x]=parent[parent[x]]; x=parent[x]
    return x
buckets=collections.defaultdict(list)
for si,seg in enumerate(loose):
    for r,b in seg['rows'].items():
        if r in CONTENT_ROWS and informative(b): buckets[(seg['prev'][0],r)].append((si,b))
for items in buckets.values():
    M=np.array([list(b) for _,b in items],dtype=np.uint8); ids=[si for si,_ in items]
    for a0 in range(0,len(ids),512):
        D=(M[a0:a0+512,None,:]!=M[None,:,:]).sum(2)
        for i,j in zip(*np.where(D<=3)):
            x,y=find(ids[a0+i]),find(ids[j])
            if x!=y: parent[x]=y
groups=collections.defaultdict(list)
for si in range(len(loose)): groups[find(si)].append(si)
gadded=0
for members in groups.values():
    if len(members)<2: continue
    C=None
    for si in members:
        c=set(cand_pages(loose[si]['prev']))
        C=c if C is None else C&c
    if C and len(C)==1:
        pg=C.pop()
        for si in members: tx[pg].append((loose[si]['i'],loose[si]['rows'],None)); gadded+=1
print('orphans by candidate intersection:',gadded)
for v in tx.values(): v.sort(key=lambda x:x[0])

# Подстраница для кусков без заголовка (сирот): по совпадению рядов с передачами
# каждой подстраницы; если не ясно — подстраница ближайшего предыдущего заголовка.
def assign_subs(T):
    main=collections.Counter(s for _,_,s in T if s is not None)
    if not main: return [(i,r,'0000') for i,r,_ in T]
    known={s for s,n in main.items() if n>=MIN_TX} or {main.most_common(1)[0][0]}
    if len(known)==1:
        only=next(iter(known)); return [(i,r,only) for i,r,s in T if s is None or s in known]
    ref=collections.defaultdict(list)                 # ряд -> [(байты, подстраница)]
    for _,rows,s in T:
        if s in known:
            for r,b in rows.items():
                if r in CONTENT_ROWS and informative(b): ref[r].append((b,s))
    out=[]; lastsub=None
    for i,rows,s in T:
        if s is not None:
            if s in known: lastsub=s; out.append((i,rows,s))
            continue
        hits=collections.Counter()
        for r,b in rows.items():
            if r in CONTENT_ROWS and informative(b):
                own={ss for bb,ss in ref[r] if near(bb,b)}
                if len(own)==1: hits.update(own)
        if hits and (len(hits)==1 or hits.most_common(2)[1][1]*3<=hits.most_common(1)[0][1]):
            out.append((i,rows,hits.most_common(1)[0][0]))
        elif lastsub: out.append((i,rows,lastsub))
    return out

def dist(x,y): return sum(a!=b for a,b in zip(x,y))
def vote(mem):
    out=[]
    for k in range(40):
        c=collections.Counter(m[k] for m in mem if bin(m[k]).count('1')%2==1)
        out.append((c.most_common(1)[0][0] if c else 0x20)&0x7f)
    return out

res={}
def build(T):
    # 1) кластеры версий для каждого ряда
    reps=collections.defaultdict(list)  # row -> [[rep, members]]
    lab=[]  # для каждой передачи: {row: cluster_id}
    for _,rows in T:
        L={}
        for r,s in rows.items():
            cl=reps[r]
            for ci,c in enumerate(cl):
                if dist(c[0],s)<=6: c[1].append(s); L[r]=ci; break
            else: cl.append([s,[s]]); L[r]=len(cl)-1
        lab.append(L)
    size={r:[len(c[1]) for c in cl] for r,cl in reps.items()}
    # 2) проход по времени: состояние меняется, когда новая версия ряда подтверждена (>=2 раза в кластере)
    state={}; snaps=[]; start=T[0][0]
    for (idx,_),L in zip(T,lab):
        changed=False
        for r,ci in L.items():
            if r==0: continue
            if state.get(r)!=ci and size[r][ci]>=MIN_ROW:
                if r in state: changed=True   # ряд изменился — новая версия
                state[r]=ci                   # ряд впервые пришёл — дополняем текущую
                if snaps and not changed: snaps[-1]['st'][r]=ci
        if changed or not snaps:
            snaps.append({'st':dict(state),'t0':idx,'t1':idx,'n':1})
        else:
            snaps[-1]['t1']=idx; snaps[-1]['n']+=1
    # ряды, впервые пришедшие позже, дописываем в более ранние версии
    for k in range(len(snaps)-2,-1,-1):
        for r,ci in snaps[k+1]['st'].items(): snaps[k]['st'].setdefault(r,ci)
    # склеиваем первые неполные состояния: снимок валиден, если держался >=2 передач
    good=[s for s in snaps if s['n']>=MIN_ROW] or [max(snaps,key=lambda s:s['n'])]
    # дедуп одинаковых состояний
    seen={}; out=[]
    hdr=vote(reps[0][max(range(len(reps[0])),key=lambda c:len(reps[0][c][1]))][1]) if 0 in reps else [32]*40
    for s in good:
        key=tuple(sorted(s['st'].items()))
        if key in seen: seen[key]['n']+=s['n']; continue
        snap={'t':round((s['t0'])/FPS_PKTS,1),'n':s['n'],
              'rows':{'0':hdr,**{str(r):vote(reps[r][ci][1]) for r,ci in s['st'].items()}},
              'c':{str(r):len(reps[r][ci][1]) for r,ci in s['st'].items()}}
        seen[key]=snap; out.append(snap)
    return out
nsub=collections.Counter()
for pid,T in sorted(tx.items()):
    if len(T)<MIN_TX: continue
    by=collections.defaultdict(list)
    for i,rows,s in assign_subs(T): by[s].append((i,rows))
    out=[]
    for sub in sorted(by):
        if len(by[sub])<MIN_TX and len(by)>1: continue
        snaps=build(by[sub])
        for sn in snaps: sn['s']=sub
        out+=snaps
    if out: res[pid]=out; nsub[min(len({sn['s'] for sn in out}),5)]+=1
# Ряд с одинаковым содержимым, попавший на несколько страниц, — почти
# всегда чужой на тех, где у него мало подтверждений: оставляем его только
# на странице с наибольшим числом копий (если перевес хотя бы втрое).
occ=collections.defaultdict(list)     # (row, bytes) -> [(count, pid, snap)]
for pid,snaps in res.items():
    for sn in snaps:
        for r,b in sn['rows'].items():
            if int(r) in CONTENT_ROWS and informative(b): occ[(r,bytes(b))].append((sn['c'].get(r,0),pid,sn))
dropped=0
for key,L in occ.items():
    best=max(c for c,_,_ in L)
    bestp={p for c,p,_ in L if c==best}
    if len({p for _,p,_ in L})<2: continue
    for c,p,sn in L:
        if p not in bestp and c*3<=best:
            sn['rows'].pop(key[0],None); sn['c'].pop(key[0],None); dropped+=1
print('foreign rows removed:',dropped)
json.dump(res,open(sys.argv[2],'w'),separators=(',',':'))
nv=collections.Counter(min(len(v),5) for v in res.values())
print(len(res),'pages; versions per page:',dict(sorted(nv.items())),'; subpages:',dict(sorted(nsub.items())))
