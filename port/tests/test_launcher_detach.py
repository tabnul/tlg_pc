# SPDX-License-Identifier: GPL-2.0-or-later
"""The game runs detached from the launcher and writes its own log file: the log reader
(launcher/logtail.py), launch() without a pipe, and the classic launcher path."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run  # noqa: E402
import logtail  # noqa: E402
import userdata  # noqa: E402

class LogTailTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'run.log'

    def append(self, data):
        with open(self.path, 'ab') as handle:
            handle.write(data)

    def test_missing_file_is_empty(self):
        self.assertEqual([], logtail.LogTail(self.path).poll())

    def test_reads_only_new_bytes_with_an_offset(self):
        tail = logtail.LogTail(self.path)
        self.append(b'one\ntwo\n')
        self.assertEqual(['one', 'two'], tail.poll())
        self.assertEqual(8, tail.offset)
        self.assertEqual([], tail.poll())
        self.append(b'three\n')
        self.assertEqual(['three'], tail.poll())
        self.assertEqual(['one', 'two', 'three'], list(tail.lines))

    def test_does_not_reread_the_whole_file(self):
        tail = logtail.LogTail(self.path)
        self.append(b'a\n' * 1000)
        tail.poll()
        self.append(b'b\n')
        real_open = open
        sizes = []

        def spy(path, *args, **kwargs):
            handle = real_open(path, *args, **kwargs)
            read = handle.read
            handle.read = lambda n=-1: (sizes.append(n), read(n))[1]
            return handle
        with mock.patch('builtins.open', spy):
            self.assertEqual(['b'], tail.poll())
        self.assertEqual([2], sizes)

    def test_partial_line_waits_for_the_newline(self):
        tail = logtail.LogTail(self.path)
        self.append(b'start of a li')
        self.assertEqual([], tail.poll())
        self.append(b'ne\nnext')
        self.assertEqual(['start of a line'], tail.poll())
        self.assertEqual([], tail.poll())
        self.append(b' one\r\n')
        self.assertEqual(['next one'], tail.poll())

    def test_final_poll_returns_the_unfinished_line(self):
        tail = logtail.LogTail(self.path)
        self.append(b'done\nlast words')
        self.assertEqual(['done'], tail.poll())
        self.assertEqual(['last words'], tail.poll(final=True))

    def test_keeps_only_the_last_lines(self):
        tail = logtail.LogTail(self.path, max_lines=3)
        self.append(''.join(f'l{i}\n' for i in range(10)).encode())
        self.assertEqual(['l7', 'l8', 'l9'], tail.poll())
        self.assertEqual(['l7', 'l8', 'l9'], list(tail.lines))

    def test_a_flood_shows_only_the_end(self):
        tail = logtail.LogTail(self.path, max_lines=5)
        line = b'x' * 99 + b'\n'
        self.append(line * (logtail.MAX_CHUNK // 100 + 5000) + b'the end\n')
        new = tail.poll()
        self.assertEqual('the end', new[-1])
        self.assertEqual(5, len(new))
        self.assertTrue(all(l == 'x' * 99 for l in new[:-1]))   # no cut-in-half line

    def test_truncated_file_starts_over(self):
        tail = logtail.LogTail(self.path)
        self.append(b'old old old\n')
        tail.poll()
        self.path.write_bytes(b'new\n')
        self.assertEqual(['new'], tail.poll())

    def test_bad_utf8_is_replaced(self):
        tail = logtail.LogTail(self.path)
        self.append(b'caf\xe9\n')
        self.assertEqual(['caf�'], tail.poll())


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def launch(self):
        port = self.root / 'port'
        settings = dict(tlg_run.DEFAULT_SETTINGS, game_dir=str(self.root / 'game'))
        patches = [mock.patch.dict(os.environ, {'TLG_USER_DIR': str(self.root / 'user')}),
                   mock.patch.object(tlg_run, 'OUT_DIR', port / 'out'),
                   mock.patch.object(tlg_run, 'LOG_DIR', port / 'out' / 'logs'),
                   mock.patch.object(tlg_run, 'probe_env', return_value={}),
                   mock.patch.object(userdata, 'read_pointer', return_value={}),
                   mock.patch.object(tlg_run.subprocess, 'Popen')]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        process, log = tlg_run.launch(settings, detached=True)   # the Qt launcher's path
        return process, log, tlg_run.subprocess.Popen.call_args

    def test_no_pipe_the_log_file_is_the_output(self):
        process, log, call = self.launch()
        self.assertIs(process, tlg_run.subprocess.Popen.return_value)
        kwargs = call.kwargs
        self.assertNotEqual(tlg_run.subprocess.PIPE, kwargs['stdout'])
        self.assertTrue(hasattr(kwargs['stdout'], 'write'))
        self.assertEqual(str(log), kwargs['stdout'].name)
        self.assertEqual(tlg_run.subprocess.STDOUT, kwargs['stderr'])
        self.assertNotIn('text', kwargs)
        self.assertTrue(Path(log).is_file())

    def test_linux_has_its_own_session(self):
        _, _, call = self.launch()
        self.assertIs(True, call.kwargs['start_new_session'])
        self.assertNotIn('creationflags', call.kwargs)

    def test_windows_has_its_own_group_and_no_console(self):
        with mock.patch.object(tlg_run.os, 'name', 'nt'), \
                mock.patch.object(tlg_run.subprocess, 'CREATE_NEW_PROCESS_GROUP', 0x200, create=True), \
                mock.patch.object(tlg_run.subprocess, 'DETACHED_PROCESS', 0x8, create=True):
            options = tlg_run.detach_options()
        self.assertEqual({'creationflags': 0x200 | 0x8}, options)

    def test_real_child_survives_and_writes_the_log(self):
        # A real process (not the game): output reaches the file with no reader attached.
        log = self.root / 'real.log'
        with open(log, 'wb') as out:
            process = tlg_run.subprocess.Popen(
                [sys.executable, '-c', 'import sys; print("out"); print("err", file=sys.stderr)'],
                stdout=out, stderr=tlg_run.subprocess.STDOUT, **tlg_run.detach_options())
        self.assertEqual(0, process.wait(timeout=30))
        self.assertEqual({'out', 'err'}, set(log.read_text().split()))


class TkPathTests(unittest.TestCase):
    """The classic Tk launcher and `--play` stay attached, but the game writes its own log file:
    a pipe stalls the game once nothing reads it (a closed or crashed launcher)."""

    def test_default_launch_writes_the_log_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            settings = dict(tlg_run.DEFAULT_SETTINGS, game_dir=str(root / 'game'))
            with mock.patch.dict(os.environ, {'TLG_USER_DIR': str(root / 'user')}), \
                    mock.patch.object(tlg_run, 'OUT_DIR', root / 'out'), \
                    mock.patch.object(tlg_run, 'LOG_DIR', root / 'out' / 'logs'), \
                    mock.patch.object(tlg_run, 'probe_env', return_value={}), \
                    mock.patch.object(userdata, 'read_pointer', return_value={}), \
                    mock.patch.object(tlg_run.subprocess, 'Popen') as popen:
                _, log = tlg_run.launch(settings, header=['Frame rate: test'])
            self.assertEqual(str(log), popen.call_args.kwargs['stdout'].name)
            self.assertNotIn('start_new_session', popen.call_args.kwargs)
            self.assertEqual('Frame rate: test\n', log.read_text(encoding='utf-8'))


class FileLimitTests(unittest.TestCase):
    def test_open_file_limit_goes_to_the_hard_maximum(self):
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        try:
            if hard != resource.RLIM_INFINITY and soft > 256:
                resource.setrlimit(resource.RLIMIT_NOFILE, (256, hard))
            tlg_run.raise_open_file_limit()
            now = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
            self.assertEqual(hard if hard != resource.RLIM_INFINITY else now, now)
        finally:
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))

if __name__ == '__main__':
    unittest.main()
