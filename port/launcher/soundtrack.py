"""The game's music for the launcher's Soundtrack page, read from the player's own game folder
(nothing of the game is shipped).

The music sits in game/sounds/bgm/ inside the trico_*.psarc archives: per stage a sound bank
(<bank>_sbuf.data: a 16-byte CDAT wrapper around Sony's SXDF header) and for most stages a stream
file (<bank>_stream.scs, SXDS) with the audio itself. Each WAVE entry of the bank is one piece of
music in ATRAC9, 4 or 6 channels at 48 kHz (the game mixes the layers live). The layout follows
vgmstream's SXD reader (src/meta/sndx.c, which names this game as one that keeps some audio in the
header file). The same piece often sits in several stage banks; it is listed once.

There are no track titles in the game; a few banks name their cues (bgm_fa_antenna, ...), so a
piece is called by its stage and number, with that name where the bank has one for it.
"""

import game_image
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import struct
import zlib

INDEX_VERSION = 2

# Bank code (bgm_st04_fa) -> a readable stage name. Only labels: the list itself always comes from
# the banks in the game folder, and a bank with an unknown code is listed under its code.
STAGES = {
    'op': 'Opening', 'tc': "Trico's Cave", 'ca': 'Canyon', 'fa': 'First Antenna', 'at': 'Attacked',
    'fb': 'Falling Bridge', 'al': 'Alone', 'lt': 'Large Tower', 'fb2': 'Falling Bridge 2', 'pi': 'The Pit',
    'cc': 'Column Corridor', 'tn': 'The Nest', 'ff': 'First Flight', 'ch': 'Condor Clash',
    'sf': 'Second Flight', 'sd': "Soldiers' Den", 'ed': 'Ending',
}


class Psarc:
    """One PSARC archive: its file names, and reads of a whole file or a byte range of one."""

    def __init__(self, path):
        self.file = path.open('rb')  # a Path, or game_image.EntryPath inside a .zar
        magic, _, _, self.comp, toc_len, entry_size, count, self.block, _ = struct.unpack(
            '>4sHH4sIIIII', self.file.read(32))
        if magic != b'PSAR':
            raise ValueError(f'{path}: not a PSARC archive')
        toc = self.file.read(toc_len - 32)
        self.entries = []
        for i in range(count):
            e = toc[i * entry_size:(i + 1) * entry_size]
            self.entries.append((struct.unpack('>I', e[16:20])[0], int.from_bytes(e[20:25], 'big'),
                                 int.from_bytes(e[25:30], 'big')))
        width = 2 if self.block <= 0x10000 else 3 if self.block <= 0x1000000 else 4
        rest = toc[count * entry_size:]
        self.blocks = [int.from_bytes(rest[i:i + width], 'big') for i in range(0, len(rest) - width + 1, width)]
        # The name list is entry 0; each name's entry is found by the MD5 of the upper-case name
        # (the update archives do not keep the entries in name order).
        md5 = {toc[i * entry_size:i * entry_size + 16]: i for i in range(count)}
        names = [n for n in re.split('[\n\0]', self._read(0, 0, None).decode('utf-8', 'replace')) if n]
        self.names = {}
        for n in names:
            index = md5.get(hashlib.md5(n.upper().encode()).digest())
            if index is not None:
                self.names[n.lower()] = index

    def close(self):
        self.file.close()

    def _read(self, entry, start, length):
        first, size, offset = self.entries[entry]
        end = size if length is None else min(size, start + length)
        skip = start // self.block
        pos = offset + sum(self.blocks[first + i] or self.block for i in range(skip))
        out = bytearray()
        got = skip * self.block
        self.file.seek(pos)
        while got < end:
            z = self.blocks[first + (got // self.block)]
            want = min(self.block, size - got)
            data = self.file.read(z or self.block)
            if not data:
                raise ValueError('archive entry runs past the end of the file')
            if z == 0 or z == want:
                out += data[:want]
            elif self.comp == b'zlib':
                out += zlib.decompress(data)
            else:
                out += lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(data)
            got += want
        lead = start - skip * self.block
        return bytes(out[lead:lead + (end - start)])

    def read(self, name, start=0, length=None):
        return self._read(self.names[name], start, length)

    def size(self, name):
        return self.entries[self.names[name]][1]


def _archive_paths(game_dir):
    """Every trico_*.psarc of the game folder. Sorted by name, an update archive (trico_001p) comes
    right after the one it updates ('.' sorts before 'p'), so its files win."""
    try:
        return game_image.root_files(game_dir, 'trico_*.psarc')
    except (OSError, RuntimeError, ValueError):
        return []


def _open_archives(game_dir):
    """name -> (archive, its file name) for every bgm file; a later (update) archive wins."""
    found, opened = {}, []
    for path in _archive_paths(game_dir):
        try:
            archive = Psarc(path)
        except (OSError, ValueError, struct.error, zlib.error, lzma.LZMAError, IndexError):
            continue   # unreadable archive: the music in the others is still listed
        opened.append(archive)
        for n in archive.names:
            if n.startswith('game/sounds/bgm/'):
                found[n] = (archive, path.name)
    return found, opened


def _chunks(head, start):
    at = start
    while at + 8 <= len(head):
        tag, size = head[at:at + 4], struct.unpack_from('<I', head, at + 4)[0]
        yield tag, at + 8, size
        at += 8 + size


def _bank_waves(head):
    """The pieces of one bank header: dicts with the stream details, and the cue names."""
    names, waves = {}, []
    for tag, body, size in _chunks(head, 0x60):
        if tag == b'NAME' and body + 8 <= len(head):
            for i in range(struct.unpack_from('<h', head, body + 4)[0]):
                e = body + 8 + i * 12
                if e + 12 > len(head):
                    break
                rel, _, cue = struct.unpack_from('<iII', head, e)
                end = head.find(b'\0', e + rel)
                if end > e + rel:
                    names.setdefault(cue, head[e + rel:end].decode('ascii', 'replace'))
        elif tag == b'WAVE':
            count = struct.unpack_from('<i', head, body + 4)[0]
            for i in range(count):
                table = body + 8 + 4 * i
                e = table + struct.unpack_from('<i', head, table)[0]
                if e + 0x28 > len(head):
                    break
                flags, codec, channels = struct.unpack_from('<IBB', head, e)
                rate, = struct.unpack_from('<i', head, e + 8)
                samples, = struct.unpack_from('<i', head, e + 0x14)
                stream_size, stream_offset = struct.unpack_from('<II', head, e + 0x24 - 4)
                config = None
                for x in range(e + 0x28, min(body + size, len(head)) - 8, 4):
                    if struct.unpack_from('>I', head, x)[0] in (0x0A010000, 0x0A010600):
                        config = struct.unpack_from('<I', head, x + 4)[0]
                        break
                if codec != 0x42 or not config or rate <= 0 or samples <= 0:
                    continue   # only ATRAC9 music; nothing else is in the bgm banks
                external = bool(flags & 2)
                waves.append({'wave': i, 'channels': channels, 'rate': rate, 'samples': samples,
                              'size': stream_size, 'external': external, 'config': config,
                              'offset': stream_offset if external else e + 0x24 + stream_offset})
    return waves, names


def scan(game_dir, cache_dir):
    """All pieces of music, each once, in story order: dicts with stage, number, name, seconds and
    where the audio is. Kept in cache_dir/soundtrack.json until the archives change."""
    stamp = []
    for path in _archive_paths(game_dir):
        st = path.stat()
        stamp.append([path.name, st.st_size, int(st.st_mtime)])
    cache = Path(cache_dir) / 'soundtrack.json'
    try:
        saved = json.loads(cache.read_text(encoding='utf-8'))
        if saved.get('version') == INDEX_VERSION and saved.get('stamp') == stamp:
            return saved['tracks']
    except (OSError, ValueError, KeyError):
        pass
    found, opened = _open_archives(game_dir)
    tracks, seen = [], {}
    try:
        banks = sorted(n for n in found if n.endswith('_sbuf.data'))
        for bank in banks:
            try:
                _scan_bank(found, bank, tracks, seen)
            except (OSError, ValueError, KeyError, IndexError, struct.error, zlib.error, lzma.LZMAError):
                continue   # a bank laid out differently is left out instead of failing the list
    finally:
        for archive in opened:
            archive.close()
    tracks.sort(key=lambda t: (t['stage_no'], t['stage'], t['number']))
    try:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps({'version': INDEX_VERSION, 'stamp': stamp, 'tracks': tracks}),
                         encoding='utf-8')
    except OSError:
        pass
    return tracks


def _scan_bank(found, bank, tracks, seen):
    """Adds the pieces of one bank that are not listed yet."""
    archive, archive_name = found[bank]
    head = archive.read(bank, 0, 4 << 20)
    body = head[16:] if head[:4] == b'CDAT' else head
    shift = len(head) - len(body)
    if body[:4] != b'SXDF':
        return
    waves, names = _bank_waves(body)
    stem = bank.rsplit('/', 1)[-1][:-len('_sbuf.data')]
    code = re.match(r'bgm_st(\d+)_([a-z0-9]+?)(?:_(\d+))?$', stem)
    if code:
        stage_no, stage_code, part = int(code.group(1)), code.group(2), code.group(3)
        stage = STAGES.get(stage_code, stage_code.upper())
        if part:
            stage += f' {int(part)}'
    else:   # a bank named differently: listed after the stages, under its file name
        stage_no, stage = 999, stem
    stream = bank.replace('_sbuf.data', '_stream.scs')
    number = 0
    for w in waves:
        if w['external']:
            if stream not in found:
                continue
            src, src_archive, offset = stream, found[stream][1], w['offset']
            sample = found[stream][0].read(stream, offset, 4096)
        else:
            src, src_archive, offset = bank, archive_name, w['offset'] + shift
            sample = archive.read(bank, offset, 4096)
        key = hashlib.md5(sample + struct.pack('<ii', w['samples'], w['size'])).hexdigest()
        if key in seen:
            also = seen[key].setdefault('also', [])
            if stage != seen[key]['stage'] and stage not in also:
                also.append(stage)
            continue
        number += 1
        track = {'stage_no': stage_no, 'stage': stage, 'number': number,
                 'name': names.get(w['wave'], ''), 'seconds': round(w['samples'] / w['rate'], 1),
                 'archive': src_archive, 'file': src, 'offset': offset, 'size': w['size'],
                 'channels': w['channels'], 'rate': w['rate'], 'samples': w['samples'],
                 'config': w['config']}
        seen[key] = track
        tracks.append(track)


def extract(game_dir, track, out_path):
    """Writes one piece as an ATRAC9 .at9 file (RIFF/WAVE, the layout the music tool reads)."""
    archive = Psarc(game_image.game_path(game_dir, track['archive']))
    try:
        data = archive.read(track['file'], track['offset'], track['size'])
    finally:
        archive.close()
    channels, rate, config = track['channels'], track['rate'], track['config']
    # ATRAC9 config (LibAtrac9 ReadConfigData): sync byte 0xFE, then 4 bits sample rate, 3 bits
    # channel layout, 1 bit, 11 bits frame bytes - 1, 2 bits log2 frames per superframe, 6 unused.
    word = config & 0xFFFFFF
    frame_bytes = ((word >> 8) & 0x7FF) + 1
    superframe = frame_bytes << ((word >> 6) & 3)
    channel_masks = {1: 0x4, 2: 0x3, 4: 0x33, 6: 0x3F, 8: 0x63F}
    fmt = struct.pack('<HHIIHHHHI', 0xFFFE, channels, rate, rate * superframe // 1024 or rate,
                      superframe, 0, 34, 256, channel_masks.get(channels, 0))
    fmt += bytes.fromhex('d2425e47a8c88f49aaba86cf4b3d1e0f')[:16]   # KSDATAFORMAT_SUBTYPE_ATRAC9
    fmt += struct.pack('<I', 1) + struct.pack('>I', config) + b'\0' * 4
    fact = struct.pack('<III', track['samples'], 0, 0)
    body = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'fact' + struct.pack('<I', len(fact)) +
            fact + b'data' + struct.pack('<I', len(data)) + data)
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix('.tmp')
    tmp.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)
    os.replace(tmp, out_path)
    return out_path


def title(track):
    text = f"{track['stage']}, piece {track['number']}"
    if track.get('name'):
        text += f" ({track['name']})"
    return text
