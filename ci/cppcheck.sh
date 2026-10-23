#!/bin/bash
# Static analysis of src/ (report only). Upstream code has many findings, so this never fails the build;
# the report (cppcheck-report.txt) is uploaded as a CI artifact for review.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 1
if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck not installed; skipping" | tee cppcheck-report.txt
    exit 0
fi
cppcheck --enable=warning,performance,portability --inline-suppr --quiet \
    --suppress=missingInclude -j"$(nproc)" \
    --template='{file}:{line}: {severity}: {id}: {message}' \
    src 2> cppcheck-report.txt || true
echo "cppcheck findings: $(wc -l < cppcheck-report.txt) (see cppcheck-report.txt)"
exit 0
