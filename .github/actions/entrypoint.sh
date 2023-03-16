#!/bin/bash

git config --global --add safe.directory $(pwd)

echo Y | make install-dep
make install-ext-deps
make build-release
make pkg-deb

cd build-root
tar -czvf oneclick-vpp.tar.gz vpp_*.deb vpp-plugin-core_*.deb vpp-plugin-dpdk_*.deb libvppinfra_*.deb