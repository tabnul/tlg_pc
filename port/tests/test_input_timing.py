"""The code input_timing.h hooks, against the 01.03 eboot (out/eboot.elf, when present)."""
import re
import struct
import unittest

from paths import ROOT

ELF = ROOT / 'out' / 'eboot.elf'
HEADER = ROOT / 'src' / 'input_timing.h'


def fnv(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


class InputTimingCode(unittest.TestCase):
    @unittest.skipUnless(ELF.exists(), 'needs out/eboot.elf')
    def test_fingerprints_match_the_eboot(self):
        elf = ELF.read_bytes()
        table, count = struct.unpack_from('<Q', elf, 0x20)[0], struct.unpack_from('<H', elf, 0x38)[0]
        loads = [struct.unpack_from('<IIQQQQ', elf, table + i * 56) for i in range(count)]

        def read(address, size):
            for kind, _, offset, virtual, _, file_size in loads:
                if kind == 1 and virtual <= address and address + size <= virtual + file_size:
                    return elf[offset + address - virtual:offset + address - virtual + size]
            self.fail(f'{address:#x} not in a PT_LOAD segment')

        code = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)ull\}', HEADER.read_text())
        self.assertEqual(len(code), 10)
        for address, size, value in code:
            with self.subTest(address=address):
                self.assertEqual(fnv(read(int(address, 16), int(size, 16))), int(value, 16))
        # The hooked sites: the update's prologue and the snapshot memcpy call.
        self.assertEqual(read(0x72d170, 8), bytes.fromhex('554889e541574156'))
        self.assertEqual(read(0x72edb0, 1), b'\xe8')


if __name__ == '__main__':
    unittest.main()
