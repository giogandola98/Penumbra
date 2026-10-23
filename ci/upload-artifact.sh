#!/bin/bash
# Upload a directory as one Forgejo Actions artifact (v4 protocol), without node.
# Why: the linux-podman runner uses the host executor and has no node, so actions/upload-artifact cannot run.
# Usage (inside a workflow step): ci/upload-artifact.sh NAME DIR
set -euo pipefail
exec python3 -I - "$@" <<'PY'
import base64, hashlib, io, json, os, sys, urllib.parse, urllib.request, zipfile

name, folder = sys.argv[1], sys.argv[2]
base = os.environ["ACTIONS_RESULTS_URL"].rstrip("/")
token = os.environ["ACTIONS_RUNTIME_TOKEN"]

# The run/job backend ids are carried in the runtime JWT ("scp": "Actions.Results:<run>:<job>").
payload = token.split(".")[1]
claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
scope = next(s for s in claims["scp"].split() if s.startswith("Actions.Results:"))
_, run_id, job_id = scope.split(":")

# Artifact content is a zip of the folder (what upload-artifact@v4 produces).
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(folder):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, folder))
blob = buf.getvalue()

def call(method, url_or_path, body=None, headers=None, rpc=True):
    url = base + "/twirp/github.actions.results.api.v1.ArtifactService/" + url_or_path if rpc else url_or_path
    h = {"Authorization": "Bearer " + token}
    if rpc:
        h["Content-Type"] = "application/json"
        body = json.dumps(body).encode()
    h.update(headers or {})
    req = urllib.request.Request(url, data=body, method=method, headers=h)
    with urllib.request.urlopen(req) as r:
        data = r.read()
    return json.loads(data) if rpc else data

ids = {"workflow_run_backend_id": run_id, "workflow_job_run_backend_id": job_id, "name": name}
res = call("POST", "CreateArtifact", dict(ids, version=4))
if not res.get("ok"):
    sys.exit("CreateArtifact failed, keys: %r" % sorted(res))
url = res.get("signed_upload_url") or res["signedUploadUrl"]
sep = "&" if "?" in url else "?"
blockid = base64.b64encode(b"block-0001").decode()
call("PUT", url + sep + "comp=block&blockid=" + urllib.parse.quote(blockid), blob, rpc=False)
blocklist = '<?xml version="1.0" encoding="UTF-8"?><BlockList><Latest>%s</Latest></BlockList>' % blockid
call("PUT", url + sep + "comp=blocklist", blocklist.encode(), rpc=False)
res = call("POST", "FinalizeArtifact", dict(ids, size=str(len(blob)), hash="sha256:" + hashlib.sha256(blob).hexdigest()))
if not res.get("ok"):
    sys.exit("FinalizeArtifact failed: %r" % res)
print("uploaded artifact %s (%d bytes)" % (name, len(blob)))
PY
