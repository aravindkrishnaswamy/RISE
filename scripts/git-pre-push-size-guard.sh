#!/bin/sh
# Pre-push guard: refuse pushes that would carry oversize blobs to GitHub.
# Install: cp scripts/git-pre-push-size-guard.sh .git/hooks/pre-push && chmod +x .git/hooks/pre-push
# Limits: any single blob > 50 MB, or > 1 GB total new blob bytes per ref.
MAX_BLOB=$((50*1024*1024)); MAX_TOTAL=$((1024*1024*1024)); zero=0000000000000000000000000000000000000000
status=0
while read local_ref local_sha remote_ref remote_sha; do
  [ "$local_sha" = "$zero" ] && continue
  if [ "$remote_sha" = "$zero" ]; then range="$local_sha --not --remotes"; else range="$remote_sha..$local_sha"; fi
  git rev-list --objects $range 2>/dev/null | git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' | awk -v mb=$MAX_BLOB -v mt=$MAX_TOTAL -v ref="$local_ref" '
    $1=="blob" { total+=$2; if ($2>mb) { big++; printf "  %6.1f MB  %s\n", $2/1048576, $3 } }
    END { if (big>0 || total>mt) { printf "pre-push guard: REFUSING %s (%d blobs over 50 MB, %.2f GB new blob bytes)\nMove run artifacts out of git (keep SHA manifests) — see docs/FIRE_SMOKE_COMMIT_MAP_ARCHIVE_STRIP.md\n", ref, big, total/1073741824; exit 1 } }' || status=1
done
exit $status
