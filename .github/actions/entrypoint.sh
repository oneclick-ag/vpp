#!/bin/bash

git config --global --add safe.directory $(pwd)

echo Y | make install-dep
make install-ext-deps
make build-release
make pkg-deb