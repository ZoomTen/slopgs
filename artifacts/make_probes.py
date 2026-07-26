#!/usr/bin/env python3
"""Generate MSGS reference-render probe MIDIs. Stdlib only.

Each file isolates ONE variable so a diff against the real synth's render
points at a specific DSP stage instead of "something is off somewhere".
Run: python3 make_probes.py   -> writes *.mid next to this script.
"""

import os
import struct

TPQ = 480          # ticks per quarter
TEMPO = 500_000    # us per quarter -> 120bpm -> 960 ticks/sec
TPS = TPQ * 1_000_000 // TEMPO


def t(seconds):
    return int(round(seconds * TPS))


def varlen(n):
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


class Track:
    """Absolute-time event list, serialized to delta times at the end."""

    def __init__(self):
        self.events = []   # (abs_tick, order, raw_bytes)
        self._seq = 0

    def at(self, tick, data):
        self.events.append((tick, self._seq, bytes(data)))
        self._seq += 1
        return self

    def note(self, tick, dur, ch, key, vel=100, off_vel=0):
        self.at(tick, [0x90 | ch, key, vel])
        self.at(tick + dur, [0x80 | ch, key, off_vel])
        return self

    def cc(self, tick, ch, num, val):
        return self.at(tick, [0xB0 | ch, num, val])

    def prog(self, tick, ch, num):
        return self.at(tick, [0xC0 | ch, num])

    def bend(self, tick, ch, value14):
        return self.at(tick, [0xE0 | ch, value14 & 0x7F, (value14 >> 7) & 0x7F])

    def sysex(self, tick, payload):
        # payload excludes the leading F0, includes the trailing F7
        return self.at(tick, b"\xf0" + varlen(len(payload)) + bytes(payload))

    def rpn(self, tick, ch, rpn_msb, rpn_lsb, data_msb):
        self.cc(tick, ch, 101, rpn_msb)
        self.cc(tick, ch, 100, rpn_lsb)
        self.cc(tick, ch, 6, data_msb)
        return self

    def serialize(self):
        body = bytearray()
        body += b"\x00\xff\x51\x03" + struct.pack(">I", TEMPO)[1:]
        prev = 0
        for tick, _, data in sorted(self.events, key=lambda e: (e[0], e[1])):
            body += varlen(tick - prev) + data
            prev = tick
        body += b"\x00\xff\x2f\x00"
        return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


# CLAUDE.adoc: "All MIDIs must be generated in probes/." Every .mid and every
# .manifest.tsv lands here; `here` stays the artifacts/ dir because the gm.dls
# lookup below is relative to that, not to probes/.
PROBE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probes")


def write(name, track, raw_track=None):
    chunk = raw_track if raw_track is not None else track.serialize()
    head = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ)
    os.makedirs(PROBE_DIR, exist_ok=True)
    path = os.path.join(PROBE_DIR, name)
    with open(path, "wb") as f:
        f.write(head + chunk)
    return path


# --- probes ---------------------------------------------------------------

def p01_programs():
    """All 128 GM programs, identical note. Isolates per-patch region lookup."""
    tr = Track()
    for prog in range(128):
        base = t(prog * 2.5)
        tr.prog(base, 0, prog)
        tr.note(base + t(0.1), t(1.5), 0, 60, 100)
    return write("01_programs.mid", tr)


def p02_keyrange():
    """Every key on three patches. Isolates region splits and pitch scaling."""
    tr = Track()
    clock = 0
    for prog in (0, 48, 73):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for key in range(128):
            tr.note(clock, t(0.35), 0, key, 100)
            clock += t(0.6)
        clock += t(1.0)
    return write("02_keyrange.mid", tr)


def p03_velocity():
    """Velocity sweep. Isolates velocity->amplitude/filter curves."""
    tr = Track()
    clock = 0
    for prog in (0, 48):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for vel in range(1, 128, 2):
            tr.note(clock, t(0.5), 0, 60, vel)
            clock += t(0.9)
        clock += t(1.0)
    return write("03_velocity.mid", tr)


def p04_envelope():
    """Note-off at many points. Isolates attack/decay/sustain/release stages."""
    tr = Track()
    clock = 0
    for prog in (0, 48, 73):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for hold in (0.01, 0.03, 0.08, 0.15, 0.3, 0.6, 1.2, 2.5, 5.0):
            tr.note(clock, t(hold), 0, 60, 100)
            clock += t(hold + 4.0)   # long gap: capture the full release tail
        clock += t(1.0)
    return write("04_envelope.mid", tr)


def p05_pitchbend():
    """Bend sweeps at three bend ranges. Isolates pitch math and RPN handling."""
    tr = Track()
    clock = 0
    for rng in (2, 12, 24):
        tr.rpn(clock, 0, 0, 0, rng)
        clock += t(0.2)
        tr.note(clock, t(6.0), 0, 60, 100)
        for i in range(121):
            tr.bend(clock + t(i * 0.05), 0, int(i * 16383 / 120))
        clock += t(7.0)
        tr.bend(clock, 0, 8192)
    return write("05_pitchbend.mid", tr)


def p06_modwheel():
    """CC1 depth steps on a held note. Isolates LFO rate/depth/shape."""
    tr = Track()
    clock = 0
    for prog in (48, 73):
        tr.prog(clock, 0, prog)
        for val in (0, 32, 64, 96, 127):
            tr.cc(clock, 0, 1, val)
            tr.note(clock + t(0.1), t(4.0), 0, 60, 100)
            clock += t(5.5)
        tr.cc(clock, 0, 1, 0)
    return write("06_modwheel.mid", tr)


def p07_pan_volume():
    """CC10/CC7/CC11 steps. Isolates pan law and the volume->gain curve."""
    tr = Track()
    clock = 0
    for cc_num in (10, 7, 11):
        for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
            tr.cc(clock, 0, 7, 100)
            tr.cc(clock, 0, 11, 127)
            tr.cc(clock, 0, 10, 64)
            tr.cc(clock, 0, cc_num, val)
            tr.note(clock + t(0.05), t(1.5), 0, 60, 100)
            clock += t(2.5)
        clock += t(1.0)
    return write("07_pan_volume.mid", tr)


def p08_reverb():
    """CC91 steps, short note, long tail. Isolates the reverb unit alone."""
    tr = Track()
    clock = 0
    tr.cc(clock, 0, 93, 0)   # chorus off, so only reverb moves
    for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        tr.cc(clock, 0, 91, val)
        tr.note(clock + t(0.05), t(0.2), 0, 60, 100)
        clock += t(6.0)      # long gap: the whole tail must decay into silence
    return write("08_reverb.mid", tr)


def p09_chorus():
    """CC93 steps with reverb off. Isolates the chorus unit alone."""
    tr = Track()
    clock = 0
    tr.cc(clock, 0, 91, 0)
    tr.prog(clock, 0, 48)
    for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        tr.cc(clock, 0, 93, val)
        tr.note(clock + t(0.05), t(3.0), 0, 60, 100)
        clock += t(5.0)
    return write("09_chorus.mid", tr)


def p10_polyphony():
    """Stack notes past the voice limit. Isolates the voice-stealing rule."""
    tr = Track()
    clock = t(0.2)
    # ramp on: one note every 60ms, 64 of them, all sustained
    for i in range(64):
        tr.at(clock + t(i * 0.06), [0x90, 24 + i, 100])
    end = clock + t(64 * 0.06) + t(4.0)
    for i in range(64):
        tr.at(end, [0x80, 24 + i, 0])
    # second pass across all 16 channels at once
    clock = end + t(3.0)
    for ch in range(16):
        if ch == 9:
            continue
        for n in range(6):
            tr.note(clock + t(ch * 0.05), t(3.0), ch, 48 + n * 4, 100)
    return write("10_polyphony.mid", tr)


def p11_drums():
    """Every drum note across seven kits on ch10. Isolates the drum key map."""
    tr = Track()
    clock = 0
    # all nine kits gm.dls actually defines — 48 (Orchestra) and 56 (SFX) were
    # missing from the first cut, and SFX has a different region count (46 vs 61)
    for kit in (0, 8, 16, 24, 25, 32, 40, 48, 56):
        tr.prog(clock, 9, kit)
        clock += t(0.2)
        for key in range(27, 88):
            tr.note(clock, t(0.3), 9, key, 100)
            clock += t(0.6)
        clock += t(1.0)
    return write("11_drums.mid", tr)


def p12_gs_sysex():
    """GS reset and part params. Isolates GS-specific state handling."""
    tr = Track()
    clock = 0
    tr.sysex(clock, [0x7E, 0x7F, 0x09, 0x01, 0xF7])                    # GM reset
    clock += t(1.0)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    tr.sysex(clock, [0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F,
                     0x00, 0x41, 0xF7])                                # GS reset
    clock += t(1.0)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    # master volume, then master pan, then ch1 -> drum part
    tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, 0x40, 0xF7])
    clock += t(0.5)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    tr.sysex(clock, [0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15,
                     0x02, 0x18, 0xF7])                                # ch2=drums
    clock += t(0.5)
    tr.note(clock, t(1.0), 1, 38, 100)
    return write("12_gs_sysex.mid", tr)


def p13_edge():
    """Parser and state edge cases. Isolates event handling, not the DSP."""
    tr = Track()
    clock = t(0.2)
    # note-on with velocity 0 must act as note-off
    tr.at(clock, [0x90, 60, 100])
    tr.at(clock + t(1.0), [0x90, 60, 0])
    clock += t(2.5)
    # sustain pedal holds through note-off
    tr.cc(clock, 0, 64, 127)
    tr.note(clock + t(0.1), t(0.3), 0, 62, 100)
    tr.cc(clock + t(2.0), 0, 64, 0)
    clock += t(4.0)
    # all-sound-off (120) vs all-notes-off (123): 120 must cut the tail dead
    tr.note(clock, t(4.0), 0, 64, 100)
    tr.cc(clock + t(1.0), 0, 120, 0)
    clock += t(3.0)
    tr.note(clock, t(4.0), 0, 65, 100)
    tr.cc(clock + t(1.0), 0, 123, 0)
    clock += t(3.0)
    # retrigger the same key without an intervening note-off
    tr.at(clock, [0x90, 67, 100])
    tr.at(clock + t(0.3), [0x90, 67, 100])
    tr.at(clock + t(0.6), [0x90, 67, 100])
    tr.at(clock + t(2.0), [0x80, 67, 0])
    clock += t(3.5)
    # reset-all-controllers must revert bend and modwheel
    tr.cc(clock, 0, 1, 127)
    tr.bend(clock, 0, 16383)
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    tr.cc(clock + t(1.5), 0, 121, 0)
    tr.note(clock + t(2.0), t(1.0), 0, 60, 100)
    return write("13_edge.mid", tr)


def p14_running_status():
    """Hand-built track using running status. Nothing else exercises it."""
    body = bytearray()
    body += b"\x00\xff\x51\x03" + struct.pack(">I", TEMPO)[1:]
    body += b"\x00\x90\x3c\x64"                      # note on, status byte given
    body += varlen(t(0.5)) + b"\x3e\x64"             # running status: note on
    body += varlen(t(0.5)) + b"\x40\x64"
    body += varlen(t(1.0)) + b"\x3c\x00"             # running status: vel-0 off
    body += varlen(t(0.2)) + b"\x3e\x00"
    body += varlen(t(0.2)) + b"\x40\x00"
    body += varlen(t(1.0)) + b"\xff\x2f\x00"
    return write("14_running_status.mid", None,
                 b"MTrk" + struct.pack(">I", len(body)) + bytes(body))


def gs(addr, data):
    """Roland GS SysEx payload with checksum. addr/data are byte lists."""
    body = list(addr) + list(data)
    csum = (128 - (sum(body) % 128)) % 128
    return [0x41, 0x10, 0x42, 0x12] + body + [csum, 0xF7]


GS_RESET = gs([0x40, 0x00, 0x7F], [0x00])


def read_dls_inventory(path):
    """Enumerate (bank_msb, bank_lsb, program, is_drum, name) from a DLS file.

    Guessing which banks exist wastes render time on silent fallbacks and, worse,
    misses the ones that do (the first cut of this probe tested banks 0/8/16/24/32
    and missed "Square" at bank 1 entirely). Read the collection instead.
    """
    d = open(path, "rb").read()
    assert d[:4] == b"RIFF" and d[8:12] == b"DLS ", "not a DLS file"

    def walk(off, end):
        out, i = [], off
        while i + 8 <= end:
            cid = d[i:i + 4]
            sz = struct.unpack("<I", d[i + 4:i + 8])[0]
            body = i + 8
            if cid in (b"LIST", b"RIFF"):
                out += walk(body + 4, body + sz)
            else:
                out.append((cid, body, sz))
            i = body + sz + (sz & 1)
        return out

    insts, cur = [], None
    for cid, body, sz in walk(12, len(d)):
        if cid == b"insh":
            _, bank, prog = struct.unpack("<III", d[body:body + 12])
            cur = ((bank >> 8) & 0x7F, bank & 0x7F, prog, bool(bank & 0x80000000))
        elif cid == b"INAM" and cur is not None:
            name = d[body:body + sz].split(b"\x00")[0].decode("latin-1").strip()
            insts.append(cur + (name,))
            cur = None
    return insts


def p15_banks():
    """Bank select: one representative tone per bank, plus the fallback rule.

    The full (bank, program) -> tone map is already recoverable from gm.dls, so
    rendering all 98 variation tones would mostly re-measure known data. What the
    collection does NOT tell us is what happens on a request for a bank that
    doesn't exist for that program: drop to bank 0, go silent, or keep the
    previous patch. That's what this probe spends its time on.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    inv = read_dls_inventory(os.path.join(here, os.pardir, "dist", "gm.dls"))
    melodic = [x for x in inv if not x[3]]
    present = {(x[0], x[1], x[2]) for x in melodic}

    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)
    manifest = ["# 15_banks.mid — expected tone per note onset",
                "# onset_seconds\tCC0\tCC32\tprogram\texpected"]

    def play(msb, lsb, prog, expect):
        nonlocal clock
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, lsb)
        tr.prog(clock + t(0.02), 0, prog)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        manifest.append(f"{clock / TPS + 0.1:.3f}\t{msb}\t{lsb}\t{prog}\t{expect}")
        clock += t(1.8)

    # A: one representative tone per non-zero bank, each against its bank-0 twin
    for msb in sorted({x[0] for x in melodic if x[0]}):
        cand = sorted([x for x in melodic if x[0] == msb], key=lambda x: x[2])[0]
        play(0, 0, cand[2], "<bank 0 reference>")
        play(msb, 0, cand[2], cand[4])

    # B: the lead variations, since these are the ones by ear
    for prog in (80, 81):
        for msb in (0, 1, 8):
            nm = next((x[4] for x in melodic if x[:3] == (msb, 0, prog)), "MISSING")
            play(msb, 0, prog, nm)

    # C: the actual unknown — fallback for combinations that don't exist
    for msb, prog in ((1, 40), (8, 40), (16, 0), (24, 0), (32, 40), (9, 80)):
        if (msb, 0, prog) not in present:
            play(msb, 0, prog, f"MISSING (bank {msb} undefined for prog {prog})")
    for msb in (10, 33, 64, 127):
        play(msb, 0, 0, f"MISSING (bank {msb} undefined everywhere)")
    for lsb in (1, 127):
        play(0, lsb, 0, f"LSB {lsb} (collection defines LSB=0 only)")
    # does a failed bank select leave the previous patch in place?
    play(0, 0, 19, "Church Organ (bank 0, establishes a known patch)")
    play(64, 0, 19, "MISSING bank on same prog — does Church Organ persist?")

    path = write("15_banks.mid", tr)
    with open(os.path.join(PROBE_DIR, "15_banks.manifest.tsv"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    return path


def p16_drum_parts():
    """Arbitrary drum parts via GS "Use For Rhythm Part".

    The GS block number does NOT equal the MIDI channel: block 0 is channel 10,
    blocks 1-9 are channels 1-9, blocks A-F are channels 11-16. Probe 12 got
    this wrong (it set block 1 = channel 1, then played on channel 2), so its
    fourth note is a pitched fallback, not a drum. This probe pins the mapping
    down by playing the same key on both candidate channels after each switch.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    def probe_pair(label_ch_a, label_ch_b):
        nonlocal clock
        tr.note(clock, t(0.8), label_ch_a, 38, 100)
        clock += t(1.5)
        tr.note(clock, t(0.8), label_ch_b, 38, 100)
        clock += t(1.5)

    # baseline: neither ch1 nor ch2 is a drum part yet
    probe_pair(0, 1)
    # block 1 -> should make MIDI channel 1 (index 0) a drum part
    tr.sysex(clock, gs([0x40, 0x11, 0x15], [0x02]))
    clock += t(0.5)
    probe_pair(0, 1)
    # block 2 -> should make MIDI channel 2 (index 1) a drum part
    tr.sysex(clock, gs([0x40, 0x12, 0x15], [0x02]))
    clock += t(0.5)
    probe_pair(0, 1)
    # block 0 -> channel 10: turn the standard drum part MELODIC
    tr.note(clock, t(0.8), 9, 38, 100)        # drums, before
    clock += t(1.5)
    tr.sysex(clock, gs([0x40, 0x10, 0x15], [0x00]))
    clock += t(0.5)
    tr.note(clock, t(0.8), 9, 38, 100)        # should now be pitched
    clock += t(1.5)
    tr.prog(clock, 9, 80)                     # and should accept a program
    tr.note(clock + t(0.1), t(0.8), 9, 60, 100)
    clock += t(1.5)
    # drum kit variation on a re-assigned part
    tr.sysex(clock, gs([0x40, 0x12, 0x15], [0x02]))
    clock += t(0.5)
    for kit in (0, 8, 16, 24, 25, 32, 40):
        tr.prog(clock, 1, kit)
        tr.note(clock + t(0.05), t(0.6), 1, 38, 100)
        clock += t(1.2)
    return write("16_drum_parts.mid", tr)


def p17_master_volume():
    """GM master volume sweep. Probe 12 showed 50% -> 0.2529 amplitude, i.e.
    the value is applied SQUARED. This maps the whole curve to confirm it."""
    tr = Track()
    clock = t(0.5)
    for msb in (0x7F, 0x60, 0x40, 0x30, 0x20, 0x10, 0x08, 0x00):
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, msb, 0xF7])
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        clock += t(2.0)
    # then the same sweep on CC7, to see whether channel volume uses that curve
    tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, 0x7F, 0xF7])
    clock += t(0.3)
    for v in (127, 96, 64, 48, 32, 16, 8, 0):
        tr.cc(clock, 0, 7, v)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        clock += t(2.0)
    return write("17_master_volume.mid", tr)


def p18_key_groups():
    """Exclusive key groups (hi-hat choke and friends).

    The Standard kit defines seven groups: 1 = closed/pedal/open hat, 2 = whistles,
    3 = guiros, 4 = cuicas, 5 = triangles, 6 = surdos, 7 = scratches. A note in a
    group must cut any sounding note in the same group. Open hat is LOOPED with a
    ~4s release, so an unchoked open hat rings for seconds and a choke is obvious.
    Probe 11 sweeps keys in ascending order, so it plays 42 before 46 and never
    triggers a single choke — none of this is otherwise tested.

    The gap between the two notes must be SHORT ENOUGH that A is still sounding.
    The first cut of this probe used a flat 2.0s gap and proved nothing: by then
    even the open hat had decayed ~51dB and there was nothing left to cut. The
    gaps below come from the region data — looped keys (46, 81, 87) ring for
    seconds, but the unlooped one-shots last only as long as their sample:
    long whistle 0.354s, long guiro 0.199s, open cuica 0.112s, scratch 0.083s.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    notes = ["# 18_key_groups.mid",
             "# first_onset\tgap_s\tkey_a\tkey_b\texpected"]

    def choke(first, second, gap, label):
        nonlocal clock
        t0 = clock
        tr.note(clock, t(0.05), 9, first, 100)
        tr.note(clock + t(gap), t(0.05), 9, second, 100)
        clock += t(3.0)
        notes.append(f"{t0 / TPS:.3f}\t{gap}\t{first}\t{second}\t{label}")

    # group 1, open hat rings ~4s — sweep the gap to see WHEN the cut applies
    for gap in (0.15, 0.30, 0.80):
        choke(46, 42, gap, f"gap {gap}s: OPEN HAT cut by closed hat (grp 1)")
    choke(46, 44, 0.30, "OPEN HAT cut by pedal hat (grp 1)")
    choke(46, 46, 0.30, "OPEN HAT retriggered by itself (grp 1)")
    # groups 5 and 6: looped, release 2.355s and 3.367s
    choke(81, 80, 0.30, "OPEN TRIANGLE cut by mute triangle (grp 5)")
    choke(87, 86, 0.30, "OPEN SURDO cut by mute surdo (grp 6)")
    # unlooped groups: gap must beat the sample length or there is nothing to cut
    choke(72, 71, 0.15, "LONG WHISTLE (0.354s sample) cut by short whistle (grp 2)")
    choke(74, 73, 0.08, "LONG GUIRO (0.199s sample) cut by short guiro (grp 3)")
    choke(79, 78, 0.05, "OPEN CUICA (0.112s sample) cut by mute cuica (grp 4)")
    choke(30, 29, 0.04, "SCRATCH PULL (0.083s sample) cut by push (grp 7)")
    # controls: no shared group, must NOT cut
    choke(46, 38, 0.30, "open hat + SNARE — different group, must NOT cut")
    choke(49, 57, 0.30, "crash 1 + crash 2 — no key group, must NOT cut")
    choke(51, 49, 0.30, "ride + crash — no key group, must NOT cut")
    # baselines: A alone, so the choked cases have something to be compared against
    for key in (46, 81, 87, 72):
        tr.note(clock, t(0.05), 9, key, 100)
        notes.append(f"{clock / TPS:.3f}\t-\t{key}\t-\tBASELINE: {key} alone, uncut")
        clock += t(3.0)

    path = write("18_key_groups.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "18_key_groups.manifest.tsv"), "w") as f:
        f.write("\n".join(notes) + "\n")
    return path


def p19_prior_art():
    """Closes out the remaining MEASURABLE claims from `msgs research/`.

    Everything else in the prior art needs the disassembly (interpolation method,
    polyphony cap). These four are settleable by ear/measurement, and each one is
    a specific published assertion rather than a general mechanism.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)
    man = ["# 19_prior_art.mid", "# onset\tsection\texpected"]

    def mark(t0, sec, txt):
        man.append(f"{t0 / TPS:.3f}\t{sec}\t{txt}")

    # A. Prior analysis: "CC#120 ... still ignores hold pedal and releases notes",
    #    whereas CC#123 should NOT release notes held by sustain.
    for cc, claim in ((120, "should release DESPITE sustain held"),
                      (123, "should NOT release, sustain holds it")):
        tr.cc(clock, 0, 64, 127)                  # sustain down first
        tr.note(clock + t(0.1), t(0.3), 0, 60, 100)
        tr.cc(clock + t(1.5), 0, cc, 0)           # while sustain still held
        mark(clock, "A", f"CC{cc} at +1.5s with sustain HELD: {claim}")
        tr.cc(clock + t(4.0), 0, 64, 0)           # lift pedal at +4.0
        clock += t(6.0)

    # B. Prior analysis: "MSGS ignores portamento and soft pedal controls"
    tr.cc(clock, 0, 5, 64)                        # portamento time
    tr.cc(clock, 0, 65, 127)                      # portamento ON
    tr.note(clock + t(0.1), t(1.0), 0, 48, 100)
    tr.note(clock + t(1.3), t(1.5), 0, 72, 100)   # big leap: glide or jump?
    mark(clock, "B", "portamento ON, C3->C5 leap: glide would be audible")
    clock += t(4.0)
    tr.cc(clock, 0, 65, 0)
    tr.note(clock + t(0.1), t(1.0), 0, 48, 100)
    tr.note(clock + t(1.3), t(1.5), 0, 72, 100)
    mark(clock, "B", "portamento OFF, same leap: reference for the above")
    clock += t(4.0)
    for soft in (0, 127):
        tr.cc(clock, 0, 67, soft)                 # soft pedal
        tr.note(clock + t(0.1), t(1.5), 0, 60, 100)
        mark(clock, "B", f"soft pedal CC67={soft}: claim is that it is ignored")
        clock += t(3.0)
    tr.cc(clock, 0, 67, 0)

    # C. Prior analysis describes Capital Tone Fallback as "closest available tone,
    #    rounded down". Program 80 has banks 0, 1, 8 only. Requesting bank 9:
    #    round-down predicts bank 8 (Sine Wave), plain fallback predicts bank 0
    #    (Square Wave). Play the candidates ADJACENT so alignment drift between
    #    distant notes cannot confound the comparison (which is what made the
    #    first attempt at this inconclusive).
    for msb, note in ((0, "bank 0 = Square Wave"), (8, "bank 8 = Sine Wave"),
                      (9, "bank 9 UNDEFINED -> which one?"),
                      (0, "bank 0 again"), (9, "bank 9 again"),
                      (8, "bank 8 again"), (9, "bank 9 again")):
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.02), 0, 80)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        mark(clock, "C", note)
        clock += t(1.6)

    # D. same, on a program whose only banks are 0 and 8, to separate
    #    "round down to nearest defined" from "always bank 0"
    for msb, note in ((0, "prog 0 bank 0 = Piano 1"), (8, "prog 0 bank 8"),
                      (16, "prog 0 bank 16"), (9, "prog 0 bank 9 UNDEFINED"),
                      (17, "prog 0 bank 17 UNDEFINED"),
                      (1, "prog 0 bank 1 UNDEFINED")):
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.02), 0, 0)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        mark(clock, "D", note)
        clock += t(1.6)

    path = write("19_prior_art.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "19_prior_art.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p20_voice_count():
    """Find the exact polyphony cap by energy plateau.

    Probe 10 could only establish >=64 because it used program 0: a decaying
    piano makes a stolen voice look like ordinary decay, and its second section
    reused the same six keys on every channel so voices could not be counted at
    all.

    This uses program 80 bank 8, the GS "Sine Wave" — measured at h2 -45dB and
    everything above -53dB, i.e. a genuinely pure tone, with a flat sustain
    (+-2% over a second) and an instant attack. One voice therefore contributes
    exactly ONE countable spectral peak, so voices are counted directly rather
    than inferred from an energy plateau.

    Keys 48-127 only: at 130Hz a semitone is ~7.8Hz, which a 32768-point FFT
    resolves comfortably. Going lower would pack peaks below the bin width and
    make them uncountable, which is exactly the register that gets stolen first.

    Velocity 40, not 100: 80 sines at full velocity would sum past full scale,
    and clipping would spray intermodulation products across the spectrum and
    destroy the peak count.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)          # bank select needs GS mode
    man = ["# 20_voice_count.mid", "# onset\tsection\tnote_index\tkey\tchannel"]
    KEYS = list(range(48, 128))    # 80 distinct, all FFT-resolvable
    VEL = 40

    def sine_on(clock, ch):
        tr.cc(clock, ch, 0, 8)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, 80)

    # A: 80 distinct sustained sines on ONE channel, one every 100ms
    clock = t(1.0)
    sine_on(clock, 0)
    clock += t(0.2)
    for i, key in enumerate(KEYS):
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tA\t{i + 1}\t{key}\t0")
    end_a = clock + t(len(KEYS) * 0.1) + t(3.0)
    for key in KEYS:
        tr.at(end_a, [0x80, key, 0])

    # B: the same 80 voices spread over 8 channels — global pool or per-channel?
    clock = end_a + t(4.0)
    for ch in range(8):
        sine_on(clock, ch)
    clock += t(0.2)
    for i, key in enumerate(KEYS):
        ch = i % 8
        tr.at(clock + t(i * 0.1), [0x90 | ch, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tB\t{i + 1}\t{key}\t{ch}")
    end_b = clock + t(len(KEYS) * 0.1) + t(3.0)
    for i, key in enumerate(KEYS):
        tr.at(end_b, [0x80 | (i % 8), key, 0])

    path = write("20_voice_count.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "20_voice_count.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p21_steal_policy():
    """Which voice gets stolen — oldest, or lowest?

    Probe 20 established a 48-voice global cap, but it ramped UPWARD in pitch,
    so "the 32 oldest notes" and "the 32 lowest notes" were the same set and the
    policy was unidentifiable. This ramps DOWNWARD from key 127:

      oldest-first  -> the LAST 48 started survive, i.e. the LOW keys  (48-95)
      lowest-first  -> the HIGH keys survive, i.e. the same set as probe 20
      quietest-first-> survivors scattered by level, not contiguous

    Section B holds one long note through the whole ramp to see whether a voice
    that has been sounding longest is protected or is the first to go.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 21_steal_policy.mid", "# onset\tsection\tnote_index\tkey"]
    KEYS_DOWN = list(range(127, 47, -1))     # 80 notes, high to low
    VEL = 40

    def sine_on(clock, ch):
        tr.cc(clock, ch, 0, 8)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, 80)

    # A: descending ramp
    clock = t(1.0)
    sine_on(clock, 0)
    clock += t(0.2)
    for i, key in enumerate(KEYS_DOWN):
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tA\t{i + 1}\t{key}")
    end_a = clock + t(len(KEYS_DOWN) * 0.1) + t(3.0)
    for key in KEYS_DOWN:
        tr.at(end_a, [0x80, key, 0])

    # B: one sentinel note held from before the ramp — protected, or first out?
    clock = end_a + t(4.0)
    sine_on(clock, 0)
    clock += t(0.2)
    tr.at(clock, [0x90, 60, VEL])            # sentinel, oldest of all
    man.append(f"{clock / TPS:.3f}\tB\t0\t60 SENTINEL (started first)")
    clock += t(1.0)
    for i, key in enumerate(range(61, 128)):  # 67 more notes on top
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tB\t{i + 1}\t{key}")
    end_b = clock + t(67 * 0.1) + t(3.0)
    for key in range(60, 128):
        tr.at(end_b, [0x80, key, 0])

    path = write("21_steal_policy.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "21_steal_policy.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p22_no_gs_reset():
    """Is bank select gated on a prior GS Reset?

    Prior adjudication open question #2. Every existing bank probe (12, 15, 16, 18, 19)
    sends a GS Reset first, so the "bank select requires GS mode" clause has
    never been tested in isolation. The disassembly gives strong circumstantial
    support — CC0 (0x1341e) and CC32 (0x13492) are both gated on a flag at
    [synth+0xf54], written only inside the SysEx handler (0x136f5 / 0x137f4) —
    but the flag's meaning is unconfirmed; it could be a parse-position counter.

    Section A sends NO GS Reset at all. Section B repeats it verbatim after one.
    Same file, so the two are directly comparable; any tone that differs between
    A and B was gated.
    """
    tr = Track()
    man = ["# 22_no_gs_reset.mid", "# onset\tsection\tCC0\tprogram\tchannel\texpected"]
    clock = t(1.0)

    def trial(sec, msb, prog, ch, note, label):
        nonlocal clock
        tr.cc(clock, ch, 0, msb)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, prog)
        tr.note(clock + t(0.1), t(1.0), ch, note, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\t{sec}\t{msb}\t{prog}\t{ch}\t{label}")
        clock += t(1.8)

    def battery(sec):
        # bank 0 first as the in-section reference
        trial(sec, 0, 80, 0, 60, "prog 80 bank 0 = Square Wave (reference)")
        trial(sec, 1, 80, 0, 60, "prog 80 bank 1 = Square — gated?")
        trial(sec, 8, 80, 0, 60, "prog 80 bank 8 = Sine Wave — gated?")
        trial(sec, 16, 0, 0, 60, "prog 0 bank 16 = Piano 1d — gated?")
        # Prior analysis claims the extra drum kits are ALWAYS available, GS or not
        trial(sec, 0, 0, 9, 38, "ch10 kit 0 Standard (reference)")
        trial(sec, 0, 8, 9, 38, "ch10 kit 8 Room — claimed available without GS")
        trial(sec, 0, 25, 9, 38, "ch10 kit 25 TR-808 — claimed available without GS")

    battery("A-no-reset")
    tr.sysex(clock, GS_RESET)
    man.append(f"{clock / TPS:.3f}\t--\t-\t-\t-\tGS RESET sent here")
    clock += t(1.5)
    battery("B-after-reset")

    path = write("22_no_gs_reset.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "22_no_gs_reset.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p23_rpn_tune():
    """RPN master coarse (0,2) and fine (0,1) tune — the gap probe 05 left.

    Probe 05 covers RPN (0,0) bend range. Nothing tests master tune, which is
    exactly what GENERAL SERUM uses (126 coarse, 42 fine) and where the octave
    bug hid. Open questions this settles about the real synth: does MSGS honor
    coarse tune at all? What range does it clamp to? Does master tune reach an
    already-sounding note, or only notes started after it? All on channel 0,
    note 60 (C4, 261.6 Hz) as the fixed reference pitch.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 23_rpn_tune.mid", "# onset\tsection\tRPN\tdata\texpected"]

    def rpn(msb, lsb, data):
        tr.cc(clock, 0, 101, msb)
        tr.cc(clock, 0, 100, lsb)
        tr.cc(clock, 0, 6, data)

    def ref_note(sec, tag):
        nonlocal clock
        tr.note(clock + t(0.05), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.05:.3f}\t{sec}\t-\t-\t{tag}")
        clock += t(1.6)

    # A. coarse tune sweep: RPN(0,2) data = 64 + semitones. Play C4 each time;
    #    it should transpose by (data-64) semitones if honored.
    ref_note("A", "baseline C4, no tune")
    for data, semis in ((64 - 24, -24), (64 - 12, -12), (64 - 5, -5),
                        (64 + 7, +7), (64 + 12, +12), (64 + 24, +24), (64, 0)):
        rpn(0, 2, data)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tA\t0,2\t{data}\tC4 {semis:+d} semitones")
        clock += t(1.6)

    # B. fine tune sweep: RPN(0,1) MSB = data. 64 = center; each step ~ up to
    #    +-100 cents across the 14-bit range (LSB left 0). Small pitch shifts.
    for data in (64, 96, 127, 32, 0):
        rpn(0, 1, data)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tB\t0,1\t{data}\tfine tune, C4 shifted a few cents")
        clock += t(1.6)
    rpn(0, 1, 64)  # recenter fine

    # C. coarse + fine together, then RPN-null (7F,7F) followed by a stray
    #    data-entry that MUST be ignored (does MSGS honor the null select?).
    rpn(0, 2, 64 + 12)
    rpn(0, 1, 96)
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tC\t0,2+0,1\t76,96\tC4 +12 semi + fine")
    clock += t(1.6)
    tr.cc(clock, 0, 101, 0x7F)
    tr.cc(clock, 0, 100, 0x7F)          # RPN null
    tr.cc(clock, 0, 6, 0x00)            # stray data entry — should be ignored
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tC\tnull\t0\tC4 unchanged (null must ignore data entry)")
    clock += t(1.6)

    # D. does master tune reach a HELD note? Start C4, then apply coarse +12
    #    while it sounds. Real-time (like pitch bend) or note-on-only?
    rpn(0, 2, 64)                       # recenter first
    clock += t(0.2)
    tr.at(clock, [0x90, 60, 100])
    man.append(f"{clock / TPS:.3f}\tD\t-\t-\tC4 held; coarse +12 applied at +1.5s")
    tr.cc(clock + t(1.5), 0, 101, 0)
    tr.cc(clock + t(1.5), 0, 100, 2)
    tr.cc(clock + t(1.5), 0, 6, 64 + 12)
    tr.at(clock + t(3.0), [0x80, 60, 0])
    clock += t(4.0)

    path = write("23_rpn_tune.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "23_rpn_tune.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p24_gain_staging():
    """The gain chain: master vol / CC7 / CC11 / velocity / sample attenuation,
    and — the untested part — whether they SUM in dB.

    The individual curves are covered (probe 03 velocity, 17 master+CC7). What
    no probe checks is the interaction: does velocity 64 + CC7 64 give the sum
    of their individual attenuations, or does something clamp/saturate/apply
    master differently? Each control at ~half is ~-12 dB (squared law); the
    minimal test is a few singles to anchor, then stacks to test additivity.

    Patch bank 1 program 80 ("Square") — a clean, steady, looped tone so RMS is
    a reliable gain proxy. Needs a GS Reset for the bank-1 select to take.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 24_gain_staging.mid",
           "# onset\tvel\tCC7\tCC11\tmasterMSB\texpected_dB_vs_baseline"]

    def master(msb):
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, msb, 0xF7])

    def note(vel, cc7, cc11, mmsb, exp):
        nonlocal clock
        tr.cc(clock, 0, 0, 1)              # bank 1
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, 80)   # Square
        master(mmsb)
        tr.cc(clock, 0, 7, cc7)
        tr.cc(clock, 0, 11, cc11)
        tr.note(clock + t(0.05), t(0.5), 0, 60, vel)
        man.append(f"{clock / TPS + 0.05:.3f}\t{vel}\t{cc7}\t{cc11}\t{mmsb}\t{exp}")
        clock += t(0.9)

    # baseline: everything at max (0 dB attenuation reference)
    note(127, 127, 127, 127, "0.0 (baseline)")
    # singles at ~half — each should be ~-12 dB (squared law)
    note(64, 127, 127, 127, "-11.9 (velocity alone)")
    note(127, 64, 127, 127, "-11.9 (CC7 alone)")
    note(127, 127, 64, 127, "-11.9 (CC11 alone)")
    note(127, 127, 127, 64, "-12.0 (master alone)")
    # stacks — test additivity in dB
    note(64, 64, 127, 127, "-23.8 (vel+CC7, if additive)")
    note(64, 64, 64, 127, "-35.7 (vel+CC7+CC11)")
    note(64, 64, 64, 64, "-47.7 (full stack, all four)")
    # a milder stack to confirm additivity away from the half point
    note(96, 96, 127, 127, "-9.0 (vel96+CC7 96, ~-4.5 each)")
    # restore, then a plain full-scale note to bookend/confirm no drift
    note(127, 127, 127, 127, "0.0 (bookend, must match baseline)")

    path = write("24_gain_staging.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "24_gain_staging.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p25_pan_law():
    """Pan (CC10) law — measure L and R separately across the full sweep.

    The finicky questions: is it constant-power (center = -3 dB per side) or
    linear (center = -6 dB per side) or something else? Does hard-left actually
    silence R? Is CC10=64 the exact center? Sine patch (bank 8 prog 80) so each
    channel's RMS is a clean gain proxy. Everything else pinned at max.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 25_pan_law.mid", "# onset\tCC10\texpected"]

    def note(cc10, tag):
        nonlocal clock
        tr.cc(clock, 0, 0, 8)             # bank 8 = Sine Wave
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, 80)
        tr.cc(clock, 0, 7, 127)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, cc10)
        tr.note(clock + t(0.05), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.05:.3f}\t{cc10}\t{tag}")
        clock += t(1.0)

    for cc10 in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        note(cc10, f"CC10={cc10}: L/R balance")
    return _write_manifest("25_pan_law.mid", tr, man)


def p26_other_gains():
    """The gain terms probe 24 didn't cover: GS part-level PAN (SysEx, separate
    from CC10) and the per-patch sample attenuation (absolute level).

    - Section A: does GS 'part pan' (SysEx 41 10 42 12 40 1p 1C vv) exist and
      stack with CC10? Roland GS carries a part pan independent of CC10.
    - Section B: play several patches at IDENTICAL velocity/CC7/master and
      measure absolute level. Different patches have different wsmp/art1
      attenuations, so this tests whether the per-patch sample volume is
      applied at the right absolute scale (the units question from probe 24).
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 26_other_gains.mid", "# onset\tsection\tdetail"]

    def sine():
        tr.cc(clock, 0, 0, 8); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, 80)
        tr.cc(clock, 0, 7, 127); tr.cc(clock, 0, 11, 127); tr.cc(clock, 0, 10, 64)

    # A: GS part pan (address 40 10 1C, part 1) — 0x00 left .. 0x40 center .. 0x7F right
    for pan, tag in ((0x40, "GS part pan CENTER (0x40)"), (0x00, "GS part pan LEFT (0x00)"),
                     (0x7F, "GS part pan RIGHT (0x7F)"), (0x40, "back to center")):
        sine()
        tr.sysex(clock, gs([0x40, 0x10, 0x1C], [pan]))
        tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tA\t{tag}")
        clock += t(1.0)
    # combine GS part pan with CC10 — do they stack?
    sine()
    tr.sysex(clock, gs([0x40, 0x10, 0x1C], [0x00]))   # GS pan hard left
    tr.cc(clock, 0, 10, 127)                           # CC10 hard right
    tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tA\tGS pan LEFT + CC10 RIGHT (cancel? stack?)")
    clock += t(1.0)

    # B: per-patch absolute level — same vel/vol, different patches.
    # bank 0: 0 Piano, 40 Violin, 48 Strings, 56 Trumpet, 80 SquareLead, 118 Melodic Tom
    tr.sysex(clock, GS_RESET); clock += t(0.3)
    for prog in (0, 40, 48, 56, 80, 118):
        tr.cc(clock, 0, 0, 0); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, prog)
        tr.cc(clock, 0, 7, 127); tr.cc(clock, 0, 11, 127); tr.cc(clock, 0, 10, 64)
        tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tB\tprog {prog} at vel100/CC7-127 (absolute level)")
        clock += t(1.0)
    return _write_manifest("26_other_gains.mid", tr, man)


def _write_manifest(name, tr, man):
    path = write(name, tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, name.replace(".mid", ".manifest.tsv")), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p27_gain_curves():
    """Rigorous version of probe 24: prove the four controls use the SAME curve
    and compose additively — WITHOUT the coincidence trap.

    Probe 24 tested every control at the same value (64), so four different
    curves that merely cross at 64 would look identical and additive. This fixes
    both holes:
      A. sweep each of vel / CC7 / CC11 / master over the SAME distinguishing
         values (8, 20, 40, 64, 90, 110) and compare the four curves point by
         point. These values maximally separate squared from linear or a table.
      B. combinations of UNEQUAL values (vel 110 + CC7 20, vel 20 + CC7 110,
         three-way asymmetric) so additivity is tested where each term differs.
    If curve_vel(a)+curve_cc7(b) predicts each combo from the section-A curves,
    same-curve + additive is proven; deviation exposes a per-control curve or a
    non-linear compose. Sine (bank 8 prog 80) for clean RMS.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 27_gain_curves.mid", "# onset\tcontrol\tvalue\tvel\tCC7\tCC11\tmasterMSB"]
    VALS = (127, 110, 90, 64, 40, 20, 8)

    def setup():
        tr.cc(clock, 0, 0, 8); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, 80)

    def play(vel, cc7, cc11, mmsb, ctrl, val):
        nonlocal clock
        setup()
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, mmsb, 0xF7])
        tr.cc(clock, 0, 7, cc7); tr.cc(clock, 0, 11, cc11); tr.cc(clock, 0, 10, 64)
        tr.note(clock + t(0.05), t(0.45), 0, 60, vel)
        man.append(f"{clock / TPS + 0.05:.3f}\t{ctrl}\t{val}\t{vel}\t{cc7}\t{cc11}\t{mmsb}")
        clock += t(0.8)

    # A: four curves over identical values
    for v in VALS: play(v, 127, 127, 127, "velocity", v)
    for v in VALS: play(127, v, 127, 127, "CC7", v)
    for v in VALS: play(127, 127, v, 127, "CC11", v)
    for v in VALS: play(127, 127, 127, v, "master", v)
    # B: asymmetric combinations
    for vel, cc7, cc11 in ((110, 20, 127), (20, 110, 127), (90, 40, 127),
                           (40, 90, 127), (110, 40, 20), (64, 90, 110)):
        play(vel, cc7, cc11, 127, "combo", f"v{vel}c{cc7}e{cc11}")
    return _write_manifest("27_gain_curves.mid", tr, man)



def p28_expression_gate():
    """CC11 square-wave gating at several rates. Isolates the expression->gain
    smoothing/attack-release behaviour AT SPEED, which no existing probe does.

    Entry 4's GAIN_SMOOTH_ALPHA (a 12 ms time constant) was fit against slow
    CC7/CC11 glides. GENERAL SERUM measure 226 gates CC11 127<->0 every 12-25
    ms, and the reference goes fully SILENT for 90 ms across a stretch where
    expression alternates 0/127 every 25 ms -- i.e. MSGS does not simply track
    a 50%-duty square. Whatever produces that (an asymmetric rise/fall, a
    slower attack than release, or expression being sampled on a slow control
    cadence) is unmeasured and unrecovered.

    Structure: one held note per section, expression square-waved at a fixed
    half-period. Sections descend from far slower than the smoothing constant
    to far faster, so the response can be read as a function of rate:
    half-period = 200, 100, 50, 25, 12, 6 ms. A final section holds CC11 at 0
    for 300 ms then restores 127, to read the rise and fall edges cleanly and
    separately at a rate where they cannot interact.
    """
    tr = Track()
    man = ["section\tstart_s\tinstrument\thalf_period_ms\tnote"]
    clock = 0
    tr.prog(clock, 0, 80)          # Lead 1 (square) -- steady, loop-sustained
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 10, 64)
    clock += t(0.5)
    for half_ms in (200, 100, 50, 25, 12, 6):
        start = clock
        man.append(f"gate\t{clock / TPS:.3f}\t80\t{half_ms}\t60")
        tr.cc(clock, 0, 11, 127)
        tr.note(clock, t(3.0), 0, 60, 100)
        # square-wave expression across the held note
        half = t(half_ms / 1000.0)
        if half < 1:
            half = 1
        k = 0
        pos = clock + t(0.25)
        while pos < clock + t(2.75):
            tr.cc(pos, 0, 11, 0 if k % 2 == 0 else 127)
            pos += half
            k += 1
        tr.cc(clock + t(2.9), 0, 11, 127)
        clock += t(4.0)
    # clean single fall/rise edge, well separated
    man.append(f"edge\t{clock / TPS:.3f}\t80\t-\t60")
    tr.cc(clock, 0, 11, 127)
    tr.note(clock, t(3.0), 0, 60, 100)
    tr.cc(clock + t(1.0), 0, 11, 0)
    tr.cc(clock + t(1.3), 0, 11, 127)
    clock += t(4.0)
    return _write_manifest("28_expression_gate.mid", tr, man)


def p29_all_sound_off_gap():
    """CC120 (All Sound Off) mid-note at decreasing intervals. Measures how
    fast the driver actually reaches silence, and whether a note-on landing
    shortly after a CC120 starts clean.

    GENERAL SERUM measure 226 issues note-off + CC120 at every group boundary
    and the reference shows hard 25-90 ms silences there; our render shows no
    gap at all, so either our post-CC120 release is far too slow or CC120's
    effect differs from ordinary note-off more than SPEC.md S4.x records.
    SPEC.md marks the choke/fast-release divisor's base quantity [O]
    (Part 5 S5.6/S5.8), so the exact rate is not recoverable from the spec.

    Structure: hold a note, fire CC120, then re-attack after a gap of
    400/200/100/50/25/12 ms. Reading the silence width per section gives the
    real post-CC120 decay directly. A control section repeats the shortest
    gap with an ordinary note-off instead of CC120, so the two paths can be
    compared on identical material (SPEC.md S5.6 says they are distinct
    routines, 0x19a2c vs 0x19aa4).
    """
    tr = Track()
    man = ["section\tstart_s\tmode\tgap_ms\tinstrument\tnote"]
    clock = 0
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    clock += t(0.5)
    for prog, pname in ((80, "square_lead"), (48, "strings")):
        tr.prog(clock, 0, prog)
        clock += t(0.1)
        for mode in ("cc120", "noteoff"):
            for gap_ms in (400, 200, 100, 50, 25, 12):
                man.append(f"{mode}\t{clock / TPS:.3f}\t{mode}\t{gap_ms}\t{prog}\t60")
                tr.at(clock, [0x90, 60, 100])
                cut = clock + t(1.0)
                if mode == "cc120":
                    tr.cc(cut, 0, 120, 0)
                else:
                    tr.at(cut, [0x80, 60, 0])
                # re-attack after the gap; silence width in the render is the answer
                re = cut + t(gap_ms / 1000.0)
                tr.note(re, t(1.0), 0, 60, 100)
                clock = re + t(2.0)
    return _write_manifest("29_all_sound_off_gap.mid", tr, man)


def p30_tune_clamp_bend():
    """RPN2 coarse tune + high keys + a pitch-bend sweep, straddling the
    CentsToRatio +-4800 clamp. Settles where latched tuning enters the pitch
    chain and whether live bend is subject to the same clamp.

    SPEC.md S3.3.3 confirms CentsToRatio clamps to +-4800 cents [A:0x18e2c],
    and S3.3.2's disassembled note-on sum is
    `fineTune + (key-unityNote)*100 + pitchBendCents` -- with NO RPN term.
    S4.4 lists the RPN1/RPN2 consumer as [O], never located. So the spec does
    not say whether latched tune lands inside or outside that clamp, and
    probe 23 only exercises RPN2 at keys nowhere near it. The distinction is
    inaudible below the clamp and drastic above it: GENERAL SERUM measure 226
    plays keys 105-127 on a patch whose unity note is 81/91, which with its
    +2400 cents of RPN2 crosses +4800 on every note, and the reference's
    pitch plainly still moves there while a summed-into-the-clamp
    implementation freezes dead.

    Structure, all on program 86 (5th Saw Wave, the same patch measure 226
    uses, unity note 81 below key 106 and 91 above it):
      A. RPN2 = 0, keys 60/81/98/105/119/127 held plain -- the unclamped
         baseline, gives each key's own pitch with no tuning applied.
      B. RPN2 = +24 semitones, same keys -- keys 105+ cross the clamp. If
         tune is inside the clamp these flatten to a common pitch; if outside,
         each stays distinct.
      C. RPN2 = +24 semitones, same keys, each with a full-range bend sweep.
         If bend shares the clamp, the sweep is silent on the clamped keys;
         if bend is applied outside it, every key sweeps.
      D. RPN2 = -24 semitones for the symmetric negative-side check.
    """
    tr = Track()
    man = ["section\tstart_s\trpn2_semitones\tkey\tbend\tinstrument"]
    KEYS = (60, 81, 98, 105, 119, 127)
    clock = 0
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.prog(clock, 0, 86)
    clock += t(0.5)

    def section(tag, semis, sweep):
        nonlocal clock
        tr.rpn(clock, 0, 0, 2, 64 + semis)   # RPN2 coarse tune
        tr.bend(clock, 0, 8192)
        clock += t(0.2)
        for key in KEYS:
            man.append(f"{tag}\t{clock / TPS:.3f}\t{semis}\t{key}\t{'sweep' if sweep else 'none'}\t86")
            tr.note(clock, t(2.0), 0, key, 100)
            if sweep:
                # full-range bend sweep across the held note, 20 ms steps
                steps = 100
                for i in range(steps + 1):
                    val = int(round(i * 16383 / steps))
                    tr.bend(clock + t(0.1) + i * t(0.018), 0, val)
                tr.bend(clock + t(2.0), 0, 8192)
            clock += t(2.6)
        clock += t(0.6)

    section("A_notune", 0, False)
    section("B_up24", 24, False)
    section("C_up24_bend", 24, True)
    section("D_down24", -24, False)
    return _write_manifest("30_tune_clamp_bend.mid", tr, man)



def p31_tune_clamp_bend_sine():
    """Probe 30 rerun on a single-partial patch: bank 8 program 80, Sine Wave.

    Probe 30 uses program 86 (5th Saw Wave) because that is what GENERAL SERUM
    measure 226 actually plays, which makes it the faithful reproduction of the
    real case -- but it is a poor *measuring instrument* at these pitches. Keys
    105-127 with +24 semitones of coarse tune put the fundamental far above
    Nyquist at the 22050 Hz render rate, so a saw's harmonic stack aliases and
    the strongest visible peak is some folded image rather than the fundamental.
    Probe 30 left keys 119/127 disagreeing with the reference by 5.6% and that
    number could not be attributed: real pitch error, or simply a different
    aliased partial winning the argmax.

    A sine has exactly one partial, so whatever appears IS the (folded)
    fundamental and the comparison is unambiguous. Same structure as probe 30
    -- sections A/B/C/D, same keys, same +-24 semitone RPN2, same bend sweep --
    so the two are directly comparable note for note.

    Also worth having independently: everything in gm.dls except this patch is
    multi-partial, so this is the project's only clean instrument for any future
    pitch question.
    """
    tr = Track()
    man = ["section\tstart_s\trpn2_semitones\tkey\tbend\tinstrument"]
    KEYS = (60, 81, 98, 105, 119, 127)
    tr.sysex(0, GS_RESET)          # bank select is GS-gated on some paths (probe 22)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)          # bank 8 ...
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)  # ... program 80 = Sine Wave
    clock += t(0.5)

    def section(tag, semis, sweep):
        nonlocal clock
        tr.rpn(clock, 0, 0, 2, 64 + semis)   # RPN2 coarse tune
        tr.bend(clock, 0, 8192)
        clock += t(0.2)
        for key in KEYS:
            man.append(f"{tag}\t{clock / TPS:.3f}\t{semis}\t{key}\t{'sweep' if sweep else 'none'}\t8:80_sine")
            tr.note(clock, t(2.0), 0, key, 100)
            if sweep:
                steps = 100
                for i in range(steps + 1):
                    val = int(round(i * 16383 / steps))
                    tr.bend(clock + t(0.1) + i * t(0.018), 0, val)
                tr.bend(clock + t(2.0), 0, 8192)
            clock += t(2.6)
        clock += t(0.6)

    section("A_notune", 0, False)
    section("B_up24", 24, False)
    section("C_up24_bend", 24, True)
    section("D_down24", -24, False)
    return _write_manifest("31_tune_clamp_bend_sine.mid", tr, man)



def p32_ramp_shape():
    """Isolated control steps, each repeated 12x for ensemble averaging, on the
    single-partial Sine Wave patch. Settles the RAMP SHAPE that probes 28 and 31
    could only bound.

    What is already known and what is missing:
      - Probe 28 proved our 12 ms one-pole gain smoothing is far too slow (gate
        depth 4.3 dB where the reference holds 77.9 dB at a 6 ms half-period)
        AND that an instant per-sample gate is also wrong (it regressed the
        corpus). Reference edge durations came out ~21 ms fall / ~13 ms rise.
      - Probe 31 showed our pitch STEPS where the reference GLIDES (~9-22 ms).
      - What neither could give is the SHAPE: linear in amplitude, linear in dB,
        or exponential; and whether fall and rise genuinely differ. Fitting one
        edge on a harmonically rich patch gave R^2 of 0.014-0.65 -- useless.

    Three design changes make the shape recoverable:
      1. SINE patch (bank 8 program 80) -- one partial, so the RMS envelope IS
         the gain, with no harmonic beating to corrupt the edge.
      2. EVERY step repeated 12 times identically, each on its own fresh note so
         the note onset is a hard alignment reference. Averaging 12 aligned
         edges cuts the envelope noise ~3.5x, which is what the earlier fits
         lacked.
      3. SEVERAL step sizes per direction. A duration that scales with step size
         means a rate-limited (constant dB/s or amplitude/s) ramp; a duration
         that does not means a fixed-length ramp. One step size cannot tell
         those apart, which is the ambiguity left over from probe 28.

    Sections A-F are CC11 expression steps, G-J are pitch-bend steps, all with
    the same repeat-and-average structure so the gain and pitch ramps are
    measured on identical footing (SPEC_GAPS.md #19).
    """
    tr = Track()
    man = ["section\trep\tnote_onset_s\tstep_time_s\tkind\tfrom\tto"]
    REPS = 12
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)              # bank 8 ...
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)    # ... program 80 = Sine Wave
    clock += t(0.5)

    def steps(tag, kind, frm, to):
        """12 identical reps: fresh note, settle, one step, hold, release."""
        nonlocal clock
        for rep in range(REPS):
            onset = clock
            if kind == "cc11":
                tr.cc(onset - t(0.02), 0, 11, frm)
            else:
                tr.bend(onset - t(0.02), 0, frm)
            tr.note(onset, t(1.0), 0, 60, 100)
            step_at = onset + t(0.45)      # well clear of the attack transient
            if kind == "cc11":
                tr.cc(step_at, 0, 11, to)
            else:
                tr.bend(step_at, 0, to)
            man.append(f"{tag}\t{rep}\t{onset / TPS:.4f}\t{step_at / TPS:.4f}\t{kind}\t{frm}\t{to}")
            clock = onset + t(1.30)        # >250ms of silence between reps
        clock += t(0.4)

    # --- expression: both directions at three step sizes -------------------
    steps("A_fall_127_0",   "cc11", 127, 0)
    steps("B_rise_0_127",   "cc11", 0, 127)
    steps("C_fall_127_64",  "cc11", 127, 64)
    steps("D_rise_64_127",  "cc11", 64, 127)
    steps("E_fall_64_0",    "cc11", 64, 0)
    steps("F_rise_0_64",    "cc11", 0, 64)

    # --- pitch bend: both directions, large and small -----------------------
    # 8192 = centre. Full-range steps first, then a two-semitone-ish step, so a
    # rate-limited glide separates from a fixed-length one.
    steps("G_bend_dn_full", "bend", 8192, 0)
    steps("H_bend_up_full", "bend", 8192, 16383)
    steps("I_bend_dn_half", "bend", 8192, 4096)
    steps("J_bend_up_half", "bend", 8192, 12288)
    tr.bend(clock, 0, 8192)
    return _write_manifest("32_ramp_shape.mid", tr, man)



def p33_pitch_ramp():
    """Characterise the driver's pitch-bend RAMP: its shape, its duration law,
    its symmetry, and whether it aims at the clamped or the raw target.

    Why this probe exists. A hand-cut excerpt containing a +-60-semitone bend
    square wave at ~15 Hz (RPN0 set to 127 semitones) was analyzed to show how
    the real driver integrates into smooth arcs; we reproduce it as flat-topped
    squares, because we latch each bend value on receipt. Probe 31 measured the
    staircase (ours steps at ~23 ms, the reference does not) and probe 32 gave
    isolated-step glide times, but neither pins down the LAW. Four things are
    still unknown and each changes the implementation:

      1. SHAPE -- linear in cents, linear in frequency ratio, or a first-order
         exponential approach? Section A's large steps separate these: a linear
         ramp crosses the midpoint at half the duration, an exponential at 69%.
      2. DURATION LAW -- fixed time regardless of step size, or rate-limited
         (constant cents/second)? Section A sweeps step size over 12x at a
         fixed bend range; a fixed-time ramp gives the same 10-90% for all,
         a rate-limited one gives duration proportional to size.
      3. SYMMETRY -- up and down steps are separate rows throughout, because
         probe 28 and probe 32 disagreed about which direction is faster for
         expression and the same question is open for pitch.
      4. CLAMP INTERACTION -- `CentsToRatio` clamps at +-4800 cents (+-48
         semitones), confirmed. Does the ramp travel toward the RAW requested
         pitch and get clipped on arrival, or toward the already-clamped value?
         Section B straddles that boundary: a -40 st step never reaches it, a
         -70 st step is far past it. If the ramp aims at the raw target, the
         -70 case spends longer pinned and its approach rate is steeper.

    Every condition is repeated 8x on its own fresh note so onsets give a hard
    alignment reference and the edges can be ensemble-averaged -- single edges
    on probe 32 gave fits too noisy to read (R^2 0.014-0.65).

    Bank 8 program 80 (Sine Wave) throughout: one partial, so the instantaneous
    pitch is unambiguous. Keys are chosen per direction so the bent pitch stays
    well inside the band and never folds -- aliasing would make the tracking
    ambiguous, which is the whole problem with measuring this on real content.
    """
    tr = Track()
    man = ["section\trep\tnote_onset_s\tstep_time_s\tkind\tkey\trpn0_semis\tfrom_semis\tto_semis"]
    REPS = 8
    CENTRE = 8192

    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)
    clock += t(0.4)

    def set_range(semis):
        nonlocal clock
        tr.rpn(clock, 0, 0, 0, semis)      # RPN0 = pitch-bend sensitivity
        clock += t(0.15)

    def bend_val(semis, rng):
        v = int(round(CENTRE + (semis / float(rng)) * 8192))
        return max(0, min(16383, v))

    def steps(tag, key, rng, to_semis, hold=1.6, at=0.5):
        """8 reps: fresh note at centre bend, one step at `at`, then settle."""
        nonlocal clock
        for rep in range(REPS):
            onset = clock
            tr.bend(onset - t(0.02), 0, CENTRE)
            tr.note(onset, t(hold), 0, key, 100)
            st = onset + t(at)
            tr.bend(st, 0, bend_val(to_semis, rng))
            man.append(f"{tag}\t{rep}\t{onset / TPS:.4f}\t{st / TPS:.4f}\tbend\t{key}\t{rng}\t0\t{to_semis}")
            clock = onset + t(hold + 0.5)
        clock += t(0.3)

    # --- A: shape + duration law + symmetry, all inside the clamp -----------
    set_range(24)
    for semis in (2, -2, 6, -6, 24, -24):
        steps(f"A_step_{semis:+d}", 72, 24, semis)

    # --- B: does the ramp aim at the raw target or the clamped one? ---------
    # key 96 for downward (stays trackable at -70 st), key 36 for upward
    # (+70 st still lands under 4 kHz, no folding).
    set_range(72)
    for semis in (-40, -70):
        steps(f"B_down_{semis:+d}", 96, 72, semis)
    for semis in (40, 70):
        steps(f"B_up_{semis:+d}", 36, 72, semis)

    # --- C: the real gesture, controlled -------------------------------------
    # Alternating square wave like HueArme's, at four rates spanning its own
    # ~34 ms half-period. If the ramp model is right, the rendered excursion
    # should SHRINK as the alternation gets faster, because the ramp has less
    # time to travel before being re-aimed.
    set_range(64)
    for half_ms in (136, 68, 34, 17):
        onset = clock
        tr.bend(onset - t(0.02), 0, CENTRE)
        tr.note(onset, t(3.0), 0, 72, 100)
        man.append(f"C_square_{half_ms}ms\t0\t{onset / TPS:.4f}\t{onset / TPS:.4f}\tsquare\t72\t64\t-30\t+30")
        half = max(1, t(half_ms / 1000.0))
        pos = onset + t(0.3)
        k = 0
        while pos < onset + t(2.8):
            tr.bend(pos, 0, bend_val(30 if k % 2 == 0 else -30, 64))
            pos += half
            k += 1
        tr.bend(onset + t(2.9), 0, CENTRE)
        clock = onset + t(3.8)

    # --- D: is the EXPRESSION ramp the same law as the pitch ramp? -----------
    # Same 8-rep structure so the two are measured identically (probe 32 hinted
    # they differ: ours glides ~55-110 ms on CC11 but ~4-8 ms on bend).
    for to in (0, 64, 127):
        for rep in range(REPS):
            onset = clock
            tr.cc(onset - t(0.02), 0, 11, 127 if to != 127 else 0)
            tr.note(onset, t(1.6), 0, 72, 100)
            st = onset + t(0.5)
            tr.cc(st, 0, 11, to)
            man.append(f"D_cc11_to{to}\t{rep}\t{onset / TPS:.4f}\t{st / TPS:.4f}\tcc11\t72\t-\t{127 if to != 127 else 0}\t{to}")
            clock = onset + t(2.1)
        clock += t(0.3)

    tr.bend(clock, 0, CENTRE)
    return _write_manifest("33_pitch_ramp.mid", tr, man)



def p34_sfx_bank_identity():
    """Which sample does a bank-2 SFX program actually select, and is it pitched
    correctly? Settles a defect found by ear on real content.

    A hand-cut excerpt opens with ~14 s of bank 2 program 125 (`Car-Stop`),
    and our render of that passage tracks its
    fundamental **+693 to +960 cents above the reference** across 13 consecutive
    frames -- audible, and confirmed present with the pitch ramp disabled, so it
    is not the ramp. SPEC.md rules out the obvious explanation: bank select is
    GS-gated at `device+0xf54` (`[A:0x1341e]`/`[A:0x13492]`) and that file sends
    a GS Reset 270 ms before its CC0=2, so the gate is open; and S3.1.2's
    three-tier fallback uses an exact-equality matcher with no masking or
    rounding, while bank 2 program 125 exists outright -- so no fallback is even
    invoked. Both engines should therefore play `Car-Stop`.

    Direct measurement on the real content could not settle it: `Car-Stop` is a
    tire-screech SFX where "the fundamental" is barely a well-defined quantity,
    and a spectral-shape correlation against our own bank-0 and bank-2 renders
    came back 0.475 vs 0.443 -- neither a match. Hence this probe, which
    replaces inference with a fingerprint.

    Sections:
      A. **No-GS-reset control, FIRST in the file** (GS mode is sticky once set,
         so this can only be tested before any GS Reset). Bank 2 + program 125
         with no GS Reset: if bank select is gated, this plays bank 0
         (`Helicopter`) in both engines.
      B. **Bank fingerprint sweep.** After a GS Reset, program 125 across ALL
         TEN banks it exists in (0 Helicopter, 1 Car-Engine, 2 Car-Stop,
         3 Car-Pass, 4 Car-Crash, 5 Siren, 6 Train, 7 Jetplane, 8 Starship,
         9 Burst Noise), one note each on the same key. This gives a reference
         fingerprint per patch, so whatever the isolate actually plays can be
         matched against all ten rather than guessed between two.
      C. **Key sweep on bank 2** at the three keys the isolate uses (55, 57, 69)
         plus 43/63/81 for span. `Car-Stop` is one region, keys 0-127,
         unityNote 63, fineTune 0 -- so if unity-note transposition is applied
         normally, key 63 plays at the sample's natural rate and each key steps
         one semitone. A flat response would mean SFX patches ignore key.
      D. **The same key sweep on bank 0** (`Helicopter`, one region, unityNote
         62, fineTune 22) so the two patches' pitch behaviour can be compared on
         identical keys.

    Deliberately plain: no pitch bend, no RPN, no expression, fixed velocity.
    The isolate confounds patch selection with bend and coarse tune; this
    separates them.
    """
    tr = Track()
    man = ["section\tonset_s\tCC0\tprogram\tkey\tgs_reset_before\texpected_patch"]
    clock = 0

    def note(tag, bank, prog, key, gs, expect, hold=2.0, gap=0.6):
        nonlocal clock
        tr.cc(clock, 0, 0, bank)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, prog)
        tr.cc(clock, 0, 7, 100)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, 64)
        on = clock + t(0.05)
        tr.note(on, t(hold), 0, key, 100)
        man.append(f"{tag}\t{on / TPS:.4f}\t{bank}\t{prog}\t{key}\t{gs}\t{expect}")
        clock = on + t(hold + gap)

    # --- A: BEFORE any GS Reset, so the +0xf54 gate is closed -------------
    for key in (57, 69):
        note("A_nogs_bank2", 2, 125, key, "no", "Helicopter if gated, Car-Stop if not")
    clock += t(0.5)

    # --- everything below is after a GS Reset -----------------------------
    tr.sysex(clock, GS_RESET)
    clock += t(0.5)

    # --- B: fingerprint every bank program 125 exists in ------------------
    NAMES = {0: "Helicopter", 1: "Car-Engine", 2: "Car-Stop", 3: "Car-Pass",
             4: "Car-Crash", 5: "Siren", 6: "Train", 7: "Jetplane",
             8: "Starship", 9: "Burst-Noise"}
    for bank in range(10):
        note(f"B_bank{bank}", bank, 125, 57, "yes", NAMES[bank])
    clock += t(0.5)

    # --- C / D: key response on the two candidate patches -----------------
    for key in (43, 55, 57, 63, 69, 81):
        note("C_bank2_keys", 2, 125, key, "yes", "Car-Stop")
    clock += t(0.5)
    for key in (43, 55, 57, 63, 69, 81):
        note("D_bank0_keys", 0, 125, key, "yes", "Helicopter")
    return _write_manifest("34_sfx_bank_identity.mid", tr, man)


def p35_decay_keyfollow():
    """EG1 decay-time key-follow: how fast does a note decay as a function of
    KEY? Turns the one number this project inferred into a measurement.

169 of gm.dls's 235 instruments carry a usSource=3 (KEYNUMBER) ->
    usDestination=0x0207 (EG1 decay) connection -- SPEC.md S2.4.3's own
    confirmed table -- and this project dropped all of them until 2026-07-26,
    so every acoustic patch decayed 3-5x too slowly (found on field/town.mid:
    a Steel-str.Gt chord that rings for the whole bar instead of fading).

    **Why not the Sine/Square carriers probes 24/25/27 use.** Those probes
    measure engine-wide laws (gain, pan) that apply to any patch, so they pick
    the cleanest tone available. This one measures a PER-INSTRUMENT gm.dls
    data row, so the instrument is not a free variable: `008:080` Sine Wave
    authors no EG1 decay at all (sentinel, no sustain row -- it holds flat
    until note-off, there is no decay segment to time), `001:080` Square and
    `000:081` Saw author 3.1 s but with sustain 968/1000, so their decay
    segment falls 0.28 dB and stops -- and none of the three carries the
    key-follow row in the first place. The usable set is the 92 instruments
    with the row AND a real decay AND sustain < 10%.

    Timbre matters less here than it would for a pitch probe: EG1 is a
    broadband multiplier and every region used below loops, so the sample
    contributes no amplitude change of its own past loop entry and total RMS
    in dB vs. time is a straight line whatever the harmonic content. Verified
    against our own render: r^2 0.993-1.000 on all 17 notes, fitted decay
    within 2-6% of the expected column below. Beating between partials adds
    ripple, not slope bias.

    What is now shipped but NOT measured is the NORMALIZATION. The driver
    stores the high word of lScale (the full-scale timecent offset) and its
    consumption code is unrecovered (`[O]`, SPEC.md Part 5 +0x13c), so the
    per-note offset `kf*key/128` is DLS-1's own convention taken on faith. The
    corpus prefers /128 over /127 by 0.13 dB, which is not a measurement.

    Sections, all one instrument per section, sustain=0, held long enough that
    the whole decay segment is visible, big gaps so each note decays into
    silence uncontaminated:

      A. **Piano 1** (program 0) at keys 24/36/48/60/72/84/96. Its own authored
         decay is 6386.5 tc (40.0 s) with kf = -3979, so under /128 the decay
         time should run 27.0 s (key 24) -> 6.1 s (key 96): a >4x span across
         the section, far larger than any plausible measurement error. Fit
         dB/s per note and the slope of log(decay_time) vs. key IS the
         normalization: /128 predicts -3979/128 = -31.1 tc per key, /127
         predicts -31.3, and a 60-relative reading predicts the same slope but
         a decay time that crosses 40.0 s at key 60 instead of key 0. The
         three differ in OFFSET, which is why the sweep needs low keys.
      B. **Steel-str.Gt** (program 25), the instrument the field defect was
         found on, at the same keys. Different base (5543.9 tc) and different
         kf (-4389) -- so it cross-checks that the model is per-instrument
         data and not a constant fitted to Piano 1.
      C. **Vibraphone** (program 11, kf = -4800, the most extreme value the
         file uses) at keys 48/72/96 -- 13.1 s down to 4.6 s. This is the
         closest thing to a sine carrier that carries the connection at all:
         near-sinusoidal, and the largest kf makes any normalization error
         the most visible. The section to read if A and B disagree.
         (`008:006` Coupled Hps. matches its kf on a 40.0 s base, a slightly
         wider absolute span, but needs a GS Reset for the bank-8 select and
         is a pluckier, less clean tone. Not used.)

    All three carry eg1_sustain = 0, so the decay segment runs all the way to
    silence and a straight-line dB/s fit measures it end to end.

    Deliberately plain: velocity fixed at 100, no CC, no bend, no pedal, no
    GS Reset (bank 0 programs need none). Nothing here should decay for the
    same reason twice.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 35_decay_keyfollow.mid -- expected EG1 decay per note onset",
           "# base_tc/kf are gm.dls art1 raw values; T = 2^((base_tc/65536 +"
           " kf*key/128)/1200) seconds",
           "# onset_seconds\tsection\tprogram\tkey\texpected_decay_s"]

    def section(tag, program, base_tc, kf, keys, hold, gap):
        nonlocal clock
        tr.prog(clock, 0, program)
        clock += t(0.05)
        for key in keys:
            secs = 2.0 ** ((base_tc / 65536.0 + kf * key / 128.0) / 1200.0)
            tr.note(clock, t(hold), 0, key, 100)
            man.append(f"{clock / TPS:.3f}\t{tag}\t{program}\t{key}\t{secs:.2f}")
            clock += t(hold + gap)
        clock += t(1.0)

    # hold 8s: long enough to see the whole decay on the fast (high) keys and
    # a clean straight-line dB/s fit on the slow (low) ones, without making
    # the probe minutes long. The note is still held at note-off on low keys
    # by design -- the decay segment is what is being measured, not release.
    section("A_piano", 0, 418578432, -3979, (24, 36, 48, 60, 72, 84, 96), 8.0, 1.5)
    section("B_steel_gt", 25, 363331584, -4389, (24, 36, 48, 60, 72, 84, 96), 8.0, 1.5)
    section("C_vibraphone", 11, 409796608, -4800, (48, 72, 96), 8.0, 1.5)
    return _write_manifest("35_decay_keyfollow.mid", tr, man)


def p36_kit_key_fallback():
    """What happens when a drum kit does not cover the key?

    SPEC.md S3.1.3 `[A:0x147b7]`: FindInstrument itself walks the key ranges,
    so an instrument whose regions miss the note is rejected exactly like a
    locale mismatch and S3.1.2's NEXT fallback tier gets a turn. Only one
    instrument in gm.dls can exercise this: the SFX kit (drum, program 56)
    covers keys 39-84, while the other eight kits cover 27-87/88. So a drum
    note below 39 on an SFX-kit part should fall through to the Standard kit
    (locale 0x80000000), not go silent.

    This project dropped the note until 2026-07-26 (it matched the kit on
    locale alone, then found no region and bailed), which is audible on
    field/HueArme-Weekend.mid: channel 9 is an SFX-kit rhythm part playing
    key 35 in a velocity-ramped roll, and the whole roll was missing.

    Sections, all on channel 10 (the default rhythm part, no sysex needed):
      A. Standard kit (program 0), keys 35 and 40 -- the control: what the
         Standard kit's own regions for those keys sound like.
      B. SFX kit (program 56), key 40 -- in range, plays the SFX kit region.
      C. SFX kit, keys 27/35/38 -- all below the kit's 39 floor. Each must
         sound, and must be identical to the same key in section A.
      D. SFX kit, key 85 -- above the kit's 84 ceiling, the other side of the
         same gap; Standard kit covers up to 87.
      E. SFX kit, key 25 -- outside BOTH kits (Standard starts at 27, S2.11).
         Retry #2 fails, and locale &= 0x7f then asks for melodic program 56
         (Trumpet), which covers the full keyboard. So this one settles
         whether the third tier really is reached from a drum locale: silence
         means the fallback chain stops at the drum tier, a pitched trumpet
         note means it does not.

    RESOLVED by the reference render `[M]`, see SPEC.md S3.1.2: C and D all
    sound (and C's key 35 is spectrally the Standard kit's own key-35 region,
    log-spectrum r = 0.988), while E is digital silence -- the chain does
    stop at the drum tier.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    def hit(prog, key):
        nonlocal clock
        tr.prog(clock, 9, prog)
        tr.note(clock + t(0.1), t(0.8), 9, key, 120)
        clock += t(2.0)

    for key in (35, 40):        # A
        hit(0, key)
    hit(56, 40)                 # B
    for key in (27, 35, 38):    # C
        hit(56, key)
    hit(56, 85)                 # D
    hit(56, 25)                 # E
    return write("36_kit_key_fallback.mid", tr)


PROBES = [p01_programs, p02_keyrange, p03_velocity, p04_envelope, p05_pitchbend,
          p06_modwheel, p07_pan_volume, p08_reverb, p09_chorus, p10_polyphony,
          p11_drums, p12_gs_sysex, p13_edge, p14_running_status,
          p15_banks, p16_drum_parts, p17_master_volume, p18_key_groups,
          p19_prior_art, p20_voice_count, p21_steal_policy, p22_no_gs_reset,
          p23_rpn_tune, p24_gain_staging, p25_pan_law, p26_other_gains,
          p27_gain_curves, p28_expression_gate, p29_all_sound_off_gap,
          p30_tune_clamp_bend, p31_tune_clamp_bend_sine, p32_ramp_shape, p33_pitch_ramp, p34_sfx_bank_identity,
          p35_decay_keyfollow, p36_kit_key_fallback]


def check(path):
    """Re-parse what we wrote: chunk framing, delta times, and event bytes."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"MThd", path
    assert struct.unpack(">I", data[4:8])[0] == 6, path
    fmt, ntrk, div = struct.unpack(">HHH", data[8:14])
    assert (fmt, ntrk, div) == (0, 1, TPQ), (path, fmt, ntrk, div)
    assert data[14:18] == b"MTrk", path
    length = struct.unpack(">I", data[18:22])[0]
    assert len(data) == 22 + length, (path, len(data), length)

    i, status, saw_eot = 22, None, False
    while i < len(data):
        while data[i] & 0x80:          # delta time
            i += 1
        i += 1
        b = data[i]
        if b == 0xFF:                  # meta: FF <type> <varlen len> <data>
            meta_type = data[i + 1]
            i += 2
            n = 0
            while data[i] & 0x80:
                n = (n << 7) | (data[i] & 0x7F)
                i += 1
            n = (n << 7) | data[i]
            i += 1 + n
            saw_eot = saw_eot or meta_type == 0x2F
        elif b in (0xF0, 0xF7):        # sysex
            i += 1
            n = 0
            while data[i] & 0x80:
                n = (n << 7) | (data[i] & 0x7F)
                i += 1
            n = (n << 7) | data[i]
            i += 1 + n
        else:
            if b & 0x80:
                status = b
                i += 1
            assert status is not None, f"{path}: running status with no status"
            i += 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
    assert i == len(data), f"{path}: trailing garbage"
    assert saw_eot, f"{path}: no end-of-track"


if __name__ == "__main__":
    for probe in PROBES:
        path = probe()
        check(path)
        print(f"ok  {os.path.basename(path):28} {os.path.getsize(path):>7} bytes")
    print(f"\n{len(PROBES)} probes written and re-parsed clean.")
