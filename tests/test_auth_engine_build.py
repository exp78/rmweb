#!/usr/bin/env python3
"""Filesystem and input-integrity boundaries of the isolated engine builder."""
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('auth_engine_build', Path(__file__).resolve().parents[1] / 'scripts/build-auth-engine.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class EngineBuildTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_unmarked_occupied_cache_preserved(self):
        sentinel = self.root / 'unrelated'
        sentinel.write_bytes(b'keep')
        with self.assertRaises(ValueError):
            builder.own_cache(self.root)
        self.assertEqual(sentinel.read_bytes(), b'keep')

    def test_cache_rejects_nested_write_redirection(self):
        cache = self.root / 'cache'
        builder.own_cache(cache)
        (cache / 'artifacts').mkdir()
        (cache / 'artifacts/output').symlink_to(self.root / 'sentinel')
        with self.assertRaises(ValueError):
            builder.own_cache(cache)

    def test_build_volume_claims_empty_directory_and_reuses_its_marker(self):
        builder.own_build_volume(self.root)
        marker = self.root / '.purpose'
        self.assertEqual(marker.read_bytes(), b'rmweb-auth-webauthn-engine-v1\n')
        sentinel = self.root / 'object.o'
        sentinel.write_bytes(b'cached object')
        builder.own_build_volume(self.root)
        self.assertEqual(sentinel.read_bytes(), b'cached object')

    def test_legacy_build_volume_requires_fresh_cache_without_mutation(self):
        marker = self.root / '.purpose'
        previous = b'kindex-auth-webauthn-engine-v1\n'
        marker.write_bytes(previous)
        sentinel = self.root / 'object.o'
        sentinel.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'fresh dedicated rmweb build volume'):
            builder.own_build_volume(self.root)
        self.assertEqual(marker.read_bytes(), previous)
        self.assertEqual(sentinel.read_bytes(), b'keep')

    def run_shell_volume_setup(self, root):
        script = (builder.REPOSITORY / 'scripts/build-auth-engine-container.sh').read_text()
        # Execute the recipe's actual ownership gate without installing SDK
        # packages or starting an engine build. Only fixed mount paths change.
        setup, _ = script.split('[[ ! -L /build/.engine-build.lock ]]', 1)
        setup = setup.replace('/build', '"$AUTH_TEST_BUILD"').replace('/work', '"$AUTH_TEST_WORK"')
        return subprocess.run(['/bin/bash', '-c', setup, 'volume-test', '1'], capture_output=True,
                              text=True, env={**os.environ, 'AUTH_TEST_BUILD': str(root),
                                              'AUTH_TEST_WORK': str(self.root / 'work')})

    def test_shell_and_python_accept_each_others_build_volume_marker(self):
        for first in ('shell', 'python'):
            with self.subTest(first=first):
                root = self.root / first
                root.mkdir()
                if first == 'python':
                    builder.own_build_volume(root)
                result = self.run_shell_volume_setup(root)
                self.assertEqual(result.returncode, 0, result.stderr)
                builder.own_build_volume(root)
                self.assertEqual((root / '.purpose').read_bytes(), b'rmweb-auth-webauthn-engine-v1\n')

    def test_shell_preserves_legacy_build_volume_before_any_build_step(self):
        root = self.root / 'legacy'
        root.mkdir()
        marker = root / '.purpose'
        previous = b'kindex-auth-webauthn-engine-v1\n'
        marker.write_bytes(previous)
        sentinel = root / 'object.o'
        sentinel.write_bytes(b'keep')
        result = self.run_shell_volume_setup(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('fresh dedicated rmweb build volume', result.stderr)
        self.assertEqual(marker.read_bytes(), previous)
        self.assertEqual(sentinel.read_bytes(), b'keep')
        self.assertEqual({path.name for path in root.iterdir()}, {'.purpose', 'object.o'})

    def test_environment_uses_explicit_runtime_directory_with_spaces(self):
        script = self.root / 'rmweb-env.sh'
        script.write_text(builder.ENVIRONMENT)
        runtime = str(self.root / 'separate runtime')
        result = subprocess.run([
            '/bin/sh', '-c', '. "$1" || exit; printf "%s\\n" "$LD_LIBRARY_PATH" '
            '"$LIBGL_DRIVERS_PATH" "$WEBKIT_INJECTED_BUNDLE_PATH" "$GIO_EXTRA_MODULES" "$HOME"',
            'runtime-env-test', str(script),
        ], env={**os.environ, 'RMWEB_AUTH_RUNTIME': runtime}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            runtime + '/lib', runtime + '/lib/dri', runtime + '/lib/wpe-webkit-2.0/injected-bundle',
            runtime + '/lib/gio/modules', '/home/root',
        ])

    def test_environment_rejects_missing_empty_and_relative_runtime_before_exports(self):
        script = self.root / 'rmweb-env.sh'
        script.write_text(builder.ENVIRONMENT)
        for runtime in (None, '', 'relative/runtime'):
            with self.subTest(runtime=runtime):
                environment = {key: value for key, value in os.environ.items() if key != 'RMWEB_AUTH_RUNTIME'}
                if runtime is not None:
                    environment['RMWEB_AUTH_RUNTIME'] = runtime
                result = subprocess.run([
                    '/bin/sh', '-c', 'LD_LIBRARY_PATH=preserved; . "$1"; status=$?; '
                    'printf "%s\\n" "$LD_LIBRARY_PATH"; exit "$status"',
                    'runtime-env-test', str(script),
                ], env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertIn('RMWEB_AUTH_RUNTIME', result.stderr)
                self.assertEqual(result.stdout, 'preserved\n')

    def test_contained_library_links_are_flattened(self):
        source, output = self.root / 'source', self.root / 'output'
        source.mkdir()
        library = source / 'libversion.so.1'
        library.write_bytes(b'library')
        library.chmod(0o755)
        (source / 'libversion.so').symlink_to(library.name)
        builder.copy_plain(source, output)
        self.assertFalse((output / 'libversion.so').is_symlink())
        self.assertEqual((output / 'libversion.so').read_bytes(), b'library')
        self.assertEqual(builder.file_map(output)['libversion.so']['mode'], 0o755)

    def test_external_library_links_and_directory_links_rejected(self):
        source = self.root / 'source'
        source.mkdir()
        outside = self.root / 'secret'
        outside.write_bytes(b'private')
        link = source / 'redirect'
        link.symlink_to(outside)
        with self.assertRaises(ValueError):
            list(builder.contained_files(source))
        link.unlink()
        directory = source / 'directory'
        directory.mkdir()
        link.symlink_to(directory, target_is_directory=True)
        with self.assertRaises(ValueError):
            list(builder.contained_files(source))

    def test_bad_cached_input_is_not_overwritten(self):
        cache = self.root / 'cache'
        cache.mkdir()
        target = cache / 'archive'
        target.write_bytes(b'keep')
        lock = {'files': {'archive': {'sha256': '0' * 64, 'bytes': 4, 'url': 'https://example.test/archive'}}}
        with patch.object(builder.urllib.request, 'urlopen') as network:
            with self.assertRaises(ValueError):
                builder.download_inputs(cache, None, lock)
            network.assert_not_called()
        self.assertEqual(target.read_bytes(), b'keep')

    def test_oversize_download_never_publishes_input(self):
        lock = {'files': {'archive': {'sha256': '0' * 64, 'bytes': 4, 'url': 'https://example.test/archive'}}}
        with patch.object(builder.urllib.request, 'urlopen', return_value=io.BytesIO(b'123456789')):
            with self.assertRaises(ValueError):
                builder.download_inputs(self.root, None, lock)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_unknown_source_fails_without_deleting_it(self):
        canonical, destination = self.root / 'canonical', self.root / 'destination'
        canonical.mkdir(); destination.mkdir()
        (canonical / 'source.cpp').write_text('canonical')
        unexpected = destination / 'abandoned.cpp'
        unexpected.write_text('keep for review')
        with self.assertRaises(ValueError):
            builder.sync_source(canonical, destination)
        self.assertEqual(unexpected.read_text(), 'keep for review')

    def test_source_root_symlink_cannot_redirect_writes(self):
        canonical, unrelated = self.root / 'canonical', self.root / 'unrelated'
        canonical.mkdir(); unrelated.mkdir()
        (canonical / 'marker.cpp').write_text('source')
        destination = self.root / 'source'
        destination.symlink_to(unrelated, target_is_directory=True)
        with self.assertRaises(ValueError):
            builder.sync_source(canonical, destination)
        self.assertEqual(list(unrelated.iterdir()), [])

    def test_pinned_contained_dangling_source_link_is_preserved_only_in_source(self):
        canonical, destination = self.root / 'canonical', self.root / 'destination'
        canonical.mkdir()
        (canonical / 'Cargo.toml').symlink_to('unused/Cargo.toml')
        builder.sync_source(canonical, destination)
        self.assertEqual(os.readlink(destination / 'Cargo.toml'), 'unused/Cargo.toml')
        builder.sync_source(canonical, destination)
        with self.assertRaises(ValueError):
            builder.copy_plain(destination, self.root / 'runtime')
        (canonical / 'Cargo.toml').unlink()
        (canonical / 'Cargo.toml').symlink_to('../outside')
        with self.assertRaises(ValueError):
            builder.sync_source(canonical, destination)

    def test_source_reuse_preserves_timestamps_and_removes_only_identified_bytecode(self):
        canonical, destination = self.root / 'canonical', self.root / 'destination'
        canonical.mkdir(); destination.mkdir()
        (canonical / 'generate.py').write_text('canonical')
        builder.sync_source(canonical, destination)
        stamp = (destination / 'generate.py').stat().st_mtime_ns
        bytecode = destination / '__pycache__/generate.cpython-312.pyc'
        bytecode.parent.mkdir(); bytecode.write_bytes(b'old code')
        builder.sync_source(canonical, destination)
        self.assertFalse(bytecode.exists())
        self.assertEqual((destination / 'generate.py').stat().st_mtime_ns, stamp)

    def test_changed_source_does_not_restore_stale_archive_timestamp(self):
        canonical, destination = self.root / 'canonical', self.root / 'destination'
        canonical.mkdir(); destination.mkdir()
        source = canonical / 'source.cpp'
        target = destination / 'source.cpp'
        source.write_text('restored canonical code')
        target.write_text('previous patched code')
        os.utime(source, (1, 1))
        builder.sync_source(canonical, destination)
        self.assertEqual(target.read_text(), source.read_text())
        self.assertGreater(target.stat().st_mtime, source.stat().st_mtime)

    def test_artifact_publication_keeps_previous_complete_result(self):
        destination, output = self.root / 'artifacts', self.root / 'next'
        destination.mkdir(); output.mkdir()
        (destination / 'manifest.json').write_text('previous')
        (output / 'manifest.json').write_text('next')
        builder.publish_artifacts(output, destination)
        self.assertEqual((destination / 'manifest.json').read_text(), 'next')
        self.assertEqual((self.root / '.artifacts.previous/manifest.json').read_text(), 'previous')

    def test_failed_publish_restores_previous_complete_result(self):
        destination = self.root / 'artifacts'
        destination.mkdir()
        (destination / 'manifest.json').write_text('previous')
        with self.assertRaises(OSError):
            builder.publish_artifacts(self.root / 'missing', destination)
        self.assertEqual((destination / 'manifest.json').read_text(), 'previous')

    def test_dependency_profile_allows_only_matching_incremental_reuse(self):
        profile = {'sdk': 'pinned image', 'inputs': 'pinned dependencies', 'headers': 'pinned generator'}
        builder.verify_dependency_profile(self.root, profile)
        (self.root / 'configure-webauthn-on').mkdir()
        builder.verify_dependency_profile(self.root, profile)
        recorded = (self.root / '.dependency-profile.json').read_bytes()
        for key in profile:
            with self.assertRaises(ValueError):
                builder.verify_dependency_profile(self.root, {**profile, key: 'changed'})
            self.assertEqual((self.root / '.dependency-profile.json').read_bytes(), recorded)

    def test_existing_unattested_objects_are_never_adopted(self):
        build = self.root / 'configure-webauthn-on'
        build.mkdir()
        (build / 'object.o').write_bytes(b'keep')
        with self.assertRaises(ValueError):
            builder.verify_dependency_profile(self.root, {'sdk': 'current'})
        self.assertFalse((self.root / '.dependency-profile.json').exists())
        self.assertEqual((build / 'object.o').read_bytes(), b'keep')

    def test_dependency_profile_symlink_never_creates_external_file(self):
        outside = self.root / 'unrelated'
        (self.root / '.dependency-profile.json').symlink_to(outside)
        with self.assertRaises(ValueError):
            builder.verify_dependency_profile(self.root, {'sdk': 'current'})
        self.assertFalse(outside.exists())


if __name__ == '__main__':
    unittest.main()
