# libvirt-colo-extension
libvirt colo extension based on libvirt-v9.6.0

## how to install

```
meson setup build -Dsystem=true -Ddriver_qemu=enabled
ninja -C build
sudo ninja -C build install
sudo service libvirtd restart


```

## use

use qemu+ssh to connect peer
```
#virsh -c qemu+ssh://root@192.168.10.2/system
```