#!/usr/bin/env bash
# tag-release.sh — Update version numbers across code/docs and create a git tag.
#
# Usage:
#   ./scripts/tag-release.sh 0.4.0
#   ./scripts/tag-release.sh 0.4.0 --dry-run
#
# What it does:
#   1. Validates the version string (semver)
#   2. Updates the single source of truth in CMakeLists.txt
#   3. Updates @version in all public header (.h) and source (.c) Doxygen comments
#   4. Updates version strings in python/csilk/_version.py
#   5. Updates version strings in cmake/ports/csilk/vcpkg.json
#   6. Updates > **Version**: X.Y.Z and > **版本**: X.Y.Z in all docs
#   7. Updates Version**: vX.Y.Z+ / 版本**: vX.Y.Z+ bold header patterns
#   8. Updates standalone | Version: X.Y.Z in doc metadata
#   9. Updates CMakeLists.txt version/版本 in architecture ASCII diagrams
#  10. Updates hardcoded "X.Y.Z" strings in doc files
#  11. Updates version: X.Y.Z in code-block examples
#  12. Updates CHANGELOG [Unreleased] → [X.Y.Z] with today's date
#  13. Commits all changes
#  14. Creates a signed git tag vX.Y.Z
#
# The single source of truth is CMakeLists.txt (CSILK_VERSION_MAJOR/MINOR/PATCH).
# Everything else is derived from it.

set -euo pipefail

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}ℹ ${NC}$*"; }
ok()    { echo -e "${GREEN}✔ ${NC}$*"; }
warn()  { echo -e "${YELLOW}⚠ ${NC}$*"; }
err()   { echo -e "${RED}✖ ${NC}$*" >&2; }
die()   { err "$@"; exit 1; }

# ── Parse arguments ─────────────────────────────────────────────────────────
DRY_RUN=false
VERSION=""

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        -h|--help)
            echo "Usage: $0 <version> [--dry-run]"
            echo ""
            echo "Examples:"
            echo "  $0 0.4.0          # Update versions and create tag v0.4.0"
            echo "  $0 0.4.0 --dry-run # Show what would change without committing"
            exit 0
            ;;
        *)
            if [[ -z "$VERSION" ]]; then
                VERSION="$arg"
            else
                die "Unexpected argument: $arg"
            fi
            ;;
    esac
done

[[ -z "$VERSION" ]] && die "Usage: $0 <version> [--dry-run]"

# ── Validate version (semver) ───────────────────────────────────────────────
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    die "Invalid version format: '$VERSION' (expected MAJOR.MINOR.PATCH, e.g. 0.4.0)"
fi

IFS='.' read -r V_MAJOR V_MINOR V_PATCH <<< "$VERSION"
TAG="v${VERSION}"

# ── Resolve project root ────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ── Pre-flight checks ───────────────────────────────────────────────────────
info "Project root: $PROJECT_ROOT"
info "Target version: ${VERSION}"
info "Target tag: ${TAG}"

# Ensure clean working tree
if [[ -n "$(git status --porcelain)" ]]; then
    die "Working tree is not clean. Commit or stash changes first."
fi

# Ensure tag doesn't already exist
if git rev-parse "$TAG" >/dev/null 2>&1; then
    die "Tag '$TAG' already exists."
fi

# Ensure we're on a branch
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
info "Current branch: ${BRANCH}"

echo ""

# ── Helper: run or print ────────────────────────────────────────────────────
run_or_print() {
    if $DRY_RUN; then
        echo -e "  ${YELLOW}[dry-run]${NC} $*"
    else
        eval "$@"
    fi
}

# ── Step 1: Update CMakeLists.txt (single source of truth) ──────────────────
info "Updating CMakeLists.txt ..."

OLD_MAJOR=$(grep -oP 'set\(CSILK_VERSION_MAJOR \K[0-9]+' CMakeLists.txt)
OLD_MINOR=$(grep -oP 'set\(CSILK_VERSION_MINOR \K[0-9]+' CMakeLists.txt)
OLD_PATCH=$(grep -oP 'set\(CSILK_VERSION_PATCH \K[0-9]+' CMakeLists.txt)
OLD_VERSION="${OLD_MAJOR}.${OLD_MINOR}.${OLD_PATCH}"

if [[ "$OLD_VERSION" == "$VERSION" ]]; then
    die "Version ${VERSION} is the same as the current version (${OLD_VERSION}). Nothing to do."
fi

info "  ${OLD_VERSION} → ${VERSION}"

run_or_print "sed -i 's/set(CSILK_VERSION_MAJOR ${OLD_MAJOR}/set(CSILK_VERSION_MAJOR ${V_MAJOR}/' CMakeLists.txt"
run_or_print "sed -i 's/set(CSILK_VERSION_MINOR ${OLD_MINOR}/set(CSILK_VERSION_MINOR ${V_MINOR}/' CMakeLists.txt"
run_or_print "sed -i 's/set(CSILK_VERSION_PATCH ${OLD_PATCH}/set(CSILK_VERSION_PATCH ${V_PATCH}/' CMakeLists.txt"

ok "CMakeLists.txt updated"

# ── Step 2: Update @version in header and source files ──────────────────────
info "Updating @version in header and source files ..."

HEADER_COUNT=0
while IFS= read -r file; do
    run_or_print "sed -i 's/@version ${OLD_VERSION}/@version ${VERSION}/g' '$file'"
    HEADER_COUNT=$((HEADER_COUNT + 1))
done < <(grep -rl "@version ${OLD_VERSION}" --include="*.h" --include="*.c" include/ src/)

ok "  Updated ${HEADER_COUNT} header/source files"

# ── Step 2b: Update python/csilk/_version.py ────────────────────────────────
PY_VERSION_FILE="python/csilk/_version.py"
if [[ -f "$PY_VERSION_FILE" ]]; then
    info "Updating $PY_VERSION_FILE ..."
    run_or_print "sed -i 's/__version__ = \"${OLD_VERSION}\"/__version__ = \"${VERSION}\"/' '$PY_VERSION_FILE'"
    ok "  $PY_VERSION_FILE updated"
fi

# ── Step 2c: Update cmake/ports/csilk/vcpkg.json ────────────────────────────
VCPKG_FILE="cmake/ports/csilk/vcpkg.json"
if [[ -f "$VCPKG_FILE" ]]; then
    info "Updating $VCPKG_FILE ..."
    run_or_print "sed -i 's/\"version-semver\": \"${OLD_VERSION}\"/\"version-semver\": \"${VERSION}\"/' '$VCPKG_FILE'"
    ok "  $VCPKG_FILE updated"
fi

# ── Step 3: Update version in documentation ──────────────────────────────────
info "Updating version in documentation ..."

DOC_COUNT=0

# Pattern 1: > **Version**: X.Y.Z and > **版本**: X.Y.Z
while IFS= read -r file; do
    run_or_print "sed -i 's/Version\*\*: ${OLD_VERSION}/Version**: ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: ${OLD_VERSION}/版本**: ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "Version\*\*: ${OLD_VERSION}\|版本\*\*: ${OLD_VERSION}" --include="*.md" docs/ README.md README.zh-CN.md 2>/dev/null || true)

# Pattern 2: Hardcoded version strings in docs (e.g., cJSON_AddStringToObject)
while IFS= read -r file; do
    run_or_print "sed -i 's/\"${OLD_VERSION}\"/\"${VERSION}\"/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\"${OLD_VERSION}\"" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 3: Version**: vX.Y.Z+ and 版本**: vX.Y.Z+ (bold markers only)
# Matches `> **Version**: v0.3.0+` headers but NOT `Implemented (v0.3.0+)`.
while IFS= read -r file; do
    run_or_print "sed -i 's/Version\*\*: v${OLD_VERSION}+/Version**: v${VERSION}+/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: v${OLD_VERSION}+/版本**: v${VERSION}+/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "Version\*\*: v${OLD_VERSION}+\|版本\*\*: v${OLD_VERSION}+" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 4: "| Version: X.Y.Z" in doc metadata lines (design docs)
# Catches `> Date: ... | Version: X.Y.Z | ...`
while IFS= read -r file; do
    run_or_print "sed -i 's/| Version: ${OLD_VERSION}/| Version: ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "| Version: ${OLD_VERSION}" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 5: "CMakeLists.txt ... version/版本 X.Y.Z" in ASCII diagrams
# Matches lines like `└── CMakeLists.txt  # C23, version 0.3.0` in architecture.md
while IFS= read -r file; do
    run_or_print "sed -i 's/version ${OLD_VERSION}/version ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本 ${OLD_VERSION}/版本 ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "CMakeLists\.txt.*\(version\|版本\) ${OLD_VERSION}" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 6: "version: X.Y.Z" in code-block examples (e.g., benchmarks/README.md)
while IFS= read -r file; do
    run_or_print "sed -i 's/version: ${OLD_VERSION}/version: ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "version: ${OLD_VERSION}" --include="*.md" docs/ benchmarks/ 2>/dev/null || true)

ok "  Updated ${DOC_COUNT} documentation files"

# ── Step 4: Update CHANGELOG ────────────────────────────────────────────────
info "Updating CHANGELOG ..."

TODAY=$(date +%Y-%m-%d)

for changelog in CHANGELOG.md CHANGELOG.zh-CN.md; do
    if [[ -f "$changelog" ]]; then
        if ! $DRY_RUN; then
            # Replace [Unreleased] header with version + date, add new [Unreleased]
            sed -i "s/^## \[Unreleased\]/## [Unreleased]\n\n## [${VERSION}] - ${TODAY}/" "$changelog"
        fi
        ok "  ${changelog} updated (Unreleased → ${VERSION})"
    fi
done

# ── Step 5: Commit ──────────────────────────────────────────────────────────
info "Committing changes ..."

if ! $DRY_RUN; then
    git add -A
    git commit -m "chore(release): 🚀 v${VERSION}"
    ok "Committed: chore(release): 🚀 v${VERSION}"
else
    warn "[dry-run] Would commit all changes with message: chore(release): 🚀 v${VERSION}"
fi

# ── Step 6: Create tag ──────────────────────────────────────────────────────
info "Creating tag ${TAG} ..."

if ! $DRY_RUN; then
    git tag -a "$TAG" -m "Release ${TAG}"
    ok "Tag ${TAG} created"
else
    warn "[dry-run] Would create tag: ${TAG}"
fi

# ── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Release ${VERSION} prepared!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════${NC}"
echo ""
echo "  Files updated:"
echo "    • CMakeLists.txt (source of truth)"
echo "    • ${HEADER_COUNT} header/source files (@version)"
echo "    • python/csilk/_version.py"
echo "    • cmake/ports/csilk/vcpkg.json"
echo "    • ${DOC_COUNT} documentation files"
echo "    • CHANGELOG.md + CHANGELOG.zh-CN.md"
echo ""
if ! $DRY_RUN; then
    echo "  Next steps:"
    echo "    • git push origin ${BRANCH} && git push origin ${TAG}"
    echo "    • Create GitHub Release from tag ${TAG}"
else
    echo "  This was a dry run. Re-run without --dry-run to apply."
fi
echo ""
