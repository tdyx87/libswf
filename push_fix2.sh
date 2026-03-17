#!/bin/bash

# GitHub API Push Script for file end fix
REPO="tdyx87/libswf"
TOKEN="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"
BRANCH="dev"

AUTH_HEADER="Authorization: token $TOKEN"
API_URL="https://api.github.com/repos/$REPO"

cd "$(dirname "$0")"

# Get current dev branch commit
CURRENT_SHA=$(curl -s "$API_URL/git/refs/heads/$BRANCH" -H "$AUTH_HEADER" | jq -r '.object.sha')
echo "Current dev branch commit: $CURRENT_SHA"

BASE_TREE_SHA=$(curl -s "$API_URL/git/commits/$CURRENT_SHA" -H "$AUTH_HEADER" | jq -r '.tree.sha')
echo "Base tree SHA: $BASE_TREE_SHA"

# File to update
FILE="src/renderer/renderer.cpp"

echo ""
echo "=== Creating blob for $FILE ==="

CONTENT=$(base64 -w 0 "$FILE")
BLOB_JSON="{\"content\":\"$CONTENT\",\"encoding\":\"base64\"}"

BLOB_SHA=$(curl -s -X POST "$API_URL/git/blobs" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$BLOB_JSON" | jq -r '.sha')

echo "Blob SHA: $BLOB_SHA"

echo ""
echo "=== Creating tree ==="

TREE_JSON="{\"base_tree\":\"$BASE_TREE_SHA\",\"tree\":[{\"path\":\"$FILE\",\"mode\":\"100644\",\"type\":\"blob\",\"sha\":\"$BLOB_SHA\"}]}"

NEW_TREE_SHA=$(curl -s -X POST "$API_URL/git/trees" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$TREE_JSON" | jq -r '.sha')

echo "New tree SHA: $NEW_TREE_SHA"

echo ""
echo "=== Creating commit ==="

COMMIT_JSON="{\"message\":\"修复: 添加缺少的命名空间闭合括号\",\"tree\":\"$NEW_TREE_SHA\",\"parents\":[\"$CURRENT_SHA\"]}"

NEW_COMMIT_SHA=$(curl -s -X POST "$API_URL/git/commits" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$COMMIT_JSON" | jq -r '.sha')

echo "New commit SHA: $NEW_COMMIT_SHA"

echo ""
echo "=== Updating dev branch ==="

REF_JSON="{\"sha\":\"$NEW_COMMIT_SHA\",\"force\":true}"

curl -s -X PATCH "$API_URL/git/refs/heads/$BRANCH" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$REF_JSON"

echo ""
echo "=== Done! Fix pushed to $BRANCH branch ==="
