import pickle,sys,random
code=pickle.load(open(sys.argv[1],'rb')); inv={v:k for k,v in code.items()}
D,T=pickle.load(open('extra.pkl','rb'))
def dec(b):
    o=[];i=0
    while i<len(b):
        for j in range(i+1,min(len(b),i+18)+1):
            if b[i:j] in inv: break
        else: return o+['{?}']
        s=inv[b[i:j]]
        if s=='\0': return o
        o.append(s);i=j
    return o
random.seed(int(sys.argv[3]) if len(sys.argv)>3 else 2)
for c in random.sample(list(D.values()),int(sys.argv[2])):
    o=dec(''.join(format(x,'08b') for x in c))
    print(''.join(s if len(s)==1 else '['+s+']' for s in o))
