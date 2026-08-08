#!/bin/bash
# Script to apply ka9q-radio security patches
# Usage: ./apply-patches.sh [path-to-ka9q-radio]

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get target directory
if [ -z "$1" ]; then
    echo -e "${YELLOW}Usage: $0 <path-to-ka9q-radio>${NC}"
    echo "Example: $0 /Users/anilsson/Documents/GitHub/ka9q-radio"
    exit 1
fi

TARGET_DIR="$1"
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"

# Verify target directory
if [ ! -d "$TARGET_DIR" ]; then
    echo -e "${RED}Error: Directory $TARGET_DIR does not exist${NC}"
    exit 1
fi

if [ ! -f "$TARGET_DIR/src/radio.c" ]; then
    echo -e "${RED}Error: $TARGET_DIR does not appear to be ka9q-radio source${NC}"
    exit 1
fi

echo -e "${GREEN}KA9Q-Radio Security Patch Installer${NC}"
echo "Target: $TARGET_DIR"
echo "Patches: $PATCH_DIR"
echo ""

# Create backup
BACKUP_DIR="$TARGET_DIR-backup-$(date +%Y%m%d-%H%M%S)"
echo -e "${YELLOW}Creating backup: $BACKUP_DIR${NC}"
cp -r "$TARGET_DIR" "$BACKUP_DIR"
echo -e "${GREEN}✓ Backup created${NC}"
echo ""

# Check if git is available and if we're in a git repo
cd "$TARGET_DIR"
if git rev-parse --git-dir > /dev/null 2>&1; then
    USE_GIT=1
    echo -e "${GREEN}Git repository detected, will use 'git apply'${NC}"

    # Check for uncommitted changes
    if ! git diff-index --quiet HEAD --; then
        echo -e "${YELLOW}Warning: You have uncommitted changes${NC}"
        read -p "Continue anyway? (y/n) " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            echo "Aborted"
            exit 1
        fi
    fi
else
    USE_GIT=0
    echo -e "${YELLOW}Not a git repository, will use 'patch' command${NC}"
fi
echo ""

# Apply patches
PATCHES=(
    "0001-fix-lmalloc-assert-to-proper-error-handling.patch"
    "0002-fix-shallow-copy-double-free-in-channel-init.patch"
    "0003-fix-filter-null-checks-for-allocations.patch"
    "0004-fix-multicast-formatsock-null-check.patch"
    "0005-fix-hid-libusb-new-device-null-check.patch"
    "0006-fix-mirror-alloc-fd-leak-on-error.patch"
    "0007-fix-radio-notches-allocation-error-handling.patch"
    "0008-fix-monitor-strdup-null-checks.patch"
    "0009-fix-modulo-signed-integer-overflow.patch"
    "0010-fix-type-punning-strict-aliasing-violations.patch"
)

APPLIED=0
FAILED=0

for patch in "${PATCHES[@]}"; do
    PATCH_FILE="$PATCH_DIR/$patch"

    if [ ! -f "$PATCH_FILE" ]; then
        echo -e "${RED}✗ Patch not found: $patch${NC}"
        FAILED=$((FAILED + 1))
        continue
    fi

    echo -n "Applying $patch ... "

    if [ $USE_GIT -eq 1 ]; then
        if git apply --check "$PATCH_FILE" 2>/dev/null; then
            git apply "$PATCH_FILE"
            echo -e "${GREEN}✓${NC}"
            APPLIED=$((APPLIED + 1))
        else
            echo -e "${RED}✗ (failed)${NC}"
            FAILED=$((FAILED + 1))
            echo "  Run 'git apply --reject $PATCH_FILE' to see conflicts"
        fi
    else
        if patch --dry-run -p1 < "$PATCH_FILE" > /dev/null 2>&1; then
            patch -p1 < "$PATCH_FILE" > /dev/null
            echo -e "${GREEN}✓${NC}"
            APPLIED=$((APPLIED + 1))
        else
            echo -e "${RED}✗ (failed)${NC}"
            FAILED=$((FAILED + 1))
            echo "  Run 'patch -p1 < $PATCH_FILE' manually to see errors"
        fi
    fi
done

echo ""
echo "================================"
echo -e "Patches applied: ${GREEN}$APPLIED${NC}"
echo -e "Patches failed:  ${RED}$FAILED${NC}"
echo "================================"
echo ""

if [ $FAILED -gt 0 ]; then
    echo -e "${RED}Some patches failed to apply!${NC}"
    echo "Your backup is at: $BACKUP_DIR"
    echo "You can restore with: rm -rf $TARGET_DIR && mv $BACKUP_DIR $TARGET_DIR"
    exit 1
fi

# Test compilation
echo -e "${YELLOW}Testing compilation...${NC}"
if make -C "$TARGET_DIR" clean > /dev/null 2>&1 && make -C "$TARGET_DIR" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    echo "Check the build manually"
    exit 1
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}All patches applied successfully!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "Backup location: $BACKUP_DIR"
echo ""
echo "Next steps:"
echo "1. Test the patched version thoroughly"
echo "2. Run with sanitizers: CFLAGS='-fsanitize=address' make"
echo "3. If everything works, delete backup: rm -rf $BACKUP_DIR"
echo ""
echo "To rollback: rm -rf $TARGET_DIR && mv $BACKUP_DIR $TARGET_DIR"
