#!/bin/bash
# Scripts/make_release_dmg.sh  [--no-build]
# ---------------------------------------------------------------------------
# P680: Build a distributable .dmg of MX68K (unsigned / ad-hoc distribution).
#
# WHY THIS EXISTS:
#   Public releases on github.com/HackCat0916/MX68K ship a prebuilt .app. There
#   is no Apple Developer Program membership, so there is no Developer ID
#   signature and no notarization — Xcode's Release build is nevertheless
#   ad-hoc signed automatically (codesign: Signature=adhoc, TeamIdentifier=not
#   set), which is what makes it runnable on Apple Silicon at all. This script
#   only VERIFIES that ad-hoc signature; it never signs anything itself.
#
#   Consequence, by design and already agreed: a downloaded .dmg carries the
#   quarantine attribute, so `spctl -a --type execute` reports "rejected" and
#   Gatekeeper shows a warning. Users open it via right-click -> Open (this is
#   documented in the public README, not solvable here).
#
# CONTRACT:
#   Exit 0 = .dmg created.   Exit 1 = build failure.   Exit 2 = setup error.
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
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        *) echo "unknown arg: $1" >&2; echo "usage: $0 [--no-build]" >&2; exit 2 ;;
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

# --- 3. Verify the ad-hoc signature (verification only, never signs) ------
if codesign -dv "$APP" 2>&1 | grep -q "Signature=adhoc"; then
    echo "==> codesign: Signature=adhoc (expected — no Developer ID / notarization)"
else
    echo "## MX68K release dmg: SETUP ERROR (app is not ad-hoc signed)"
    echo "--- codesign -dv output ---"
    codesign -dv "$APP" 2>&1
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

# --- 5. Staging directory (drag-and-drop installer layout) ----------------
rm -rf "$STAGING"
mkdir -p "$STAGING" || { echo "## MX68K release dmg: SETUP ERROR (cannot create $STAGING)"; exit 2; }
cp -R "$APP" "$STAGING/" || { echo "## MX68K release dmg: SETUP ERROR (app copy failed)"; rm -rf "$STAGING"; exit 2; }
ln -s /Applications "$STAGING/Applications" || { echo "## MX68K release dmg: SETUP ERROR (Applications symlink failed)"; rm -rf "$STAGING"; exit 2; }

# --- 6. Create the compressed disk image ----------------------------------
echo "==> hdiutil create (UDZO)"
hdiutil create -volname "MX68K" -srcfolder "$STAGING" -ov -format UDZO "$DMG"
HDIUTIL_RC=$?

# --- 7. Clean up staging, report ------------------------------------------
rm -rf "$STAGING"

if [ "$HDIUTIL_RC" -ne 0 ] || [ ! -f "$DMG" ]; then
    echo "## MX68K release dmg: FAIL (hdiutil rc=$HDIUTIL_RC)"
    exit 1
fi

echo
echo "## MX68K release dmg: OK"
echo "- dmg:     $DMG"
echo "- size:    $(du -h "$DMG" | awk '{print $1}') ($(stat -f%z "$DMG") bytes)"
echo "- version: $VERSION"
echo "- signing: ad-hoc only (Gatekeeper warns after download — right-click > Open)"
exit 0
