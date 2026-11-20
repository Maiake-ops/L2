"""Recipe size limits must reject input before any build command executes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent


class RecipeValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', '-C', str(REPO / 'l2pm')], check=True,
                       capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='l2pm-recipe-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, L2PM_ROOT=str(self.root / 'root'))
        self.recipe = self.root / 'test.recipe'

    def run_recipe(self, text, command='build'):
        self.recipe.write_text(text)
        return subprocess.run([str(REPO / 'l2pm/l2pm'), command, str(self.recipe)],
                              env=self.env, cwd=self.root, capture_output=True,
                              text=True, timeout=20)

    def test_oversized_fields(self):
        limits = {'name': 128, 'version': 128, 'source': 4096,
                  'sha256': 128, 'source_dir': 4096, 'srcdir': 4096,
                  'deps': 1024, 'repo': 4096, 'ref': 128,
                  'build': 16384, 'compile': 16384, 'install': 16384}
        for key, limit in limits.items():
            with self.subTest(key=key):
                result = self.run_recipe('name=test\nversion=1\n' +
                                         key + '=' + 'x' * limit + '\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f'{self.recipe}:3:', result.stderr)
                self.assertIn(f'{key} exceeds {limit - 1} bytes', result.stderr)
                self.assertFalse((self.root / 'root').exists())

    def test_oversized_command_never_executes(self):
        marker = self.root / 'executed'
        command = f'printf ran > "{marker}"; #' + 'x' * 16384
        for operation in ('build', 'add'):
            with self.subTest(operation=operation):
                result = self.run_recipe('name=test\nversion=1\nbuild=' + command,
                                         operation)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(marker.exists(), 'A truncated build command ran')
                self.assertIn('build exceeds 16383 bytes', result.stderr)

    def test_maximum_command_and_names_are_accepted(self):
        for name, version in [('n' * 127, '1'), ('test', 'v' * 127)]:
            with self.subTest(name_length=len(name), version_length=len(version)):
                result = self.run_recipe('name=' + name + '\nversion=' + version +
                                         '\nbuild=: #' + 'x' * (16383 - 3) + '\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('sha256:', result.stdout)


if __name__ == '__main__':
    unittest.main()
