import math,sys
from pathlib import Path
starts=[127,156,185,214,303,332,1,30]
radius_lo=205.0
radius_hi=218.0
N=5
subpixels=[(i+0.5)/N-0.5 for i in range(N)]
out=Path(sys.argv[1])
entries=[]
with out.open('w') as of:
    print('#pragma once\n#include <cstdint>\nnamespace signal_mask {',file=of)
    for i,start in enumerate(starts):
        lo=math.radians(start); hi=math.radians(start+24)
        samples=[(r*math.cos(lo+(hi-lo)*n/96),r*math.sin(lo+(hi-lo)*n/96))
                 for n in range(97) for r in (radius_lo,radius_hi)]
        left=math.floor(min(x for x,y in samples))-1
        top=math.floor(min(y for x,y in samples))-1
        right=math.ceil(max(x for x,y in samples))+1
        bottom=math.ceil(max(y for x,y in samples))+1
        width=right-left+1
        if width%2: width+=1
        h=bottom-top+1
        raw=[]
        for y in range(top,bottom+1):
            row=[]
            for x in range(left,left+width):
                count=0
                for dy in subpixels:
                    py=y+dy
                    for dx in subpixels:
                        px=x+dx
                        rr=px*px+py*py
                        if radius_lo**2<=rr<=radius_hi**2:
                            theta=math.atan2(py,px)%(2*math.pi)
                            count+=lo<=theta<=hi
                row.append(round(count*15/(N*N)))
            raw.extend((a<<4)|b for a,b in zip(row[::2],row[1::2]))
        assert len(raw)==width*h//2
        print(f'inline constexpr uint8_t arc_{i}[]={{',file=of)
        for n in range(0,len(raw),28): print('  '+','.join(f'0x{v:02x}' for v in raw[n:n+28])+',',file=of)
        print('};',file=of)
        entries.append(f'{{{left},{top},{width},{h},arc_{i}}}')
    print('struct Arc { int16_t x,y,w,h; const uint8_t* pixels; };',file=of)
    print('inline constexpr Arc arcs[]={'+','.join(entries)+'};\n}',file=of)
