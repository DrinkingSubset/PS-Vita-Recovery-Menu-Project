#!/usr/bin/env bash
# Increments the VERSION file by one digit: 1.0.8 -> 1.0.9 -> 1.1.0 -> 1.1.1 ...
# Each of the minor and patch digits runs 0-9, then rolls over into the next.
set -e
cd "$(dirname "$0")"
IFS=. read -r MAJ MIN PAT < VERSION
PAT=$((PAT + 1))
if [ "$PAT" -gt 9 ]; then PAT=0; MIN=$((MIN + 1)); fi
if [ "$MIN" -gt 9 ]; then MIN=0; MAJ=$((MAJ + 1)); fi
echo "$MAJ.$MIN.$PAT" > VERSION
echo "Version is now v$MAJ.$MIN.$PAT"
