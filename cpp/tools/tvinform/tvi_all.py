import numpy as np,sys
f=sys.argv[1]; li=int(sys.argv[2]); rate=float(sys.argv[3]); nb=int(sys.argv[4]); out=sys.argv[5]
a=np.memmap(f,dtype=np.uint8,mode='r').reshape(-1,32,2048)
T=35468950/rate; N=a.shape[0]
res=np.zeros((N,nb),np.uint8); ok=np.zeros(N,bool); q=np.zeros(N,np.float32)
for fr in range(N):
    s=a[fr,li,:].astype(np.float32); lo,hi=np.percentile(s,5),np.percentile(s,95)
    if hi-lo<40: continue
    thr=(lo+hi)/2; st=int(np.argmax(s>thr))
    if st<20 or st>400: continue
    d=s-thr; z=np.where(d[:-1]*d[1:]<0)[0]; z=z[z>=st]
    if len(z)<4: continue
    e=z+(thr-s[z])/(s[z+1]-s[z]); k=np.round((e-e[0])/T); m=np.abs((e-e[0])/T-k)<0.3
    T2,o2=T,e[0]
    if m.sum()>5:
        A=np.vstack([k[m],np.ones(m.sum())]).T; t,o=np.linalg.lstsq(A,e[m],rcond=None)[0]
        if abs(t/T-1)<0.05: T2,o2=t,o
    idx=np.minimum(2047,(o2+(np.arange(nb)+0.5)*T2).astype(int))
    v=s[idx]; res[fr]=v>thr; ok[fr]=True; q[fr]=np.min(np.abs(v-thr))/(hi-lo)
np.savez(out,bits=res,ok=ok,q=q)
print(N,'frames',ok.sum(),'sliced')
