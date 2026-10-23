#!/bin/bash
echo probing; env | grep -E '^(ACTIONS_|GITHUB_RUN_ID|FORGEJO_RUN_ID)' | sed 's/\(TOKEN=\).*/\1<hidden>/'
