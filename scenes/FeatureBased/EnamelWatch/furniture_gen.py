#!/usr/bin/env python3
"""Generate the anOrdain dial furniture as thin gold RELIEF geometry (furniture.ply).

anOrdain STAMPS its markings (a pad-printed gold layer), so they are modelled as
real geometry -- flat gold quads just proud of the enamel top -- NOT an alpha
decal (the spectral PT does not honour material alpha masks).  A furniture MASK
is drawn with matplotlib (railroad chapter ring + even-Arabic numerals + brand
text), then run-length-merged per row into gold quads at z=-0.80 (dial radius
20.6).  Re-run after editing the layout: python3 furniture_gen.py
"""
import numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from PIL import Image
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MASK = os.path.join(HERE, "furniture_mask.png")
PLY  = os.path.join(HERE, "furniture.ply")
R    = 20.6     # dial radius (scene units)
Z    = -0.80    # just proud of the enamel top (-0.82)

def draw_mask():
    fig = plt.figure(figsize=(10,10), dpi=110); ax = fig.add_axes([0,0,1,1])
    ax.set_xlim(-1,1); ax.set_ylim(-1,1); ax.set_aspect('equal'); ax.axis('off')
    fig.patch.set_facecolor('black'); ax.set_facecolor('black')
    G='white'
    def pol(r,deg):
        a=np.deg2rad(90-deg); return r*np.cos(a), r*np.sin(a)
    for m in range(60):                                   # railroad minute track
        deg=m*6
        r0,r1,lw = (0.83,0.955,2.4) if m%5==0 else (0.895,0.95,1.0)
        x0,y0=pol(r0,deg); x1,y1=pol(r1,deg)
        ax.plot([x0,x1],[y0,y1],color=G,lw=lw,solid_capstyle='butt')
    for m in range(0,60,5):                               # 5-min numbers
        deg=m*6; x,y=pol(0.75,deg)
        ax.text(x,y,f"{m:02d}",color=G,ha='center',va='center',fontsize=11,rotation=-deg,family='serif')
    for h,deg in [(12,0),(2,60),(4,120),(6,180),(8,240),(10,300)]:  # hour numerals
        x,y=pol(0.66,deg); ax.text(x,y,str(h),color=G,ha='center',va='center',fontsize=34,family='serif',fontweight='light')
    ax.text(0, 0.42,"anOrdain",color=G,ha='center',va='center',fontsize=15,family='serif',style='italic')
    ax.text(0,-0.42,"VITREOUS ENAMEL",color=G,ha='center',va='center',fontsize=9,family='serif')
    fig.savefig(MASK,facecolor='black'); plt.close(fig)

def extrude():
    im=np.array(Image.open(MASK).convert("L")); H,W=im.shape; mask=im>128
    wx=lambda px:(px/(W-1)*2-1)*R; wy=lambda py:-((py/(H-1)*2-1))*R
    verts=[]; faces=[]
    for py in range(H):
        row=mask[py]; x=0
        while x<W:
            if row[x]:
                x0=x
                while x<W and row[x]: x+=1
                X0,X1=wx(x0),wx(x); Y0,Y1=wy(py+1),wy(py); b=len(verts)
                verts+=[(X0,Y0,Z),(X1,Y0,Z),(X1,Y1,Z),(X0,Y1,Z)]
                faces+=[(b,b+1,b+2),(b,b+2,b+3)]
            else: x+=1
    with open(PLY,"w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(verts)}\n")
        f.write("property float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\n")
        f.write(f"element face {len(faces)}\nproperty list uchar int vertex_indices\nend_header\n")
        for (x,y,z) in verts: f.write(f"{x:.4f} {y:.4f} {z:.4f} 0 0 1\n")
        for (a,b,c) in faces: f.write(f"3 {a} {b} {c}\n")
    print(f"furniture.ply: {len(faces)//2} quads, {len(faces)} tris")

if __name__=="__main__":
    draw_mask(); extrude()
