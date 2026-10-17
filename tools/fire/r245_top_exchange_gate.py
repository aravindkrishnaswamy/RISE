#!/usr/bin/env python3
"""Hand-calculated staggered top-plane/sign/limiter/donor-separation controls."""
import numpy as np
from r245_top_exchange_localization import top_exchange

nx=ny=2; nz=1; cells=4; faces=20
raw=np.zeros(19*cells+9*faces,dtype='<f4')
rates=np.zeros((26,faces),dtype='<f4')
# Top-plane faces are 16..19, distinct from lower and side planes.
raw[9*cells+8*faces:9*cells+9*faces]=.5
raw[9*cells+faces+16:9*cells+faces+20]=[-1,1,2,-2]
raw[9*cells+9*faces:10*cells+9*faces]=[10,11,12,13]
rates[1,16:20]=[-3,3,-1,4]; rates[2,16:20]=[1,-1,4,-2]
rates[10,16:20]=[2,0,-2,0];rates[8,16:20]=[-5,6,7,-8]
# A bottom-plane sentinel catches selecting z=0 instead of z=nz.
rates[1,12:16]=999
native=raw.tobytes()+bytes(216);flux=bytes(72)+rates.tobytes()
mass,enthalpy,temperature,velocity,radius=top_exchange(native,flux,(nx,ny,nz),.5)
assert np.array_equal(mass,[-.25,.5,.5,.5])
assert np.array_equal(enthalpy,[-1.25,1.5,1.75,-2])
assert np.array_equal(temperature,[10,11,12,13])
assert np.array_equal(velocity,[-1,1,2,-2])
assert np.array_equal(radius,np.full(4,np.hypot(.25,.25)))
# Incoming mass comes from the sign of total species (Z excluded), not
# separate positive/negative species sums or endpoint velocity classification.
rates[0,16:20]=900000
assert np.array_equal(top_exchange(native,bytes(72)+rates.tobytes(),(2,2,1),.5)[0],mass)
for bad_native,bad_flux in ((native[:-1],flux),(native,flux[:-4])):
    try: top_exchange(bad_native,bad_flux,(2,2,1),.5)
    except ValueError as error: assert 'capsule layout' in str(error)
    else: raise AssertionError('missing operand accepted')
print('PASS hand-calculated top-plane/limited-sign/counterdiffusion/tracer/reference/endpoint controls and missing-layout REDs')
