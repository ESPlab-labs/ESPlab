#!/bin/bash
# Push docs/site to esplab-labs/esplab-labs.github.io (the org Pages site)
# and enable Pages with the workflow build. Run after org_migration.sh.
set -e
ORG=esplab-labs
SRC="$(dirname "$0")/../docs/site"
WORK=/tmp/org-site

gh repo view $ORG/$ORG.github.io >/dev/null 2>&1 || \
  gh repo create $ORG/$ORG.github.io --public --description "ESPlab Labs — documentation home"

rm -rf $WORK && mkdir -p $WORK/.github/workflows
cd $WORK
git init -b main -q
git remote add origin "https://github.com/$ORG/$ORG.github.io.git"
cp -r $SRC/* .
cat > .github/workflows/deploy.yml <<'EOF'
name: deploy
on:
  push:
    branches: [main]
permissions:
  contents: read
  pages: write
  id-token: write
concurrency:
  group: pages
  cancel-in-progress: true
jobs:
  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/configure-pages@v5
      - uses: actions/upload-pages-artifact@v3
        with:
          path: .
      - id: deployment
        uses: actions/deploy-pages@v4
EOF
git add -A
git -c user.name="Joshua Simon" -c user.email="joshua@tycooncoder.dev" \
  commit -q -m "ESPlab Labs site: landing + VM docs"
git push -u origin main --force 2>&1 | tail -1

# enable Pages (workflow build)
gh api "/repos/$ORG/$ORG.github.io/pages" -X POST -f build_type=workflow 2>/dev/null || true
sleep 2
gh api "/repos/$ORG/$ORG.github.io/pages" -q '.build_type' 2>/dev/null && \
  echo "Pages enabled. Site will be live at https://$ORG.github.io"
