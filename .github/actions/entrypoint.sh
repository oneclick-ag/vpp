#!/bin/bash

echo Y | make install-dep
make install-ext-deps
make build-release
make pkg-deb