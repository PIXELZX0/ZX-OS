# PlatformIO pre-build script: injects the firmware version and build env name
# so the on-device OTA updater can compare versions and pick the right asset.
#
#   ZX_FW_VERSION  - release tag (CI sets this); falls back to `git describe`
#   ZX_BUILD_ENV   - PlatformIO env name (matches openclaw-<env>-*.bin assets)
#   ZX_FW_REPO     - GitHub "owner/repo" whose releases are checked for updates;
#                    defaults to GITHUB_REPOSITORY (CI) or the git origin remote

import os
import re
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def git(*args):
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return ""


def resolve_version():
    version = os.environ.get("ZX_FW_VERSION", "").strip()
    return version or git("describe", "--tags", "--always", "--dirty") or "dev"


def resolve_repo():
    for name in ("ZX_FW_REPO", "GITHUB_REPOSITORY"):
        repo = os.environ.get(name, "").strip()
        if repo:
            return repo
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?/?$", git("remote", "get-url", "origin"))
    return match.group(1) if match else ""


defines = [
    ("ZX_FW_VERSION", env.StringifyMacro(resolve_version())),  # noqa: F821
    ("ZX_BUILD_ENV", env.StringifyMacro(env.subst("$PIOENV"))),  # noqa: F821
]
repo = resolve_repo()
if repo:
    defines.append(("ZX_FW_REPO", env.StringifyMacro(repo)))  # noqa: F821
env.Append(CPPDEFINES=defines)  # noqa: F821
