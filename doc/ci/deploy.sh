#!/bin/sh
# Push a mkdocs build to MinIO under docs/<NAME>/<VERSION>/, refresh the
# `meta` file from the bucket's real state, and update `latest/` redirect
# only if the deployed version is the highest semver-sorted one.
#
# Inputs (env vars, in priority order):
#   NAME        | CI_PROJECT_NAME    : software name (also bucket prefix)
#   VERSION     | CI_COMMIT_TAG      : version being published
#   MINIO_URL                          : full URL with scheme (http://host:port)
#   MINIO_ACCESS_KEY
#   MINIO_SECRET_KEY
#
# Expects ./site/ (mkdocs build output) and ./doc/ci/templates/ (meta + latest templates).
set -eu

: "${NAME:=${CI_PROJECT_NAME:-}}"
: "${VERSION:=${CI_COMMIT_TAG:-${CI_COMMIT_SHORT_SHA:-}}}"

if [ -z "$NAME" ] || [ -z "$VERSION" ]; then
    echo "ERROR: NAME and VERSION required (or CI_PROJECT_NAME / CI_COMMIT_TAG / CI_COMMIT_SHORT_SHA)" >&2
    exit 1
fi

echo "[deploy] installing mc + envsubst..."
apk add --no-cache curl gettext >/dev/null
curl -sSLo /usr/local/bin/mc https://dl.min.io/client/mc/release/linux-amd64/mc
chmod +x /usr/local/bin/mc

echo "[deploy] connecting to ${MINIO_URL}..."
mc alias set m "$MINIO_URL" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" >/dev/null

echo "[deploy] mirroring ./site -> docs/${NAME}/${VERSION}/"
mc mirror --overwrite --remove ./site/ "m/docs/${NAME}/${VERSION}/"

echo "[deploy] enumerating versions present in bucket..."
mc ls "m/docs/${NAME}/" \
    | awk '{print $NF}' \
    | tr -d '/' \
    | grep -vE '^(latest|meta)$' \
    | sort -V > /tmp/versions.txt

LATEST=$(tail -n1 /tmp/versions.txt)
VERSIONS=$(awk '{printf "\"%s\",",$0}' /tmp/versions.txt | sed 's/,$//')
export NAME LATEST VERSIONS

echo "[deploy] versions=[${VERSIONS}], latest=${LATEST}"

echo "[deploy] writing meta..."
envsubst < ./doc/ci/templates/meta.tpl.json > /tmp/meta.json
mc cp /tmp/meta.json "m/docs/${NAME}/meta"

if [ "$VERSION" = "$LATEST" ]; then
    echo "[deploy] ${VERSION} is the highest version, updating latest/ redirect"
    envsubst < ./doc/ci/templates/latest.tpl.html > /tmp/latest.html
    mc cp /tmp/latest.html "m/docs/${NAME}/latest/index.html"
else
    echo "[deploy] ${VERSION} is older than ${LATEST}, leaving latest/ untouched"
fi

echo "[deploy] done."
