import sys, os
sys.path.insert(0, 'E:/code/code tools/esp-idf-v5.5.3/components/fatfs')
from wl_fatfsgen import WLFATFS

BACKUP = 'E:/code/ai passport/references/appfs-backup-20260830'
OUT = 'E:/code/ai passport/o-platform/build/data_partition.bin'

wl = WLFATFS(size=0x100000, long_names_enabled=True)
fs = wl.plain_fatfs

for f in os.listdir(BACKUP):
    fp = os.path.join(BACKUP, f)
    if not os.path.isfile(fp) or f == '_appfs-partition-image.bin':
        continue
    with open(fp, 'rb') as src:
        data = src.read()
    name, ext = os.path.splitext(f)
    ext = ext.lstrip('.')
    fs.create_file(name.upper(), extension=ext.upper())
    fs.write_content([f'{name.upper()}.{ext.upper()}'] if ext else [name.upper()], data)

wl.init_wl()
with open(OUT, 'wb') as dst:
    dst.write(wl.fatfs_binary_image)
print(f'Written {OUT} ({os.path.getsize(OUT)} bytes)')