#!/usr/bin/env python3
"""Exercise the shared command in temporary native Steam trees, without Vulkan."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from fgvk_remote_play.remote_play_launch import SDR_HELPER_RELATIVE_PATH, SDR_HELPER_MARKER

ROOT = Path(__file__).resolve().parent
ELF = b'\x7fELF\x02\x01' + b'\0' * 12 + b'\x3e\0' + b'test client'
SYSTEM_PYTHON_SUPPORTED = subprocess.run(
    ['/usr/bin/python3', '-I', '-c', "import tomllib, hashlib; assert hasattr(hashlib, 'file_digest')"],
    capture_output=True, timeout=5).returncode == 0


class RemotePlayCommandTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.home = Path(temporary.name)
        self.client = self.home / '.local/share/Steam/ubuntu12_64/streaming_client'
        self.client.parent.mkdir(parents=True)
        self.client.write_bytes(ELF)
        self.client.chmod(0o750)
        self.prefix = self.home / '.local'
        self.launcher = self.prefix / 'libexec/framegen-vulkan/fgvk-launch'
        self.launcher.parent.mkdir(parents=True)
        self.launcher.write_text('#!/bin/sh\nexec "$@"\n')
        self.launcher.chmod(0o750)
        helper = self.prefix / SDR_HELPER_RELATIVE_PATH
        helper.parent.mkdir(parents=True)
        helper.write_bytes(Path(os.environ['FGVK_TEST_SDR_HELPER']).read_bytes() if os.environ.get('FGVK_TEST_SDR_HELPER')
                           else ELF[:16] + b'\x03\0\x3e\0' + SDR_HELPER_MARKER)
        self.config = self.home / '.config/framegen-vulkan/conf.toml'
        self.config.parent.mkdir(parents=True)
        self.config.write_text('version=2\n[[profile]]\nname="stream-quality"\n')
        self.environment = {'PATH': '/usr/bin:/bin', 'HOME': str(self.home)}

    def command(self, action, *, config=None, success=True):
        if action == 'install' and success and not SYSTEM_PYTHON_SUPPORTED:
            self.skipTest('Remote Play activation requires system Python 3.11 or newer')
        result = subprocess.run([str(ROOT / 'fgvk-remote-play'), action,
            '--launcher', str(self.launcher), '--config', str(config or self.config)],
            env=self.environment, capture_output=True, timeout=5)
        response = json.loads(result.stdout)
        self.assertEqual(success, response['success'], response)
        self.assertEqual(0 if success else 1, result.returncode, result.stderr.decode())
        return response

    def test_disabled_status_and_refresh_never_install(self):
        self.assertFalse(self.command('status')['installed'])
        self.assertFalse(self.command('refresh')['installed'])
        self.assertEqual(ELF, self.client.read_bytes())

    @unittest.skipIf(SYSTEM_PYTHON_SUPPORTED, 'system Python supports Remote Play activation')
    def test_unsupported_system_python_preserves_original(self):
        response = self.command('install', success=False)
        self.assertIn('Python 3.11', response['error'])
        self.assertEqual(ELF, self.client.read_bytes())
        self.assertFalse(self.client.with_name('streaming_client.fgvk-original').exists())
        self.assertFalse((self.config.parent / 'native-remote-play.json').exists())

    def test_install_restore_keep_original(self):
        self.assertTrue(self.command('install')['installed'])
        state = self.config.parent / 'native-remote-play.json'
        saved = json.loads(state.read_text())
        self.assertEqual(3, saved['version'])
        self.assertEqual(str(self.launcher), saved['launcher_path'])
        self.assertNotIn('profile', saved)
        self.assertFalse(self.command('remove')['installed'])
        self.assertEqual(ELF, self.client.read_bytes())
        self.assertEqual(0o750, self.client.stat().st_mode & 0o7777)

    def test_standalone_retired_vkbasalt_choice_is_inert(self):
        self.client.write_bytes(Path('/usr/bin/env').read_bytes())
        config = self.home / 'other config/conf.toml'
        config.parent.mkdir()
        config.write_text(self.config.read_text())
        # An old saved vkBasalt choice must behave as no external layer.
        settings = {'version': 1, 'profiles': {'stream-quality': {'external_vulkan_layer': 'vkbasalt'}}}
        (config.parent / 'profile-wrapper-settings.json').write_text(json.dumps(settings))
        self.command('install', config=config)
        result = subprocess.run([str(self.client), '-0'], env=self.environment, capture_output=True, timeout=5)
        self.assertEqual(0, result.returncode, result.stderr.decode())
        values = dict(item.decode().split('=', 1) for item in result.stdout.split(b'\0') if item)
        self.assertEqual(str(config), values['FGVK_CONFIG'])
        self.assertEqual('stream-quality', values['FGVK_PROFILE'])
        self.assertNotIn('ENABLE_VKBASALT', values)
        self.assertNotIn('VKBASALT_CONFIG_FILE', values)
        self.assertEqual('1', values['DISABLE_VKBASALT'])
        self.assertFalse((config.parent / 'vkbasalt').exists())
        self.assertEqual('1', values['FGVK_FOLLOW_CURRENT_PROFILE'])
        self.assertEqual('1', values['FGVK_REMOTE_PLAY_SDR'])
        self.assertEqual(str(self.prefix / SDR_HELPER_RELATIVE_PATH), values['LD_PRELOAD'])
        self.assertNotIn('GALLIUM_DRIVER', values)
        # The stored path survives calls made by the default config.
        self.assertEqual(str(config), self.command('status')['configuration_path'])
        before = self.client.read_bytes()
        self.command('refresh')
        self.assertEqual(before, self.client.read_bytes())
        self.command('restore-before-uninstall')
        self.assertEqual(Path('/usr/bin/env').read_bytes(), self.client.read_bytes())

    def test_conflicts_leave_client_untouched(self):
        backup = self.client.with_name('streaming_client.fgvk-original')
        backup.write_bytes(ELF)
        self.command('install', success=False)
        self.assertEqual(ELF, self.client.read_bytes())

    @unittest.skipUnless(os.environ.get('FGVK_TEST_SDR_CLIENT'), 'SDL ABI client supplied by native CTest')
    def test_sdr_creation_and_native_fallback_through_real_standalone_launcher(self):
        shutil.copyfile(os.environ['FGVK_TEST_SDR_CLIENT'], self.client)
        shutil.copyfile(ROOT / 'fgvk-launch', self.launcher)
        self.environment['LD_LIBRARY_PATH'] = os.environ['FGVK_TEST_SDL_FIXTURE_DIR']
        shutil.copyfile(os.environ['FGVK_TEST_SDR_NATIVE_HELPER'], self.prefix / SDR_HELPER_RELATIVE_PATH)
        if os.environ.get('ASAN_OPTIONS'):
            self.environment['ASAN_OPTIONS'] = os.environ['ASAN_OPTIONS']
        self.command('install')
        result = subprocess.run([str(self.client), 'forced'], env=self.environment, capture_output=True, timeout=5)
        self.assertEqual(0, result.returncode, result.stderr.decode())
        self.assertIn('result=sRGB', result.stderr.decode())
        (self.prefix / SDR_HELPER_RELATIVE_PATH).unlink()
        result = subprocess.run([str(self.client), 'native'], env=self.environment, capture_output=True, timeout=5)
        self.assertEqual(0, result.returncode, result.stderr.decode())
        self.assertIn('Remote Play passthrough', result.stderr.decode())
        self.assertNotIn('result=sRGB', result.stderr.decode())
        self.command('remove')

    def test_real_standalone_launcher_ignores_retired_vkbasalt_choice(self):
        self.client.write_bytes(Path('/usr/bin/env').read_bytes())
        shutil.copyfile(ROOT / 'fgvk-launch', self.launcher)
        # Native config overrides must not change the standalone owner's global
        # launcher.conf discovery. Its ALSA setting still applies; Zink does not.
        (self.config.parent / 'launcher.conf').write_text('version=1\nenable_zink=1\nforce_alsa_audio=1\n')
        configuration = self.home / 'alternate/conf.toml'
        configuration.parent.mkdir()
        configuration.write_text(self.config.read_text())
        self.config = configuration
        prefix = self.prefix
        # A leftover vkBasalt payload must never join a launch, even when an old setting selects it.
        for relative in ('share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.json',
                         'share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.x86.json',
                         'lib/vkbasalt/libvkbasalt.so', 'lib32/vkbasalt/libvkbasalt.so'):
            path = prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        self.command('install')
        for external in ('', 'vkbasalt', ''):
            settings = {'version': 1, 'profiles': {'stream-quality': {
                'external_vulkan_layer': external}}}
            (self.config.parent / 'profile-wrapper-settings.json').write_text(json.dumps(settings))
            self.command('remove')
            self.command('install')
            result = subprocess.run([str(self.client), '-0'], env=self.environment, capture_output=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stderr.decode())
            values = dict(item.decode().split('=', 1) for item in result.stdout.split(b'\0') if item)
            self.assertEqual('stream-quality', values['FGVK_PROFILE'])
            self.assertEqual(str(self.config), values['FGVK_CONFIG'])
            self.assertNotIn('VK_LAYER_VKBASALT_post_processing', values.get('VK_INSTANCE_LAYERS', ''))
            self.assertNotIn('ENABLE_VKBASALT', values)
            self.assertNotIn('VKBASALT_CONFIG_FILE', values)
            self.assertEqual('1', values['DISABLE_VKBASALT'])
            self.assertEqual('1', values['DISABLE_GAMESCOPE_WSI'])
            self.assertEqual('alsa', values['SDL_AUDIODRIVER'])
            self.assertNotIn('GALLIUM_DRIVER', values)
        self.assertFalse((self.config.parent / 'vkbasalt').exists())


if __name__ == '__main__':
    unittest.main()
