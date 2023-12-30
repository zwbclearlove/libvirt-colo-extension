#!/bin/bash
sudo ninja -C build
sudo ninja -C build install
sudo echo 'user = "root"' >> /etc/libvirt/qemu.conf
sudo echo 'group = "root"' >> /etc/libvirt/qemu.conf
sudo systemctl restart libvirtd

