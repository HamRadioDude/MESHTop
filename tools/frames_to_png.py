import sys, glob, os
from PIL import Image
for f in sorted(glob.glob(sys.argv[1] + '/*.rgb565')):
    d = open(f, 'rb').read()
    im = Image.frombytes('RGB', (800, 480), d, 'raw', 'BGR;16')
    im.save(f[:-7] + '.png')
    print(f[:-7] + '.png')
