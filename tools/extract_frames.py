import os, re, struct

MAGE_SRC = 'E:/code/ai passport/o-platform/components/human_display/human/mage'
OUT = 'E:/code/ai passport/o-platform/build/frames/human/mage'

FRAME_PATTERN = re.compile(r'static const uint8_t \w+_map\[\] = \{(.*?)\};', re.DOTALL)

os.makedirs(OUT, exist_ok=True)

for f in sorted(os.listdir(MAGE_SRC)):
    if not f.endswith('.c'):
        continue
    path = os.path.join(MAGE_SRC, f)
    with open(path, 'r', encoding='utf-8') as fp:
        text = fp.read()
    m = FRAME_PATTERN.search(text)
    if not m:
        print(f'  SKIP {f}: no map array found')
        continue
    data = bytes(int(b, 0) for b in m.group(1).replace('\n', '').split(',') if b.strip())
    # action name from filename: human_mage_walk_1 -> walk_1.bin
    base = f[len('human_mage_'):-len('.c')]
    out_path = os.path.join(OUT, f'{base}.bin')
    with open(out_path, 'wb') as out:
        out.write(data)
    print(f'  {base}.bin  {len(data)} bytes')

print(f'\nAll frames extracted to {OUT}')