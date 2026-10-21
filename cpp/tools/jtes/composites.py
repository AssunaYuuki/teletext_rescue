# tu-tx100-mosaic-composites.json -> composites.txt for mosaictest.cpp
import json, os, sys

here = os.path.dirname(os.path.abspath(__file__))
d = json.load(open(os.path.join(here, 'tu-tx100-mosaic-composites.json'), encoding='utf-8'))
with open(os.path.join(here, 'composites.txt'), 'w', newline='\n') as f:
    for stl, key in ((0, 'plain'), (1, 'underlined')):
        for pair, hexs in zip(d['pairs'], d[key]):
            a, b, c, e = [x if x else '-' for x in pair]
            f.write(f'{a} {b} {c} {e} {stl} {hexs}\n')
