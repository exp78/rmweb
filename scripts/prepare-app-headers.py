#!/usr/bin/env python3
"""Prepare development headers only; never build/replace the release runtime."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from app_only_cache import require_owned_cache

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache', type=Path, required=True, help='Dedicated external app-only cache')
args = parser.parse_args()
BASE = args.cache.expanduser().resolve()
REPOSITORY = Path(__file__).resolve().parents[1]
if BASE == REPOSITORY or REPOSITORY in BASE.parents:
    parser.error('cache must remain outside the repository')
require_owned_cache(BASE)
UNIFDEF = shutil.which('unifdef')
MKENUMS = shutil.which('glib-mkenums')
if not UNIFDEF or not MKENUMS:
    parser.error('install unifdef and glib-mkenums (macOS: brew install unifdef glib)')
SOURCES = BASE / 'sources'
FLAGS = ['-DWTF_PLATFORM_GTK=0', '-DWTF_PLATFORM_WPE=1', '-DUSE_GTK4=0',
         '-DENABLE_2022_GLIB_API=1', '-DENABLE_WPE_PLATFORM=1', '-DUSE_GI_FINISH_FUNC_ANNOTATION=0']
INPUTS = {
    'wpewebkit-2.48.5.tar.xz': ('https://wpewebkit.org/releases/wpewebkit-2.48.5.tar.xz',
        '01f36010705adb14404c56baf033147f7927cc7c6badec81bb141266fcdd8d0b'),
    'libwpe-1.16.2.tar.xz': ('https://wpewebkit.org/releases/libwpe-1.16.2.tar.xz',
        '960bdd11c3f2cf5bd91569603ed6d2aa42fd4000ed7cac930a804eac367888d7'),
    'libsoup-3.6.0.tar.xz': ('https://download.gnome.org/sources/libsoup/3.6/libsoup-3.6.0.tar.xz',
        '62959f791e8e8442f8c13cedac8c4919d78f9120d5bb5301be67a5e53318b4a3'),
    'libxkbcommon-1.7.0.tar.xz': ('https://xkbcommon.org/download/libxkbcommon-1.7.0.tar.xz',
        '65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247'),
}

def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True, capture_output=True).stdout

def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)

def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)

def configure_version(source, destination):
    text = source.read_text()
    for key, value in [('MAJOR', 2), ('MINOR', 48), ('MICRO', 5)]:
        text = text.replace(f'@PROJECT_VERSION_{key}@', str(value))
    write(destination, text)

SOURCES.mkdir(parents=True, exist_ok=True)
for filename, (url, expected) in INPUTS.items():
    archive_path = SOURCES / filename
    if not archive_path.exists():
        temporary = archive_path.with_suffix(archive_path.suffix + '.partial')
        try:
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
                raise SystemExit(f'Checksum mismatch: {filename}')
            temporary.replace(archive_path)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive_path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Checksum mismatch: {filename}')

# Always regenerate from verified archives; never execute scripts from an old,
# editable source extraction. Keep output separate from downloads and SDK files.
work = tempfile.TemporaryDirectory(prefix='headers-', dir=BASE)
EXTRACTED = Path(work.name) / 'sources'
STAGE = Path(work.name) / 'stage/usr'
for filename in INPUTS:
    with tarfile.open(SOURCES / filename) as archive:
        selected = []
        for item in archive:
            path = item.name
            if not item.isfile():
                continue
            if filename.startswith('wpewebkit'):
                wanted = ('/Source/WebKit/UIProcess/API/' in path
                    or '/Source/WebKit/WPEPlatform/' in path
                    or '/Source/JavaScriptCore/API/glib/' in path
                    or path.endswith('/Source/WebKit/PlatformWPE.cmake')
                    or path.endswith('/Source/WebKit/Scripts/glib/generate-api-header.py'))
                wanted = wanted and (path.endswith(('.h', '.h.in', 'CMakeLists.txt', 'PlatformWPE.cmake', 'generate-api-header.py')))
            elif filename.startswith('libwpe'):
                wanted = '/include/wpe/' in path and path.endswith('.h')
            elif filename.startswith('libsoup'):
                wanted = '/libsoup/' in path
            else:
                wanted = '/include/xkbcommon/' in path and path.endswith('.h')
            if wanted:
                selected.append(item)
        archive.extractall(EXTRACTED, members=selected, filter='data')
WPE = EXTRACTED / 'wpewebkit-2.48.5'
WEBKIT = WPE / 'Source/WebKit'
JSC = WPE / 'Source/JavaScriptCore/API/glib'
PUBLIC = STAGE / 'include/wpe-webkit-2.0'
GENERATOR = WEBKIT / 'Scripts/glib/generate-api-header.py'

# Main API templates are selected from the exact upstream install manifest.
platform = (WEBKIT / 'PlatformWPE.cmake').read_text()
block = re.search(r'set\(WPE_API_HEADER_TEMPLATES\s+(.*?)\n\)', platform, re.S).group(1)
templates = re.findall(r'\$\{WEBKIT_DIR\}/([^\s]+)', block)
templates.append('UIProcess/API/glib/WebKitNetworkSession.h.in')
for relative in templates:
    source = WEBKIT / relative
    destination = PUBLIC / 'wpe' / source.name.removesuffix('.in')
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(sys.executable, GENERATOR, 'WPE', source, destination, UNIFDEF, *FLAGS)
for name in ['WebKitColor.h', 'WebKitRectangle.h', 'WebKitWebViewBackend.h']:
    copy(WEBKIT / 'UIProcess/API/wpe' / name, PUBLIC / 'wpe' / name)
configure_version(WEBKIT / 'UIProcess/API/wpe/WebKitVersion.h.in', PUBLIC / 'wpe/WebKitVersion.h')
headers = sorted((PUBLIC / 'wpe').glob('*.h'))
headers = [path for path in headers if path.name != 'WebKitEnumTypes.h']
enum = run(MKENUMS, '--template', WEBKIT / 'UIProcess/API/wpe/WebKitEnumTypes.h.in', *headers).decode()
write(PUBLIC / 'wpe/WebKitEnumTypes.h', enum.replace('web_kit', 'webkit').replace('WEBKIT_TYPE_KIT', 'WEBKIT_TYPE'))

# JSC GLib declarations are exported by libWPEWebKit in this release.
for source in JSC.glob('*.h.in'):
    destination = PUBLIC / 'jsc' / source.name.removesuffix('.in')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.name == 'JSCVersion.h.in':
        configure_version(source, destination)
    else:
        run(sys.executable, GENERATOR, 'WPE', source, destination, UNIFDEF, *FLAGS)
for name in ['JSCOptions.h', 'JSCAutocleanups.h']:
    copy(JSC / name, PUBLIC / 'jsc' / name)

# WPEPlatform uses opaque GL handles, so no Mesa implementation build is needed.
wpe_platform = WEBKIT / 'WPEPlatform'
platform_cmake = (wpe_platform / 'CMakeLists.txt').read_text()
block = re.search(r'set\(WPEPlatform_INSTALLED_HEADERS\s+(.*?)\n\)', platform_cmake, re.S).group(1)
relative_headers = re.findall(r'\$\{WEBKIT_DIR\}/WPEPlatform/(\S+\.h)', block)
platform_dest = PUBLIC / 'wpe-platform'
for relative in relative_headers:
    copy(wpe_platform / relative, platform_dest / relative)
for source in (wpe_platform / 'wpe/headless').glob('*.h'):
    copy(source, platform_dest / 'wpe/headless' / source.name)
configure_version(wpe_platform / 'wpe/WPEVersion.h.in', platform_dest / 'wpe/WPEVersion.h')
config = (wpe_platform / 'wpe/WPEConfig.h.in').read_text()
config = config.replace('#cmakedefine WPE_PLATFORM_HEADLESS', '#define WPE_PLATFORM_HEADLESS')
config = re.sub(r'#cmakedefine (\S+)', r'/* #undef \1 */', config)
write(platform_dest / 'wpe/WPEConfig.h', config)
headers = [path for path in sorted((platform_dest / 'wpe').glob('*.h')) if path.name != 'WPEEnumTypes.h']
enum = run(MKENUMS, '--template', wpe_platform / 'wpe/WPEEnumTypes.h.in', *headers).decode()
for before, after in [('w_pe','wpe'), ('WPE_TYPE_PE','WPE_TYPE'), ('WPE_TYPEEGL','WPE_TYPE_EGL'), ('wpeegl','wpe_egl')]:
    enum = enum.replace(before, after)
write(platform_dest / 'wpe/WPEEnumTypes.h', enum)

for source in (EXTRACTED / 'libwpe-1.16.2/include/wpe').glob('*.h'):
    copy(source, STAGE / 'include/wpe-1.0/wpe' / source.name)

soup = EXTRACTED / 'libsoup-3.6.0/libsoup'
block = re.search(r'soup_introspection_headers = \[(.*?)\n\]', (soup/'meson.build').read_text(), re.S).group(1)
soup_headers = re.findall(r"'([^']+\.h)'", block)
soup_dest = STAGE / 'include/libsoup-3.0/libsoup'
for relative in soup_headers:
    copy(soup / relative, soup_dest / Path(relative).name)
copy(soup / 'include/soup-installed.h', soup_dest / 'soup.h')
run(sys.executable, soup / 'generate-version-header.py', soup / 'soup-version.h.in', soup_dest / 'soup-version.h', '3.6.0')
enum = run(MKENUMS, '--template', soup/'soup-enum-types.h.template', *[soup / item for item in soup_headers]).decode()
write(soup_dest / 'soup-enum-types.h', enum)

for source in (EXTRACTED / 'libxkbcommon-1.7.0/include/xkbcommon').glob('*.h'):
    copy(source, STAGE / 'include/xkbcommon' / source.name)

def pc(name, version, requires, library, include):
    text = f'''prefix=/usr
exec_prefix=${{prefix}}
libdir=${{exec_prefix}}/lib
includedir=${{prefix}}/include
Name: {name}
Description: App-only development overlay for the pinned rmweb runtime
Version: {version}
Requires: {requires}
Libs: -L${{libdir}} -l{library}
Cflags: -I${{includedir}}/{include}
'''
    write(STAGE / 'lib/pkgconfig' / f'{name}.pc', text)

pc('wpe-webkit-2.0', '2.48.5', 'glib-2.0 libsoup-3.0 wpe-1.0 wpe-platform-2.0', 'WPEWebKit-2.0', 'wpe-webkit-2.0')
pc('wpe-platform-2.0', '2.48.5', 'glib-2.0 gobject-2.0 gio-2.0', 'WPEPlatform-2.0', 'wpe-webkit-2.0/wpe-platform')
pc('wpe-1.0', '1.16.2', '', 'wpe-1.0', 'wpe-1.0')
pc('libsoup-3.0', '3.6.0', 'glib-2.0 gobject-2.0 gio-2.0', 'soup-3.0', 'libsoup-3.0')
manifest = {'schemaVersion': 1, 'inputs': {name: {'url': url, 'sha256': sha, 'bytes': (SOURCES/name).stat().st_size}
    for name, (url, sha) in INPUTS.items()}, 'generatedHeaders': {str(path.relative_to(STAGE)): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(STAGE.rglob('*')) if path.is_file()}}
manifest['generatorTools'] = {'python': sys.version.split()[0], 'glibMkenums': run(MKENUMS, '--version').decode().strip(), 'unifdef': UNIFDEF}
# The stage directory is owned by this build lane. Replace it only after all
# pinned header generators succeed.
if (BASE / 'stage').exists():
    shutil.rmtree(BASE / 'stage')
(STAGE.parent).replace(BASE / 'stage')
write(BASE / 'header-manifest.json', json.dumps(manifest, indent=2) + '\n')
work.cleanup()
print(f'Prepared {len(manifest["generatedHeaders"])} development files under {BASE / 'stage/usr'}')
