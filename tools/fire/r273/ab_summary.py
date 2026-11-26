#!/usr/bin/env python3
"""r273 tier-6 A/B summary from ab_frames tables. usage: ab_summary.py <yield> <ab_frames output> [more yield/table pairs]
Frames are 1/16 s apart from 2.5 s; the state had no soot at 2.414 s and nothing leaves the domain in 0.6 s, so
d(inventory)/dt = formed - oxidised. Fuel rate = (HRR + d(inventory)/dt * Hc) / LHV; formed = yield * fuel."""
import sys
LHV,HC,DT=50.03e6,32.76e6,0.0625
print("arm                 yield  HRR_kW  fuel_g/s formed_mg/s net_mg/s unburned_share  inv_3.0s_mg hot_share hot_vol_L peak_T  Tc0.1  Tc0.3  Tc0.6")
for i in range(1,len(sys.argv),2):
    y=float(sys.argv[i]);rows=[[float(v) for v in l.split()] for l in open(sys.argv[i+1]) if l.strip() and not l.startswith('#')]
    n=len(rows);span=(n-1)*DT
    trap=lambda c:sum((rows[k][c]+rows[k+1][c])*0.5 for k in range(n-1))/(n-1)
    hrr=trap(5)*1e3;net=(rows[-1][1]-rows[0][1])*1e-6/span;fuel=(hrr+net*HC)/LHV;formed=y*fuel
    name=sys.argv[i+1].split('/')[-1].replace('.frames.txt','')
    print(f"{name:19s} {y:5.2f}  {hrr*1e-3:6.2f}  {fuel*1e3:7.3f}  {formed*1e6:9.2f}  {net*1e6:7.2f}    {net/formed:6.3f}       {rows[-1][1]:8.3f}    {trap(2):5.3f}   {trap(4):6.2f}  {trap(6):6.0f} {trap(7):6.0f} {trap(8):6.0f} {trap(9):6.0f}")
