import random, subprocess, re, sys
random.seed(777)
W,H = 7,6
def gen(target):
    while True:
        cols=[0]*W; cells=[[None]*H for _ in range(W)]; seq=''
        def wins(c,r,p):
            for dc,dr in ((1,0),(0,1),(1,1),(1,-1)):
                n=1
                for s in (1,-1):
                    cc,rr=c+dc*s,r+dr*s
                    while 0<=cc<W and 0<=rr<H and cells[cc][rr]==p: n+=1; cc+=dc*s; rr+=dr*s
                if n>=4: return True
            return False
        ok=True
        for ply in range(target):
            p=ply%2
            moves=[c for c in range(W) if cols[c]<H]
            random.shuffle(moves)
            placed=False
            for c in moves:
                if not wins(c,cols[c],p):
                    cells[c][cols[c]]=p; cols[c]+=1; seq+=str(c); placed=True; break
            if not placed: ok=False; break
        if ok: return seq
ref='Connect4-Strong-Solver/src/connect4/build/full_ab_search_w7_h6.out'
n=int(sys.argv[1]) if len(sys.argv)>1 else 40
mism=0
for i in range(n):
    ply=random.randint(4,40)
    seq=gen(ply)
    my=subprocess.run(['./c4','probe',seq],capture_output=True,text=True).stdout
    m=re.search(r'Value for side to move: (\w+)',my)
    r=subprocess.run([ref,seq],capture_output=True,text=True).stdout
    rm=re.search(r'res = (-?\d+)',r)
    mine=m.group(1); theirs={'100':'WIN','0':'DRAW','-100':'LOSS'}[rm.group(1)]
    tag='OK' if mine==theirs else 'MISMATCH'
    if mine!=theirs: mism+=1
    print(f'{i:3d} ply {ply:2d} seq {seq:<42s} mine={mine:<5s} ref={theirs:<5s} {tag}',flush=True)
print('TOTAL MISMATCHES:',mism)
