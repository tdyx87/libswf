#!/bin/bash

# GitHub API Push Script
REPO="tdyx87/libswf"
TOKEN="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"
BRANCH="dev"
BASE_SHA="715c0ff1c49eec31a7066c6cb859166964e867cb"

# Headers
AUTH_HEADER="Authorization: token $TOKEN"
API_URL="https://api.github.com/repos/$REPO"

echo "=== 1. Creating blobs for modified files ==="

# Function to create blob
create_blob() {
    local file_path="$1"
    local content=$(base64 -w 0 "$file_path")
    local json="{\"content\":\"$content\",\"encoding\":\"base64\"}"
    
    curl -s -X POST "$API_URL/git/blobs" \
        -H "$AUTH_HEADER" \
        -H "Content-Type: application/json" \
        -d "$json" | jq -r '.sha'
}

cd "$(dirname "$0")"

# Create blobs for each modified file
declare -A FILE_BLOBS

FILES=(
    "CMakeLists.txt"
    "README.md"
    "include/renderer/renderer.h"
    "samples/CMakeLists.txt"
    "samples/swf_extract.cpp"
    "samples/swf_viewer.cpp"
    "src/avm2/avm2_renderer_bridge.cpp"
    "src/renderer/renderer.cpp"
)

for file in "${FILES[@]}"; do
    echo "Creating blob for: $file"
    SHA=$(create_blob "$file")
    FILE_BLOBS["$file"]="$SHA"
    echo "  -> $SHA"
done

echo ""
echo "=== 2. Creating tree ==="

# Build tree JSON
TREE_ITEMS=""
for file in "${FILES[@]}"; do
    if [ -n "$TREE_ITEMS" ]; then
        TREE_ITEMS="$TREE_ITEMS,"
    fi
    TREE_ITEMS="$TREE_ITEMS{\"path\":\"$file\",\"mode\":\"100644\",\"type\":\"blob\",\"sha\":\"${FILE_BLOBS[$file]}\"}"
done

TREE_JSON="{\"base_tree\":\"$BASE_SHA\",\"tree\":[$TREE_ITEMS]}"

TREE_SHA=$(curl -s -X POST "$API_URL/git/trees" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$TREE_JSON" | jq -r '.sha')

echo "Tree SHA: $TREE_SHA"

echo ""
echo "=== 3. Creating commit ==="

COMMIT_JSON="{\"message\":\"修改: 添加 USE_GDI_PLUS 选项，默认使用软件渲染\",\"tree\":\"$TREE_SHA\",\"parents\":[\"$BASE_SHA\"]}"

COMMIT_SHA=$(curl -s -X POST "$API_URL/git/commits" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$COMMIT_JSON" | jq -r '.sha')

echo "Commit SHA: $COMMIT_SHA"

echo ""
echo "=== 4. Creating/Updating ref ==="

REF_JSON="{\"sha\":\"$COMMIT_SHA\",\"force\":true}"

curl -s -X PATCH "$API_URL/git/refs/heads/$BRANCH" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$REF_JSON"

echo ""
echo "=== Done! Pushed to $BRANCH branch ==="
