import pickle,sys,collections
code=pickle.load(open(sys.argv[1],'rb')); pre=sys.argv[2]; drop=sys.argv[3] if len(sys.argv)>3 else None
if drop: code.pop(drop)
inv={v:k for k,v in code.items()}
comp,plain=pickle.load(open('titles98.pkl','rb')); D,T=pickle.load(open('extra.pkl','rb'))
S=[''.join(format(x,'08b') for x in c) for c in list(comp.values())+list(D.values())]
cnt=collections.Counter(); ex=collections.defaultdict(list)
for b in S:
    i=0;o=''
    while i<len(b):
        if b.startswith(pre,i):
            nx=b[i+len(pre):i+len(pre)+int(sys.argv[4] if len(sys.argv)>4 else 6)]; cnt[nx]+=1
            # decode following
            j=i+len(pre); 
            ex[nx].append(o[-12:])
        for j in range(i+1,min(len(b),i+18)+1):
            if b[i:j] in inv: break
        else: break
        s=inv[b[i:j]]
        if s=='\0': break
        o+=s;i=j
for k,v in cnt.most_common(20): print(k,v,ex[k][:4])
