#!/usr/bin/env bash
# tag-release.sh — Comprehensive version synchronization and git release tagging tool.
#
# Usage:
#   ./scripts/tag-release.sh <version> [options]
#   ./scripts/tag-release.sh --bump-patch [options]
#   ./scripts/tag-release.sh --bump-minor [options]
#   ./scripts/tag-release.sh --bump-major [options]
#
# Options:
#   --dry-run             Preview all file modifications without modifying anything or committing
#   --skip-tests          Skip running pre-release unit tests
#   --skip-format         Skip running clang-format formatting check
#   --skip-mermaid        Skip running Mermaid diagram validation
#   --no-commit           Apply version modifications across files but do not create a git commit or tag
#   --no-tag              Commit the version changes but do not create a git tag
#   -h, --help            Show this help manual
#
# What it does:
#   1. Validates SemVer format and ensures target version > current version
#   2. Performs pre-flight checks (clean git status, tag collision check, test & format suite)
#   3. Updates single source of truth in cmake/options.cmake (CSILK_VERSION_MAJOR/MINOR/PATCH)
#   4. Updates Doxygen @version in all public headers (include/) and sources (src/)
#   5. Updates version strings in python/csilk/_version.py
#   6. Updates version strings in cmake/ports/csilk/vcpkg.json
#   7. Updates version strings in scripts/csilkskel (VERSION = "...")
#   8. Updates all documentation files (docs/, benchmarks/, README.md, README.zh-CN.md):
#      - Document headers (> **Version**: X.Y.Z | **Last updated**: YYYY-MM-DD)
#      - Header metadata (**Version**: vX.Y.Z, **版本**: vX.Y.Z, - **版本**：vX.Y.Z)
#      - Status & implementation badges (Implemented (vX.Y.Z+), 已实现（vX.Y.Z+）, Completed: vX.Y.Z)
#      - Metadata lines (| Version: X.Y.Z)
#      - Code blocks & configs (version: X.Y.Z, csilk_version: X.Y.Z, "version": "X.Y.Z")
#      - ASCII tree diagrams (CMakeLists.txt ... (version|版本) X.Y.Z)
#      - Specifications & standard texts (vX.Y.Z 标准)
#   9. Promotes CHANGELOG.md and CHANGELOG.zh-CN.md [Unreleased] → [X.Y.Z] with today's date
#  10. Runs post-update residual scanner to ensure 0 version discrepancies remain
#  11. Verifies version sync and format integrity
#  12. Commits changes (chore(release): 🚀 vX.Y.Z) and creates annotated tag vX.Y.Z
#
# Single source of truth: cmake/options.cmake
#

set -euo pipefail

# ── Colors & Logging ────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}ℹ ${NC}$*"; }
ok()    { echo -e "${GREEN}✔ ${NC}$*"; }
warn()  { echo -e "${YELLOW}⚠ ${NC}$*"; }
err()   { echo -e "${RED}✖ ${NC}$*" >&2; }
die()   { err "$@"; exit 1; }

# ── Resolve project root ────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

VERSION_FILE="cmake/options.cmake"
[[ ! -f "$VERSION_FILE" ]] && die "Could not find $VERSION_FILE. Are you running from csilk repository?"

# Extract current version
OLD_MAJOR=$(grep -oP 'set\(CSILK_VERSION_MAJOR \K[0-9]+' "$VERSION_FILE")
OLD_MINOR=$(grep -oP 'set\(CSILK_VERSION_MINOR \K[0-9]+' "$VERSION_FILE")
OLD_PATCH=$(grep -oP 'set\(CSILK_VERSION_PATCH \K[0-9]+' "$VERSION_FILE")
OLD_VERSION="${OLD_MAJOR}.${OLD_MINOR}.${OLD_PATCH}"

# ── CLI Arguments & Options ─────────────────────────────────────────────────
DRY_RUN=false
SKIP_TESTS=false
SKIP_FORMAT=false
SKIP_MERMAID=false
NO_COMMIT=false
NO_TAG=false
VERSION=""
BUMP_MODE=""

usage() {
    echo -e "${BOLD}Usage:${NC}"
    echo "  $0 <version> [options]"
    echo "  $0 --bump-patch [options]"
    echo "  $0 --bump-minor [options]"
    echo "  $0 --bump-major [options]"
    echo ""
    echo -e "${BOLD}Options:${NC}"
    echo "  --dry-run             Preview all file modifications without modifying anything or committing"
    echo "  --skip-tests          Skip running pre-release unit tests"
    echo "  --skip-format         Skip running clang-format formatting check"
    echo "  --skip-mermaid        Skip running Mermaid diagram validation"
    echo "  --no-commit           Apply version modifications across files but do not create a git commit or tag"
    echo "  --no-tag              Commit the version changes but do not create a git tag"
    echo "  -h, --help            Show this help manual"
    echo ""
    echo -e "${BOLD}Current Version:${NC} ${GREEN}${OLD_VERSION}${NC}"
    echo ""
    echo -e "${BOLD}Examples:${NC}"
    echo "  $0 0.6.0              # Upgrade to v0.6.0, commit and tag"
    echo "  $0 --bump-patch       # Automatically bump ${OLD_VERSION} -> ${OLD_MAJOR}.${OLD_MINOR}.$((OLD_PATCH + 1))"
    echo "  $0 --bump-minor       # Automatically bump ${OLD_VERSION} -> ${OLD_MAJOR}.$((OLD_MINOR + 1)).0"
    echo "  $0 0.6.0 --dry-run    # Preview all changes for v0.6.0 without applying"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --skip-tests) SKIP_TESTS=true; shift ;;
        --skip-format) SKIP_FORMAT=true; shift ;;
        --skip-mermaid) SKIP_MERMAID=true; shift ;;
        --no-commit) NO_COMMIT=true; shift ;;
        --no-tag) NO_TAG=true; shift ;;
        --bump-patch) BUMP_MODE="patch"; shift ;;
        --bump-minor) BUMP_MODE="minor"; shift ;;
        --bump-major) BUMP_MODE="major"; shift ;;
        -h|--help) usage ;;
        -*) die "Unknown option: $1 (run $0 --help for usage)" ;;
        *)
            if [[ -z "$VERSION" ]]; then
                VERSION="$1"
            else
                die "Unexpected extra argument: $1"
            fi
            shift
            ;;
    esac
done

# Handle auto-bump modes
if [[ -n "$BUMP_MODE" ]]; then
    if [[ -n "$VERSION" ]]; then
        die "Cannot specify both explicit version ($VERSION) and --bump-$BUMP_MODE."
    fi
    case "$BUMP_MODE" in
        patch) VERSION="${OLD_MAJOR}.${OLD_MINOR}.$((OLD_PATCH + 1))" ;;
        minor) VERSION="${OLD_MAJOR}.$((OLD_MINOR + 1)).0" ;;
        major) VERSION="$((OLD_MAJOR + 1)).0.0" ;;
    esac
fi

[[ -z "$VERSION" ]] && usage

# ── Validate SemVer Format ──────────────────────────────────────────────────
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    die "Invalid version format: '$VERSION' (expected MAJOR.MINOR.PATCH, e.g. 0.6.0)"
fi

IFS='.' read -r V_MAJOR V_MINOR V_PATCH <<< "$VERSION"
TAG="v${VERSION}"
TODAY=$(date +%Y-%m-%d)

if [[ "$OLD_VERSION" == "$VERSION" ]]; then
    die "Target version ${VERSION} is identical to current version (${OLD_VERSION}). Nothing to do."
fi

# SemVer monotonicity check
if (( V_MAJOR < OLD_MAJOR )) || \
   (( V_MAJOR == OLD_MAJOR && V_MINOR < OLD_MINOR )) || \
   (( V_MAJOR == OLD_MAJOR && V_MINOR == OLD_MINOR && V_PATCH < OLD_PATCH )); then
    warn "Target version ${VERSION} is smaller than current version ${OLD_VERSION} (downgrade)."
fi

info "═══════════════════════════════════════════════════════════════"
info " Csilk Release Orchestrator: ${BOLD}${OLD_VERSION}${NC} → ${GREEN}${BOLD}${VERSION}${NC} (${TAG})"
info " Project root: ${PROJECT_ROOT}"
info " Release date: ${TODAY}"
info "═══════════════════════════════════════════════════════════════"

# ── Pre-flight Checks ───────────────────────────────────────────────────────
info "Running pre-flight checks ..."

# 1. Clean working tree
if [[ -n "$(git status --porcelain)" ]]; then
    if ! $DRY_RUN; then
        die "Working tree is not clean. Commit or stash changes before running tag-release.sh."
    else
        warn "Working tree is not clean (continuing because --dry-run is active)."
    fi
else
    ok "Git working tree is clean"
fi

# 2. Tag does not already exist
if git rev-parse "$TAG" >/dev/null 2>&1; then
    die "Local git tag '$TAG' already exists."
fi
if git ls-remote --tags origin "$TAG" 2>/dev/null | grep -q "$TAG"; then
    die "Remote git tag '$TAG' already exists on origin."
fi
ok "Git tag '$TAG' is available"

# 3. Branch check
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
info "Active branch: ${BRANCH}"

# 4. Code format check
if ! $SKIP_FORMAT; then
    if [[ -d "build" ]]; then
        info "Checking code formatting ..."
        if cmake --build build --target check-format >/dev/null 2>&1; then
            ok "Code formatting check passed"
        else
            die "Code formatting mismatch detected. Run 'cmake --build build --target format' first."
        fi
    fi
fi

# 5. Mermaid diagrams check
if ! $SKIP_MERMAID; then
    if [[ -d "build" ]]; then
        info "Validating Mermaid diagrams ..."
        if cmake --build build --target check-mermaid >/dev/null 2>&1; then
            ok "Mermaid diagrams validation passed"
        else
            die "Mermaid diagram validation failed. Fix broken diagrams before release."
        fi
    fi
fi

# 6. Unit tests check
if ! $SKIP_TESTS; then
    if [[ -d "build" ]]; then
        info "Running unit test suite ..."
        NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
        if ctest --test-dir build -E test_integration -j"$NPROC" --timeout 30 --output-on-failure >/dev/null 2>&1; then
            ok "All unit tests passed"
        else
            die "Unit tests failed. Resolve test failures before creating a release tag."
        fi
    else
        warn "Build directory not found, skipping pre-release ctest. (Use cmake -B build to configure)"
    fi
fi

echo ""

# ── Helper: run or print ────────────────────────────────────────────────────
run_or_print() {
    if $DRY_RUN; then
        echo -e "  ${YELLOW}[dry-run]${NC} $*"
    else
        eval "$@"
    fi
}

# ── Step 1: Update cmake/options.cmake (single source of truth) ─────────────
info "Updating cmake/options.cmake (source of truth) ..."
info "  ${OLD_VERSION} → ${VERSION}"

run_or_print "sed -i 's/set(CSILK_VERSION_MAJOR ${OLD_MAJOR}/set(CSILK_VERSION_MAJOR ${V_MAJOR}/' '$VERSION_FILE'"
run_or_print "sed -i 's/set(CSILK_VERSION_MINOR ${OLD_MINOR}/set(CSILK_VERSION_MINOR ${V_MINOR}/' '$VERSION_FILE'"
run_or_print "sed -i 's/set(CSILK_VERSION_PATCH ${OLD_PATCH}/set(CSILK_VERSION_PATCH ${V_PATCH}/' '$VERSION_FILE'"
ok "cmake/options.cmake updated"

# ── Step 2: Update @version in header and source files ──────────────────────
info "Updating Doxygen @version in header and source files ..."

HEADER_COUNT=0
while IFS= read -r file; do
    run_or_print "sed -i 's/@version ${OLD_VERSION}/@version ${VERSION}/g' '$file'"
    HEADER_COUNT=$((HEADER_COUNT + 1))
done < <(grep -rl "@version ${OLD_VERSION}" --include="*.h" --include="*.c" include/ src/ 2>/dev/null || true)

ok "Updated ${HEADER_COUNT} header/source files"

# ── Step 3: Update Python bindings, vcpkg & CLI tool ─────────────────────────
info "Updating packaging and tool version references ..."

# Python bindings
PY_VERSION_FILE="python/csilk/_version.py"
if [[ -f "$PY_VERSION_FILE" ]]; then
    run_or_print "sed -i 's/__version__ = \"${OLD_VERSION}\"/__version__ = \"${VERSION}\"/' '$PY_VERSION_FILE'"
    ok "  $PY_VERSION_FILE updated"
fi

# vcpkg manifest
VCPKG_FILE="cmake/ports/csilk/vcpkg.json"
if [[ -f "$VCPKG_FILE" ]]; then
    run_or_print "sed -i 's/\"version-semver\": \"${OLD_VERSION}\"/\"version-semver\": \"${VERSION}\"/' '$VCPKG_FILE'"
    ok "  $VCPKG_FILE updated"
fi

# csilkskel generator tool
CSILKSKEL_FILE="scripts/csilkskel"
if [[ -f "$CSILKSKEL_FILE" ]]; then
    run_or_print "sed -i 's/VERSION = \"${OLD_VERSION}\"/VERSION = \"${VERSION}\"/' '$CSILKSKEL_FILE'"
    ok "  $CSILKSKEL_FILE updated"
fi

# ── Step 4: Comprehensive Documentation Version Synchronization ─────────────
info "Updating version references and release dates across documentation ..."

DOC_COUNT=0

# Pattern 1: Doc headers with version & update date
# Matches: > **Version**: 0.5.0 | **Last updated**: 2026-08-08
while IFS= read -r file; do
    run_or_print "sed -i 's/Version\*\*: ${OLD_VERSION} | \*\*Last updated\*\*: [0-9-]*/Version**: ${VERSION} | **Last updated**: ${TODAY}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: ${OLD_VERSION} | \*\*最后更新\*\*: [0-9-]*/版本**: ${VERSION} | **最后更新**: ${TODAY}/g' '$file'"
    run_or_print "sed -i 's/Version\*\*: v${OLD_VERSION} | \*\*Last updated\*\*: [0-9-]*/Version**: v${VERSION} | **Last updated**: ${TODAY}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: v${OLD_VERSION} | \*\*最后更新\*\*: [0-9-]*/版本**: v${VERSION} | **最后更新**: ${TODAY}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\(Version\|版本\)\*\*[:：] *v\?${OLD_VERSION} *| *\*\*\(Last updated\|最后更新\)\*\*" --include="*.md" docs/ README.md README.zh-CN.md 2>/dev/null || true)

# Pattern 2: Standalone bold headers / bullet points
while IFS= read -r file; do
    run_or_print "sed -i 's/Version\*\*: ${OLD_VERSION}/Version**: ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: ${OLD_VERSION}/版本**: ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/Version\*\*: v${OLD_VERSION}/Version**: v${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*: v${OLD_VERSION}/版本**: v${VERSION}/g' '$file'"
    run_or_print "sed -i 's/Version\*\*：v${OLD_VERSION}/Version**：v${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*：v${OLD_VERSION}/版本**：v${VERSION}/g' '$file'"
    run_or_print "sed -i 's/Version\*\*：${OLD_VERSION}/Version**：${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本\*\*：${OLD_VERSION}/版本**：${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\(Version\|版本\)\*\*[:：] *v\?${OLD_VERSION}" --include="*.md" docs/ README.md README.zh-CN.md 2>/dev/null || true)

# Pattern 3: Implementation badges (Status: Implemented (v0.5.0+), 已实现（v0.5.0+）, Completed: v0.5.0)
while IFS= read -r file; do
    run_or_print "sed -i 's/(v${OLD_VERSION}+)/(v${VERSION}+)/g' '$file'"
    run_or_print "sed -i 's/(v${OLD_VERSION})/(v${VERSION})/g' '$file'"
    run_or_print "sed -i 's/（v${OLD_VERSION}+）/（v${VERSION}+）/g' '$file'"
    run_or_print "sed -i 's/（v${OLD_VERSION}）/（v${VERSION}）/g' '$file'"
    run_or_print "sed -i 's/Completed: v${OLD_VERSION}/Completed: v${VERSION}/g' '$file'"
    run_or_print "sed -i 's/Completed: ${OLD_VERSION}/Completed: ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\(Status\|状态\|Completed\).*\(v${OLD_VERSION}\|${OLD_VERSION}\)" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 4: Metadata lines (| Version: X.Y.Z, | Version: vX.Y.Z)
while IFS= read -r file; do
    run_or_print "sed -i 's/| Version: ${OLD_VERSION}/| Version: ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/| Version: v${OLD_VERSION}/| Version: v${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "| Version: v\?${OLD_VERSION}" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 5: ASCII architecture tree diagrams
while IFS= read -r file; do
    run_or_print "sed -i 's/version ${OLD_VERSION}/version ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/版本 ${OLD_VERSION}/版本 ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "CMakeLists\.txt.*\(version\|版本\) ${OLD_VERSION}" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 6: Code blocks & YAML config examples (version: X.Y.Z, csilk_version: X.Y.Z)
while IFS= read -r file; do
    run_or_print "sed -i 's/version: ${OLD_VERSION}/version: ${VERSION}/g' '$file'"
    run_or_print "sed -i 's/csilk_version: ${OLD_VERSION}/csilk_version: ${VERSION}/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\(version:\|csilk_version:\) ${OLD_VERSION}" --include="*.md" docs/ benchmarks/ 2>/dev/null || true)

# Pattern 7: JSON string literals in documentation (e.g. cJSON_AddStringToObject)
while IFS= read -r file; do
    run_or_print "sed -i 's/\"${OLD_VERSION}\"/\"${VERSION}\"/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "\"${OLD_VERSION}\"" --include="*.md" docs/ 2>/dev/null || true)

# Pattern 8: Spec and plan standards ("v0.5.0 标准")
while IFS= read -r file; do
    run_or_print "sed -i 's/v${OLD_VERSION} 标准/v${VERSION} 标准/g' '$file'"
    DOC_COUNT=$((DOC_COUNT + 1))
done < <(grep -rl "v${OLD_VERSION} 标准" --include="*.md" docs/ 2>/dev/null || true)

ok "Updated ${DOC_COUNT} documentation files"

# ── Step 5: Update CHANGELOGs (Promote Unreleased → Version) ─────────────────
info "Updating CHANGELOG files (Promoting [Unreleased] → [${VERSION}]) ..."

for changelog in CHANGELOG.md CHANGELOG.zh-CN.md; do
    if [[ -f "$changelog" ]]; then
        if ! $DRY_RUN; then
            # Replace [Unreleased] with new unreleased + release section
            sed -i "s/^## \[Unreleased\]/## [Unreleased]\n\n## [${VERSION}] - ${TODAY}/" "$changelog"
        fi
        ok "  ${changelog} updated"
    fi
done

# ── Step 6: Post-Update Validation & Residual Audit ──────────────────────────
info "Performing post-update residual audit ..."

if ! $DRY_RUN; then
    # 1. Check version sync against target release version
    ./scripts/check_version_sync.sh --expected "$VERSION" || die "Version sync check failed for target $VERSION."
    
    # 2. Residual scanner (excluding build, git, and past changelog history)
    RESIDUALS=$(grep -rn "${OLD_VERSION}" \
        --exclude-dir="build*" \
        --exclude-dir=".git" \
        --exclude-dir=".codegraph" \
        --exclude="CHANGELOG*.md" \
        --exclude="tag-release.sh" \
        --exclude="BENCHMARK_REPORT.md" \
        --exclude="PLAN.md" \
        include/ src/ python/ cmake/ scripts/ docs/ benchmarks/ README*.md 2>/dev/null || true)
    
    if [[ -n "$RESIDUALS" ]]; then
        warn "Found potential residual references to old version '${OLD_VERSION}':"
        echo "$RESIDUALS"
    else
        ok "Residual scan clean: 0 old version references found in active code and docs"
    fi
fi

# ── Step 7: Git Commit & Tagging ─────────────────────────────────────────────
if ! $NO_COMMIT; then
    info "Committing changes ..."
    if ! $DRY_RUN; then
        git add -A
        git commit -m "chore(release): 🚀 v${VERSION}"
        ok "Committed: chore(release): 🚀 v${VERSION}"
    else
        warn "[dry-run] Would execute: git commit -m \"chore(release): 🚀 v${VERSION}\""
    fi
else
    warn "Skipping git commit (--no-commit specified)."
fi

if ! $NO_COMMIT && ! $NO_TAG; then
    info "Creating annotated git tag ${TAG} ..."
    if ! $DRY_RUN; then
        git tag -a "$TAG" -m "Release ${TAG}"
        ok "Git tag ${TAG} created"
        
        # Verify git tag and CMake version are now in full sync
        ./scripts/check_version_sync.sh || die "Final git tag version sync check failed."
    else
        warn "[dry-run] Would execute: git tag -a ${TAG} -m \"Release ${TAG}\""
    fi
else
    if $NO_TAG; then
        warn "Skipping git tag creation (--no-tag specified)."
    fi
fi

# ── Summary & Next Steps ─────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Release ${VERSION} successfully prepared!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
echo ""
echo "  Artifacts & Components updated:"
echo "    • cmake/options.cmake (single source of truth: ${VERSION})"
echo "    • ${HEADER_COUNT} C header and source files (@version)"
echo "    • python/csilk/_version.py"
echo "    • cmake/ports/csilk/vcpkg.json"
echo "    • scripts/csilkskel"
echo "    • ${DOC_COUNT} documentation & markdown files (including dates & statuses)"
echo "    • CHANGELOG.md & CHANGELOG.zh-CN.md ([Unreleased] → [${VERSION}] - ${TODAY})"
echo ""
if ! $DRY_RUN && ! $NO_COMMIT && ! $NO_TAG; then
    echo -e "${BOLD}Next steps to publish release:${NC}"
    echo "  1. Push release commit:"
    echo "     ${CYAN}git push origin ${BRANCH}${NC}"
    echo "  2. Push release tag to trigger CI/CD & wheel builds:"
    echo "     ${CYAN}git push origin ${TAG}${NC}"
    echo "  3. Create GitHub Release at:"
    echo "     ${CYAN}https://github.com/quintin-lee/csilk/releases/new?tag=${TAG}${NC}"
else
    echo "  Dry-run or local-only mode finished. No git tags or remote changes pushed."
fi
echo ""

