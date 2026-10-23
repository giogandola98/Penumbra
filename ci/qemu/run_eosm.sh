#!/bin/bash
# EOS M (EOSM.202) entry point; see run_cam.sh for options. Without ROMs: SKIPPED, exit 0.
CAM=EOSM exec "$(dirname "${BASH_SOURCE[0]}")/run_cam.sh" "$@"
