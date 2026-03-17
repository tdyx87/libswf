#!/bin/bash

# GitHub API Push All Files Script
REPO="tdyx87/libswf"
TOKEN="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"
BRANCH="dev"

# Headers
AUTH_HEADER="Authorization: token $TOKEN"
API_URL="https://api.github.com/repos/$REPO"

cd "$(dirname "$0")"

# 获取 dev 分支当前 commit
CURRENT_SHA=$(curl -s "$API_URL/git/refs/heads/$BRANCH" -H "$AUTH_HEADER" | jq -r '.object.sha')
echo "Current dev branch commit: $CURRENT_SHA"

# 获取当前 tree SHA
BASE_TREE_SHA=$(curl -s "$API_URL/git/commits/$CURRENT_SHA" -H "$AUTH_HEADER" | jq -r '.tree.sha')
echo "Base tree SHA: $BASE_TREE_SHA"

# 文件列表
FILES=(
    "CMakeLists.txt"
    "README.md"
    "include/audio/audio_player.h"
    "include/avm2/avm2.h"
    "include/avm2/avm2_renderer_bridge.h"
    "include/common/bitstream.h"
    "include/common/gdiplus_fix.h"
    "include/common/profiler.h"
    "include/common/types.h"
    "include/parser/swfheader.h"
    "include/parser/swfparser.h"
    "include/parser/swftag.h"
    "include/renderer/renderer.h"
    "include/writer/swfwriter.h"
    "samples/as3_test.cpp"
    "samples/CMakeLists.txt"
    "samples/debug_swf.cpp"
    "samples/filter_blend_test.cpp"
    "samples/graphics_api_test.cpp"
    "samples/image_buffer_test.cpp"
    "samples/profile_test.cpp"
    "samples/software_renderer_test.cpp"
    "samples/swf_create_test.cpp"
    "samples/swf_extract.cpp"
    "samples/swf_info.cpp"
    "samples/swf_optimizer.cpp"
    "samples/swf_viewer.cpp"
    "src/audio/audio_player.cpp"
    "src/avm2/avm2.cpp"
    "src/avm2/avm2_renderer_bridge.cpp"
    "src/parser/swfheader.cpp"
    "src/parser/swfparser.cpp"
    "src/renderer/renderer.cpp"
    "src/writer/swfwriter.cpp"
    "swf_parser.py"
    "tests/CMakeLists.txt"
    "tests/test_avm2.cpp"
    "tests/test_parser.cpp"
    "tests/test_renderer.cpp"
    "tests/test_runner.cpp"
)

echo ""
echo "=== Creating blobs for all files ==="

# 创建临时文件存储 tree 项
TREE_JSON_FILE=$(mktemp)
echo '[' > "$TREE_JSON_FILE"

FIRST=true
for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "Creating blob for: $file"
        CONTENT=$(base64 -w 0 "$file")
        BLOB_JSON="{\"content\":\"$CONTENT\",\"encoding\":\"base64\"}"
        
        BLOB_SHA=$(curl -s -X POST "$API_URL/git/blobs" \
            -H "$AUTH_HEADER" \
            -H "Content-Type: application/json" \
            -d "$BLOB_JSON" | jq -r '.sha')
        
        echo "  -> SHA: $BLOB_SHA"
        
        # 添加到 tree JSON
        if [ "$FIRST" = true ]; then
            FIRST=false
        else
            echo ',' >> "$TREE_JSON_FILE"
        fi
        echo "{\"path\":\"$file\",\"mode\":\"100644\",\"type\":\"blob\",\"sha\":\"$BLOB_SHA\"}" >> "$TREE_JSON_FILE"
    else
        echo "Warning: File not found: $file"
    fi
done

echo ']' >> "$TREE_JSON_FILE"

echo ""
echo "=== Creating tree ==="

TREE_ITEMS=$(cat "$TREE_JSON_FILE")
FINAL_TREE_JSON="{\"base_tree\":\"$BASE_TREE_SHA\",\"tree\":$TREE_ITEMS}"

NEW_TREE_SHA=$(curl -s -X POST "$API_URL/git/trees" \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$FINAL_TREE_JSON" | jq -r '.sha')

echo "New tree SHA: $NEW_TREE_SHA"

rm "$TREE_JSON_FILE"

echo ""
echo "=== Creating commit ==="

COMMIT_JSON="{\"message\":\"完整代码推送: libswf 库所有源代码\",\"tree\":\"$NEW_TREE_SHA\",\"parents\":[\"$CURRENT_SHA\"]}"

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
echo "=== Done! All files pushed to $BRANCH branch ==="
echo "Commit URL: https://github.com/$REPO/commit/$NEW_COMMIT_SHA"
