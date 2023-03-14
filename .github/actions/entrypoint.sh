#!/bin/bash

git config --global --add safe.directory .
echo Y | make install-dep
make install-ext-deps
make build-release
make pkg-deb