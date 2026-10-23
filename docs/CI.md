# CI

CI runs on the self-hosted Forgejo (`<forgejo-server>`, repo `root/ML-Optimized`), never on GitHub Actions.
Workflow: `.forgejo/workflows/ci.yml`, triggered on push and pull_request.

## How it works

The runner `linux-podman-105` (label `linux-podman`) uses the *host* executor (Rocky 9, root, no node, no
`container:` support) but has podman. So the job:

1. fetches the commit with git (no `actions/checkout`, which needs node);
2. builds `ci/Containerfile` (Debian bookworm-slim pinned by digest, Arm GNU Toolchain 12.2.rel1 with
   sha256 check, make, python docutils/pycparser, zip, gcc, cppcheck). The image tag is the hash of the
   Containerfile, so it is rebuilt only when that file changes (first run ~6 min, later runs ~1.5 min);
3. runs inside the image: `ci/build.sh` (EOSM.202, `FATAL_WARNINGS=y`), `ci/host-tests.sh`
   (`make -C tests/host`, or "no host tests yet"), `ci/cppcheck.sh` (report only, never fails);
4. uploads `magiclantern.zip`, `build-report.txt`, `cppcheck-report.txt` as artifact `ml-optimized-eosm202`
   with `ci/upload-artifact.sh` (v4 protocol over curl/python, because JS actions cannot run without node).

## Run locally

    ci/build.sh          # sources ~/.local/opt/ml-env.sh if the toolchain is not on PATH
    ci/host-tests.sh
    ci/cppcheck.sh

Or in the same container as CI (needs podman):

    podman build -t ml-ci -f ci/Containerfile ci/
    podman run --rm -v "$PWD":/work -w /work ml-ci ci/build.sh

`build-report.txt` lists autoexec.bin size, `arm-none-eabi-size` output, module count in the zip, number of
`TASK_CREATE`/`task_create` occurrences in `src/`, and the delta against `ci/baseline.txt`.

Sizes embed absolute paths (`__FILE__`), so a local build in a different directory differs by a few tens of
bytes from CI. The baseline is taken from the CI container (`/work`); compare against that.

## Update the baseline

After an intentional size change is reviewed and merged, regenerate it from a CI-equivalent build
(podman command above, with `ci/build.sh --update-baseline`) and commit `ci/baseline.txt`.
