#!/bin/bash
# Move the three ESPlab repos under the esplab-labs org and finish wiring.
# PREREQ (manual, web UI only): create the org at
#   https://github.com/organizations/new  → name: esplab-labs
#   (invite TycoonCoder as owner when prompted)
# Then run:  bash tools/org_migration.sh
set -e
ORG=esplab-labs
REPOS="ESPlab ESPlab-WebUI ESP-On-Demand"

echo "1. verify org exists and TycoonCoder is a member"
gh api /orgs/$ORG -q '.login' || { echo "org $ORG not found — create it first"; exit 1; }
gh api /orgs/$ORG/members/TycoonCoder -q '.login' >/dev/null || {
  echo "TycoonCoder is not a member of $ORG yet — accept the invite"; exit 1; }

echo "2. transfer the three repos (this RETIRES the old TycoonCoder/... URLs with redirects)"
for r in $REPOS; do
  echo "   transferring $r"
  gh api "/repos/TycoonCoder/$r/transfer" -X POST \
     -f new_owner=$ORG --silent && echo "   $r -> $ORG ✓"
done

echo "3. flip local remotes"
cd /home/joshua/projects/esp32-cluster
git remote set-url origin "https://github.com/$ORG/ESPlab.git"
cd /tmp/eod-repo     && git remote set-url origin "https://github.com/$ORG/ESP-On-Demand.git"
cd /tmp/webui-repo   && git remote set-url origin "https://github.com/$ORG/ESPlab-WebUI.git"

echo "4. set org default repo + homepage links"
gh repo edit $ORG/ESPlab --homepage "https://esplab-labs.github.io"
gh api "/orgs/$ORG" -X PATCH -f default_repository_name=ESPlab -q '.default_repository'

echo "5. move Pages: enable Pages on org site repo"
gh repo create $ORG/$ORG.github.io --public --description "ESPlab Labs — documentation home" 2>/dev/null || true
echo "   (docs/site will be pushed to $ORG/$ORG.github.io by scripts/push_org_site.sh)"

echo "DONE. Old URLs redirect automatically; site next:"
echo "  bash tools/push_org_site.sh"
