gastera1n - Fork of [gaster](https://github.com/0x7ff/gaster)

iPhone9,3(14.1, 15.6)

Features
- Boot into SSH ramdisk

SSH connect in Terminal, with password `alpine`:
```sh
iproxy 2222 22 &
ssh root@localhost -p2222
```

use `mountfs` to mount filesystems
