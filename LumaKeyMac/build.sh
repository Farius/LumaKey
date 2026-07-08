#!/bin/sh
set -e
cd "$(dirname "$0")"
swiftc -O main.swift -o LumaKey
echo "Built ./LumaKey"
