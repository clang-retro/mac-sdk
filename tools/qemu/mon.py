"""Drive a Mac OS 9 guest through the QEMU monitor socket (see boot.sh).

The guest has only a relative USB mouse, so clickat() finds the arrow
cursor in a screendump and steers it there. Screens go to $OS9_DIR/NAME.png.

    python3 mon.py shot cur          # screenshot to $OS9_DIR/cur.png
    python3 mon.py info status       # any other monitor command
"""
import os, socket, sys, time
DIR=os.environ.get('OS9_DIR', os.path.expanduser('~/os9'))
_s=None
def _sock():
    global _s
    if _s is None:
        _s=socket.socket(socket.AF_UNIX); _s.connect(os.path.join(DIR,'mon.sock')); _s.setblocking(False)
        cmd('mouse_set 2')
    return _s
def _drain():
    try:
        while _sock().recv(65536): pass
    except Exception: pass
def send(c):
    _sock().sendall((c+'\n').encode()); time.sleep(0.01); _drain()
def cmd(c, wait=0.5):
    _drain(); _sock().sendall((c+'\n').encode()); time.sleep(wait)
    out=b''
    try:
        while True: out+=_sock().recv(65536)
    except Exception: pass
    return out.decode(errors='replace')
def shot(name):
    p=os.path.join(DIR,name+'.ppm')
    cmd('screendump '+p, 1.0)
    from PIL import Image; Image.open(p).save(os.path.join(DIR,name+'.png'))
# Screen pixels per mouse_move unit, measured at 800x600.
RATIO=0.43
RX=0.43
def goto(x,y,step=4):
    for _ in range(8): send('mouse_move -600 -600')
    time.sleep(0.2)
    ux=int(round(x/RX)); uy=int(round(y/RATIO))
    while ux>0 or uy>0:
        dx=min(step,ux); dy=min(step,uy); ux-=dx; uy-=dy
        send('mouse_move %d %d'%(dx,dy)); time.sleep(0.02)
def press(): send("mouse_button 1"); time.sleep(0.15); send('mouse_button 0'); time.sleep(0.1)
def click(x,y,double=False):
    goto(x,y); time.sleep(0.3)
    for i in range(2 if double else 1): press()
def drag(x1,y1,x2,y2):
    goto(x1,y1); time.sleep(0.2); send('mouse_button 1'); time.sleep(0.3)
    ux=int(round((x2-x1)/RX)); uy=int(round((y2-y1)/RATIO))
    sx=1 if ux>=0 else -1; sy=1 if uy>=0 else -1; ux=abs(ux); uy=abs(uy)
    while ux>0 or uy>0:
        dx=min(5,ux); dy=min(5,uy); ux-=dx; uy-=dy
        send('mouse_move %d %d'%(sx*dx,sy*dy)); time.sleep(0.02)
    time.sleep(0.3); send('mouse_button 0')
def key(k): send('sendkey '+k); time.sleep(0.1)
def type_(s):
    m={' ':'spc','.':'dot','-':'minus','/':'slash',',':'comma'}
    for ch in s:
        if ch.isupper(): key('shift-'+ch.lower())
        else: key(m.get(ch,ch))
import numpy as np
# The arrow cursor: '#' is black, '.' is white.
_ART=["#","##","###","####","#####","######","#######","########","#####","##.##","#...##","....##",".....##",".....##"]
_PTS=[(r,c) for r,l in enumerate(_ART) for c,ch in enumerate(l) if ch=='#']
def find_cursor(name='loc'):
    shot(name)
    from PIL import Image
    a0=np.array(Image.open(os.path.join(DIR,name+'.png')).convert('L')).astype(int)
    a=np.pad(a0,((2,20),(2,20)),constant_values=255)
    H,W=a.shape
    dark=a<60; bright=a>200
    ok=np.ones((H-22,W-22),bool)
    for r,c in _PTS: ok&=dark[r+2:r+2+H-22,c+2:c+2+W-22]
    for r in range(8):
        n=len(_ART[r])
        ok&=bright[r+2:r+2+H-22, 2+n:2+n+W-22]
        ok&=bright[r+2:r+2+H-22, 1:1+W-22]
    ys,xs=np.nonzero(ok)
    if len(ys)==0: return None
    return int(xs[0]), int(ys[0])
def moveto(x,y,tries=8):
    for _ in range(tries):
        p=find_cursor()
        if p is None:
            goto(x,y); continue
        dx=x-p[0]; dy=y-p[1]
        if abs(dx)<=1 and abs(dy)<=1: return True
        ux=int(round(dx/RX)); uy=int(round(dy/RATIO))
        while ux or uy:
            sx=max(-4,min(4,ux)); sy=max(-4,min(4,uy)); ux-=sx; uy-=sy
            send('mouse_move %d %d'%(sx,sy)); time.sleep(0.03)
        time.sleep(0.3)
    return False
def clickat(x,y,double=False):
    moveto(x,y); send('mouse_move 2 0'); time.sleep(0.1); send('mouse_move -2 0'); time.sleep(0.3)
    for i in range(2 if double else 1): press()
if __name__=='__main__':
    if sys.argv[1]=='shot': shot(sys.argv[2])
    else: print(cmd(' '.join(sys.argv[1:])))
