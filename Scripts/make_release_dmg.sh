#!/bin/bash
# Scripts/make_release_dmg.sh  [--no-build] [--no-notarize] [--keychain-profile NAME]
# ---------------------------------------------------------------------------
# Build a distributable, notarized .dmg of MX68K.
#
# P680 originally built an ad-hoc-signed .dmg (no Apple Developer Program
# membership at the time). Since then a paid membership was obtained and the
# MX68K target's Release configuration now signs with a "Developer ID
# Application" certificate (see project.pbxproj: CODE_SIGN_IDENTITY[sdk=
# macosx*] / DEVELOPMENT_TEAM[sdk=macosx*]). This script now:
#   1. verifies the built .app carries a Developer ID Application signature
#      (never signs anything itself — signing happens via Xcode/xcodebuild
#      using the project's own settings);
#   2. builds the .dmg;
#   3. submits it to Apple's notary service and waits for the result
#      (`xcrun notarytool submit --wait`);
#   4. staples the notarization ticket to the .dmg (`xcrun stapler staple`)
#      so it also verifies offline.
#
# Consequence: a downloaded, notarized .dmg opens normally (no more
# right-click -> Open workaround) — see the public README update that
# accompanies this change.
#
# NOTARIZATION CREDENTIALS:
#   Uses a keychain profile created once via:
#     xcrun notarytool store-credentials "mx68k-notary" \
#       --apple-id <Apple ID> --team-id SHRK77GS48
#   (interactive; prompts for an app-specific password from
#   appleid.apple.com — never pass the password on the command line).
#   Override the profile name with --keychain-profile if a different one
#   was used.
#
# CONTRACT:
#   Exit 0 = .dmg created, notarized, and stapled.
#   Exit 1 = build failure.
#   Exit 2 = setup error (missing/wrong signature, missing files, etc).
#   Exit 3 = notarization or stapling failure (the .dmg was built but is
#            not a valid, notarized release artifact — do not publish it).
#
#   `-e` is intentionally NOT set (same convention as smoke_test.sh): failures
#   are checked explicitly so cleanup and diagnostics always run.
#
# OUTPUT:
#   build/dist/MX68K-<CFBundleShortVersionString>.dmg
#   (`build/` is already covered by .gitignore — no new ignore rule needed.)
# ---------------------------------------------------------------------------
set -u

DO_BUILD=1
DO_NOTARIZE=1
KEYCHAIN_PROFILE="mx68k-notary"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-notarize) DO_NOTARIZE=0 ;;
        --keychain-profile)
            shift
            KEYCHAIN_PROFILE="${1:-}"
            ;;
        *) echo "unknown arg: $1" >&2
           echo "usage: $0 [--no-build] [--no-notarize] [--keychain-profile NAME]" >&2
           exit 2 ;;
    esac
    shift
done

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XCODEPROJ="$PROJ/MX68K/MX68K.xcodeproj"
DISTDIR="$PROJ/build/dist"
STAGING="$DISTDIR/staging"
BUILDLOG="/tmp/mx68k_release_build.log"

# --- 1. Release build -----------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    echo "==> Release build (log: $BUILDLOG)"
    xcodebuild -project "$XCODEPROJ" -scheme MX68K \
               -configuration Release -destination 'platform=macOS' \
               > "$BUILDLOG" 2>&1
    if [ $? -ne 0 ] || ! grep -q "BUILD SUCCEEDED" "$BUILDLOG"; then
        echo "## MX68K release dmg: FAIL (BUILD)"
        echo "- build log: $BUILDLOG"
        grep -E "error:|BUILD FAILED" "$BUILDLOG" 2>/dev/null | head -20
        exit 1
    fi
    echo "    BUILD SUCCEEDED"
else
    echo "==> --no-build: reusing the existing Release build product"
fi

# --- 2. Resolve the Release build product for THIS project/scheme ---------
# Same pattern as smoke_test.sh §4 (P448): -showBuildSettings ties the path to
# the exact project/scheme/configuration, immune to orphaned DerivedData dirs
# left behind by removed worktrees (mtime-sorting "newest .app" recurred 4+
# times as a stale-binary bug — see feedback_smoke_test_stale_binary_bug.md).
BUILT_PRODUCTS_DIR=$(xcodebuild -project "$XCODEPROJ" -scheme MX68K \
                      -configuration Release -destination 'platform=macOS' \
                      -showBuildSettings 2>/dev/null \
                      | awk -F'= ' '/ BUILT_PRODUCTS_DIR /{print $2; exit}')
APP="$BUILT_PRODUCTS_DIR/MX68K.app"
if [ -z "$BUILT_PRODUCTS_DIR" ] || [ ! -x "$APP/Contents/MacOS/MX68K" ]; then
    echo "## MX68K release dmg: SETUP ERROR (no runnable Release MX68K.app)"
    echo "- BUILT_PRODUCTS_DIR: ${BUILT_PRODUCTS_DIR:-<unresolved>}"
    echo "- expected: $APP/Contents/MacOS/MX68K"
    [ "$DO_BUILD" -eq 0 ] && echo "- hint: --no-build was given; run without it to produce the Release build first"
    exit 2
fi
echo "==> app: $APP"

# --- 3. Verify the Developer ID Application signature (verification only,
#        never signs anything itself — signing happens via the project's own
#        Release build settings) -------------------------------------------
CODESIGN_OUT=$(codesign -dv --verbose=4 "$APP" 2>&1)
if echo "$CODESIGN_OUT" | grep -q "Authority=Developer ID Application"; then
    echo "==> codesign: Developer ID Application signature confirmed"
    echo "$CODESIGN_OUT" | grep "^Authority=" | sed 's/^/    /'
elif echo "$CODESIGN_OUT" | grep -q "Signature=adhoc"; then
    echo "## MX68K release dmg: SETUP ERROR (app is ad-hoc signed, expected Developer ID Application)"
    echo "--- codesign -dv --verbose=4 output ---"
    echo "$CODESIGN_OUT"
    echo "- hint: check project.pbxproj Release config: CODE_SIGN_IDENTITY[sdk=macosx*] / DEVELOPMENT_TEAM[sdk=macosx*]"
    exit 2
else
    echo "## MX68K release dmg: SETUP ERROR (unexpected/no signature)"
    echo "--- codesign -dv --verbose=4 output ---"
    echo "$CODESIGN_OUT"
    exit 2
fi

# --- 4. Version, read from the BUILT bundle -------------------------------
# Deliberately NOT the source tree's Info.plist: two Info.plist files are
# tracked (MX68K/Resources/ and MX68K/MX68K/Resources/ — the known nested
# duplicate flagged in .gitignore), and INFOPLIST_FILE is relative to the
# .xcodeproj's directory, so picking the wrong one is easy. The built bundle
# is unambiguous: it is whatever actually shipped.
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
          "$APP/Contents/Info.plist" 2>/dev/null)
if [ -z "$VERSION" ]; then
    echo "## MX68K release dmg: SETUP ERROR (CFBundleShortVersionString unreadable)"
    echo "- plist: $APP/Contents/Info.plist"
    exit 2
fi
DMG="$DISTDIR/MX68K-$VERSION.dmg"
echo "==> version: $VERSION -> $DMG"

# --- 5. Staging directory (drag-and-drop installer layout with custom
#        Finder background, P849) ------------------------------------------
rm -rf "$STAGING"
mkdir -p "$STAGING/.background" || { echo "## MX68K release dmg: SETUP ERROR (cannot create $STAGING)"; exit 2; }
cp -R "$APP" "$STAGING/" || { echo "## MX68K release dmg: SETUP ERROR (app copy failed)"; rm -rf "$STAGING"; exit 2; }
ln -s /Applications "$STAGING/Applications" || { echo "## MX68K release dmg: SETUP ERROR (Applications symlink failed)"; rm -rf "$STAGING"; exit 2; }
cp "$PROJ/Scripts/dmg_background.png" "$STAGING/.background/dmg_background.png" \
    || { echo "## MX68K release dmg: SETUP ERROR (background image copy failed)"; rm -rf "$STAGING"; exit 2; }

# --- 6. Build a read-write dmg first so the Finder window layout (background
#        image, icon positions, window size) can be customized via
#        AppleScript, then convert to the final compressed read-only image
#        (P849; previously a single `hdiutil create -format UDZO` step
#        produced a plain, uncustomized Finder window). ---------------------
RW_DMG="$DISTDIR/.MX68K-$VERSION-rw.dmg"
rm -f "$RW_DMG"
echo "==> hdiutil create (UDRW, temporary — for Finder layout customization)"
hdiutil create -volname "MX68K" -srcfolder "$STAGING" -ov -fs HFS+ -format UDRW "$RW_DMG"
if [ $? -ne 0 ]; then
    echo "## MX68K release dmg: FAIL (hdiutil create rw failed)"
    rm -rf "$STAGING"
    rm -f "$RW_DMG"
    exit 1
fi

echo "==> mount rw dmg and customize Finder window layout"
MOUNT_OUT=$(hdiutil attach -readwrite -noverify -noautoopen "$RW_DMG" 2>&1)
MOUNT_POINT=$(echo "$MOUNT_OUT" | grep -oE "/Volumes/[^ ]*" | tail -1)
if [ -z "$MOUNT_POINT" ]; then
    echo "## MX68K release dmg: FAIL (could not mount rw dmg for layout customization)"
    echo "$MOUNT_OUT"
    rm -rf "$STAGING"
    rm -f "$RW_DMG"
    exit 1
fi

# ★Code Review 1周目REVISE対応: osascriptのFinder自動化がTCC許可ダイアログで
# ブロックしてハングした場合でも、マウント済みボリュームが残り続けて次回実行の
# 冪等性を壊さないよう、trapで確実にdetachする(どの経路で終了してもtrapが効く)。
trap 'hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1' EXIT

# osascript自体も有限時間でラップする(TCC許可ダイアログがブロックして返って
# 来ないケースでも、Build & Testエージェントが無期限停止しないようにする)。
# ★Code Review 2周目REVISE対応: GNU coreutilsの`timeout`コマンドはstock
# macOSに存在しない(このプロジェクトの実行環境で実機確認済み、brew版
# coreutilsも未インストール)ため、sleep+kill+waitによるウォッチドッグ
# パターンへ置き換える(bashビルトインのみ、新規外部コマンド不要)。
SCPT_FILE="$DISTDIR/.mx68k_dmg_layout.applescript"
cat > "$SCPT_FILE" <<'APPLESCRIPT'
tell application "Finder"
    tell disk "MX68K"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {400, 100, 1060, 500}
        set theViewOptions to the icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to 128
        set background picture of theViewOptions to file ".background:dmg_background.png"
        set position of item "MX68K.app" of container window to {165, 165}
        set position of item "Applications" of container window to {495, 165}
        close
        open
        update without registering applications
        delay 2
    end tell
end tell
APPLESCRIPT
osascript "$SCPT_FILE" &
OSASCRIPT_PID=$!
( sleep 90; kill -9 "$OSASCRIPT_PID" 2>/dev/null ) &
WATCHDOG_PID=$!
wait "$OSASCRIPT_PID" 2>/dev/null
OSASCRIPT_RC=$?
kill "$WATCHDOG_PID" 2>/dev/null
wait "$WATCHDOG_PID" 2>/dev/null
rm -f "$SCPT_FILE"

sync
hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1
sleep 1
trap - EXIT

if [ "$OSASCRIPT_RC" -ne 0 ]; then
    if [ "$OSASCRIPT_RC" -ge 128 ]; then
        echo "## MX68K release dmg: FAIL (osascript was killed by the 90s watchdog, rc=$OSASCRIPT_RC — likely blocked on a TCC/Automation permission dialog; grant Finder automation access once interactively and retry)"
    else
        echo "## MX68K release dmg: FAIL (osascript layout customization failed, rc=$OSASCRIPT_RC)"
    fi
    rm -rf "$STAGING"
    rm -f "$RW_DMG"
    exit 1
fi

echo "==> hdiutil convert (UDZO, final)"
rm -f "$DMG"
hdiutil convert "$RW_DMG" -format UDZO -o "$DMG"
HDIUTIL_RC=$?

# --- 7. Clean up staging ---------------------------------------------------
rm -rf "$STAGING"
rm -f "$RW_DMG"

if [ "$HDIUTIL_RC" -ne 0 ] || [ ! -f "$DMG" ]; then
    echo "## MX68K release dmg: FAIL (hdiutil convert rc=$HDIUTIL_RC)"
    exit 1
fi

# --- 8. Notarize + staple ---------------------------------------------------
if [ "$DO_NOTARIZE" -eq 1 ]; then
    echo "==> notarytool submit --wait (keychain profile: $KEYCHAIN_PROFILE; this can take a few minutes)"
    SUBMIT_OUT=$(xcrun notarytool submit "$DMG" --keychain-profile "$KEYCHAIN_PROFILE" --wait 2>&1)
    SUBMIT_RC=$?
    echo "$SUBMIT_OUT"
    if [ "$SUBMIT_RC" -ne 0 ] || ! echo "$SUBMIT_OUT" | grep -q "status: Accepted"; then
        echo "## MX68K release dmg: FAIL (notarization not accepted)"
        SUBMIT_ID=$(echo "$SUBMIT_OUT" | awk -F': ' '/^[[:space:]]*id:/{print $2; exit}')
        if [ -n "$SUBMIT_ID" ]; then
            echo "--- notarytool log ($SUBMIT_ID) ---"
            xcrun notarytool log "$SUBMIT_ID" --keychain-profile "$KEYCHAIN_PROFILE" 2>&1
        fi
        echo "- dmg (unnotarized, do NOT publish): $DMG"
        exit 3
    fi
    echo "==> notarytool: Accepted"

    echo "==> stapler staple"
    STAPLE_OUT=$(xcrun stapler staple "$DMG" 2>&1)
    STAPLE_RC=$?
    echo "$STAPLE_OUT"
    if [ "$STAPLE_RC" -ne 0 ]; then
        echo "## MX68K release dmg: FAIL (stapler staple rc=$STAPLE_RC)"
        echo "- dmg (notarized but not stapled, do NOT publish): $DMG"
        exit 3
    fi

    # ★spctl検証は dmg ファイル自体ではなく、中の .app に対して行う。
    #   `spctl -a -t open` を dmg 自体へ直接かけると、dmg コンテナ自体には
    #   コード署名が無い(署名されるのは中の .app だけ、これは通常の dmg
    #   配布として正しい)ため "rejected: source=no usable signature" に
    #   なる——これは dmg 自体が quarantine 属性を持たないローカル生成物
    #   であることに起因する spctl 側の限界であり、実際のユーザー体験
    #   (ダウンロード→マウント→.app起動)を代表しない。実際にGatekeeperが
    #   評価する対象は展開後の .app であり、これが `source=Notarized
    #   Developer ID` として accepted になることが実質的な合格基準。
    echo "==> mount dmg and spctl verify the .app inside (offline, post-staple)"
    MOUNT_OUT=$(hdiutil attach "$DMG" -nobrowse -readonly 2>&1)
    MOUNT_POINT=$(echo "$MOUNT_OUT" | grep -oE "/Volumes/[^ ]*" | tail -1)
    if [ -z "$MOUNT_POINT" ] || [ ! -d "$MOUNT_POINT/MX68K.app" ]; then
        echo "## MX68K release dmg: FAIL (could not mount dmg or find MX68K.app inside)"
        echo "$MOUNT_OUT"
        exit 3
    fi
    SPCTL_OUT=$(spctl -a -vvv -t execute "$MOUNT_POINT/MX68K.app" 2>&1)
    echo "$SPCTL_OUT"
    hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1
    if ! echo "$SPCTL_OUT" | grep -q "accepted"; then
        echo "## MX68K release dmg: FAIL (spctl did not accept the app inside the dmg)"
        exit 3
    fi
    SIGNING_LINE="Developer ID Application (notarized, stapled)"
else
    echo "==> --no-notarize: skipping notarytool submit / stapler (dmg is signed but NOT notarized — do not publish)"
    SIGNING_LINE="Developer ID Application (NOT notarized — --no-notarize was given)"
fi

echo
echo "## MX68K release dmg: OK"
echo "- dmg:     $DMG"
echo "- size:    $(du -h "$DMG" | awk '{print $1}') ($(stat -f%z "$DMG") bytes)"
echo "- version: $VERSION"
echo "- signing: $SIGNING_LINE"
exit 0
