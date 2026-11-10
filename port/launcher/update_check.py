"""Release check: is a newer release of the port out on GitHub?

One request to the GitHub API at launcher start (in the background, 5 s timeout). It sends no data
about the player: a plain GET with a generic User-Agent. Any failure (offline, rate limit, a
non-release build) is silent. Only builds whose VERSION is a release tag (vX.Y.Z) are compared;
development and CI builds never report an update. Settings > Launcher turns the check off.
"""
import json
from pathlib import Path
import re
import urllib.request

REPO = 'tabnul/the-last-guardian-definitive'
API = f'https://api.github.com/repos/{REPO}/releases/latest'
RELEASES = f'https://github.com/{REPO}/releases'
_TAG = re.compile(r'^v(\d+)\.(\d+)\.(\d+)$')


def parse(tag):
    """(major, minor, patch) of a release tag like v0.9.0; None for anything else."""
    m = _TAG.match((tag or '').strip())
    return tuple(int(part) for part in m.groups()) if m else None


def own_version(port_dir):
    """The first line of the package's VERSION file ('' when there is none)."""
    try:
        return (Path(port_dir) / 'VERSION').read_text(encoding='utf-8').splitlines()[0].strip()
    except (OSError, IndexError, UnicodeDecodeError):
        return ''


def latest(timeout=5):
    """(tag, page url) of the newest published release; None on any failure."""
    request = urllib.request.Request(API, headers={'Accept': 'application/vnd.github+json',
                                                   'User-Agent': 'tlg-launcher'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            data = json.loads(response.read(256 * 1024).decode('utf-8'))
        return str(data['tag_name']), str(data.get('html_url') or RELEASES)
    except Exception:  # noqa: BLE001 - offline, rate limited, bad JSON: no notice
        return None


def newer(current, release):
    """The release tag when it is newer than the current release build; else None."""
    mine, theirs = parse(current), parse(release)
    return release if mine and theirs and theirs > mine else None


def check(port_dir, timeout=5):
    """(tag, url) when a newer release exists for this release build; else None."""
    current = own_version(port_dir)
    if not parse(current):
        return None
    found = latest(timeout)
    if found and newer(current, found[0]):
        return found
    return None
