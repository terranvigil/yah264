#!/usr/bin/env python3
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""h264_syntax.py - the Annex-B / SPS / SEI reader the stream checkers share.

Not a tool: scripts/hrd_check.py and scripts/level_check.py both have to walk
the same syntax down to the same bit, and two copies of an Exp-Golomb reader is
two places for a subtle skew to hide. Everything here is written from ITU-T
H.264 (7.3.2.1 sequence parameter set, Annex E VUI/HRD, Annex D SEI) and parses
only what those two need; it decodes no slice data.

What it deliberately does NOT do: entropy-decode a slice. Every value below is
readable from the parameter sets, the SEI messages and the slice HEADER, which
is the boundary that keeps these checkers cheap enough to run inside a gate.
"""


class Bits:
    """RBSP bit reader (Exp-Golomb per 9.1). Raises on running off the end, so
    a misparse fails loudly instead of silently returning zeros -- a parser that
    reads past a truncated VUI and then reports a clean HRD is the exact failure
    an oracle must not have."""

    def __init__(self, data):
        self.d = data
        self.pos = 0                      # bit position

    def bits_left(self):
        return len(self.d) * 8 - self.pos

    def u(self, n):
        if n > self.bits_left():
            raise ValueError("read past end of RBSP")
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def u1(self):
        return self.u(1)

    def ue(self):
        lz = 0
        while self.u1() == 0:
            lz += 1
            if lz > 32:
                raise ValueError("Exp-Golomb prefix longer than 32 bits")
        return (1 << lz) - 1 + (self.u(lz) if lz else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def rbsp(nal_payload):
    """Strip emulation_prevention_three_byte (7.4.1.1)."""
    out = bytearray()
    zeros = 0
    for b in nal_payload:
        if zeros == 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def split_nals(data):
    """Annex-B byte stream -> list of NAL dicts.

    `start` is the offset of the start-code prefix and `size` spans up to the
    next one, so the sizes sum to the file length: the NAL HRD counts the byte
    stream as delivered, start codes included (Annex C), and a checker that
    dropped them would under-count every access unit by 3-4 bytes per NAL.
    """
    nals = []
    n = len(data)
    i = 0
    marks = []
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                marks.append((i, i + 3))
                i += 3
                continue
            if i + 4 < n and data[i + 2] == 0 and data[i + 3] == 1:
                marks.append((i, i + 4))
                i += 4
                continue
        i += 1
    for k, (start, hdr) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else n
        b0 = data[hdr]
        nals.append({
            "start": start,
            "size": end - start,               # start code included
            "nal_size": end - hdr,             # nal_unit() only
            "type": b0 & 0x1F,
            "ref_idc": (b0 >> 5) & 3,
            "rbsp": rbsp(data[hdr + 1:end]),
        })
    return nals


VCL_TYPES = (1, 2, 3, 4, 5)


def _scaling_list(br, size):
    last = next_ = 8
    for _ in range(size):
        if next_:
            next_ = (last + br.se() + 256) % 256
        last = last if next_ == 0 else next_


def parse_sps(rb):
    """7.3.2.1.1 plus Annex E. Returns a dict of everything the checkers use."""
    br = Bits(rb)
    s = {}
    s["profile_idc"] = br.u(8)
    s["constraint_set"] = br.u(8)
    s["level_idc"] = br.u(8)
    s["sps_id"] = br.ue()
    s["chroma_format_idc"] = 1
    s["separate_colour_plane_flag"] = 0
    s["bit_depth_luma"] = 8
    s["bit_depth_chroma"] = 8
    if s["profile_idc"] in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        s["chroma_format_idc"] = br.ue()
        if s["chroma_format_idc"] == 3:
            s["separate_colour_plane_flag"] = br.u1()
        s["bit_depth_luma"] = br.ue() + 8
        s["bit_depth_chroma"] = br.ue() + 8
        br.u1()                                   # qpprime_y_zero_transform_bypass
        if br.u1():                               # seq_scaling_matrix_present
            n = 8 if s["chroma_format_idc"] != 3 else 12
            for i in range(n):
                if br.u1():
                    _scaling_list(br, 16 if i < 6 else 64)
    s["log2_max_frame_num"] = br.ue() + 4
    s["pic_order_cnt_type"] = br.ue()
    if s["pic_order_cnt_type"] == 0:
        s["log2_max_poc_lsb"] = br.ue() + 4
    elif s["pic_order_cnt_type"] == 1:
        br.u1()
        br.se()
        br.se()
        for _ in range(br.ue()):
            br.se()
    s["max_num_ref_frames"] = br.ue()
    s["gaps_in_frame_num_allowed"] = br.u1()
    s["pic_width_in_mbs"] = br.ue() + 1
    s["pic_height_in_map_units"] = br.ue() + 1
    s["frame_mbs_only_flag"] = br.u1()
    s["mb_adaptive_frame_field_flag"] = 0 if s["frame_mbs_only_flag"] else br.u1()
    s["direct_8x8_inference_flag"] = br.u1()
    s["crop"] = (0, 0, 0, 0)
    if br.u1():
        s["crop"] = (br.ue(), br.ue(), br.ue(), br.ue())
    s["vui"] = None
    if br.u1():
        s["vui"] = _parse_vui(br)
    s["frame_height_in_mbs"] = (2 - s["frame_mbs_only_flag"]) * s["pic_height_in_map_units"]
    s["frame_size_mbs"] = s["pic_width_in_mbs"] * s["frame_height_in_mbs"]
    return s


def _hrd(br):
    h = {}
    cnt = br.ue() + 1
    h["cpb_cnt"] = cnt
    h["bit_rate_scale"] = br.u(4)
    h["cpb_size_scale"] = br.u(4)
    h["sched"] = []
    for _ in range(cnt):
        bit_rate = (br.ue() + 1) * (1 << (6 + h["bit_rate_scale"]))
        cpb_size = (br.ue() + 1) * (1 << (4 + h["cpb_size_scale"]))
        cbr = br.u1()
        h["sched"].append({"bit_rate": bit_rate, "cpb_size": cpb_size, "cbr": cbr})
    h["initial_cpb_removal_delay_length"] = br.u(5) + 1
    h["cpb_removal_delay_length"] = br.u(5) + 1
    h["dpb_output_delay_length"] = br.u(5) + 1
    h["time_offset_length"] = br.u(5)
    return h


def _parse_vui(br):
    v = {}
    v["sar"] = None
    if br.u1():                                   # aspect_ratio_info_present
        idc = br.u(8)
        v["sar"] = (br.u(16), br.u(16)) if idc == 255 else idc
    if br.u1():                                   # overscan_info_present
        br.u1()
    if br.u1():                                   # video_signal_type_present
        br.u(3)
        br.u1()
        if br.u1():
            br.u(8)
            br.u(8)
            br.u(8)
    if br.u1():                                   # chroma_loc_info_present
        br.ue()
        br.ue()
    v["timing"] = None
    if br.u1():                                   # timing_info_present
        num_units_in_tick = br.u(32)
        time_scale = br.u(32)
        fixed = br.u1()
        v["timing"] = {"num_units_in_tick": num_units_in_tick,
                       "time_scale": time_scale, "fixed_frame_rate": fixed}
    v["nal_hrd"] = _hrd(br) if br.u1() else None
    v["vcl_hrd"] = _hrd(br) if br.u1() else None
    if v["nal_hrd"] or v["vcl_hrd"]:
        v["low_delay_hrd_flag"] = br.u1()
    v["pic_struct_present_flag"] = br.u1()
    v["bitstream_restriction"] = None
    if br.u1():
        br.u1()                                   # motion_vectors_over_pic_boundaries
        r = {"max_bytes_per_pic_denom": br.ue(), "max_bits_per_mb_denom": br.ue(),
             "log2_max_mv_length_horizontal": br.ue(),
             "log2_max_mv_length_vertical": br.ue(),
             "max_num_reorder_frames": br.ue(),
             "max_dec_frame_buffering": br.ue()}
        v["bitstream_restriction"] = r
    return v


def parse_sei(rb):
    """Annex D: split one SEI RBSP into (payload_type, payload_bytes) pairs."""
    out = []
    i = 0
    n = len(rb)
    while i < n:
        t = 0
        while i < n and rb[i] == 0xFF:
            t += 255
            i += 1
        if i >= n:
            break
        t += rb[i]
        i += 1
        size = 0
        while i < n and rb[i] == 0xFF:
            size += 255
            i += 1
        if i >= n:
            break
        size += rb[i]
        i += 1
        out.append((t, rb[i:i + size]))
        i += size
        if i < n and rb[i] == 0x80:               # rbsp_stop_one_bit
            break
    return out


def parse_buffering_period(payload, sps):
    """D.1.2 / D.2.2. Needs the SPS for the delay field widths."""
    br = Bits(payload)
    bp = {"sps_id": br.ue(), "nal": [], "vcl": []}
    vui = sps.get("vui") or {}
    for key, name in (("nal_hrd", "nal"), ("vcl_hrd", "vcl")):
        h = vui.get(key)
        if not h:
            continue
        w = h["initial_cpb_removal_delay_length"]
        for _ in range(h["cpb_cnt"]):
            bp[name].append({"initial": br.u(w), "offset": br.u(w)})
    return bp


def parse_pic_timing(payload, sps):
    """D.1.3 / D.2.3. The field widths come from whichever HRD is present."""
    vui = sps.get("vui") or {}
    h = vui.get("nal_hrd") or vui.get("vcl_hrd")
    br = Bits(payload)
    pt = {"cpb_removal_delay": None, "dpb_output_delay": None, "pic_struct": None}
    if h:
        pt["cpb_removal_delay"] = br.u(h["cpb_removal_delay_length"])
        pt["dpb_output_delay"] = br.u(h["dpb_output_delay_length"])
    if vui.get("pic_struct_present_flag"):
        pt["pic_struct"] = br.u(4)
    return pt


def parse_slice_header(rb, nal_type, sps):
    """The prefix of 7.3.3 that needs no PPS: enough to find access-unit
    boundaries and to see whether a picture is a field."""
    br = Bits(rb)
    sh = {"first_mb_in_slice": br.ue()}
    st = br.ue()
    sh["slice_type"] = st % 5
    sh["pps_id"] = br.ue()
    if sps.get("separate_colour_plane_flag"):
        br.u(2)
    sh["frame_num"] = br.u(sps["log2_max_frame_num"])
    sh["field_pic_flag"] = 0
    sh["bottom_field_flag"] = 0
    if not sps["frame_mbs_only_flag"]:
        sh["field_pic_flag"] = br.u1()
        if sh["field_pic_flag"]:
            sh["bottom_field_flag"] = br.u1()
    sh["idr_pic_id"] = br.ue() if nal_type == 5 else None
    return sh


def split_aus(data):
    """Annex-B stream -> access units.

    Each AU is {'nals': [...], 'bytes': int, 'vcl_bytes': int, 'sps', 'sei',
    'first_slice'}. Non-VCL NALs attach to the AU that FOLLOWS them, which is
    what the CPB actually receives before that picture is removed; a new AU
    opens at a VCL NAL with first_mb_in_slice == 0 (7.4.1.2.4, restricted to
    the single-slice-per-picture and field-pair shapes this tree emits).
    """
    nals = split_nals(data)
    aus = []
    cur = None
    sps_by_id = {}
    active_sps = None

    def flush():
        nonlocal cur
        if cur is not None and cur["nals"]:
            aus.append(cur)
        cur = None

    for nl in nals:
        t = nl["type"]
        if t == 7:
            s = parse_sps(nl["rbsp"])
            sps_by_id[s["sps_id"]] = s
            active_sps = s
        if t in VCL_TYPES:
            sh = None
            if active_sps is not None:
                try:
                    sh = parse_slice_header(nl["rbsp"], t, active_sps)
                except ValueError:
                    sh = None
            starts_au = sh is None or sh["first_mb_in_slice"] == 0
            if starts_au and cur is not None and cur.get("first_slice") is not None:
                flush()
            if cur is None:
                cur = {"nals": [], "bytes": 0, "vcl_bytes": 0, "sps": active_sps,
                       "sei": [], "first_slice": None}
            if cur["first_slice"] is None:
                cur["first_slice"] = sh
                cur["nal_type"] = t
            cur["vcl_bytes"] += nl["nal_size"]
        else:
            # Filler and the end-of-sequence/stream markers TRAIL the picture
            # they belong to (7.4.1.2.3); everything else non-VCL opens the next
            # access unit. Getting this backwards puts CBR filler in the FOLLOWING
            # AU, which moves several hundred bytes across a removal boundary and
            # turns a compliant CBR stream into a reported underflow.
            if t not in (10, 11, 12) and cur is not None and cur.get("first_slice") is not None:
                flush()
            if cur is None:
                cur = {"nals": [], "bytes": 0, "vcl_bytes": 0, "sps": active_sps,
                       "sei": [], "first_slice": None}
            if t == 6:
                cur["sei"].extend(parse_sei(nl["rbsp"]))
        cur["sps"] = active_sps
        cur["nals"].append(nl)
        cur["bytes"] += nl["size"]
    flush()
    return aus, sps_by_id


def read_stream(path):
    with open(path, "rb") as f:
        return f.read()


def probe(path):
    """The handful of stream properties a conformance harness needs in order to
    know which decoders can even be ASKED about a stream. Printed as shell
    `key=value` lines by `python3 h264_syntax.py --probe`."""
    data = read_stream(path)
    aus, _ = split_aus(data)
    sps = aus[0]["sps"] if aus else None
    if sps is None:
        return {"ok": 0}
    has_b = 0
    for au in aus:
        sh = au.get("first_slice")
        if sh is not None and sh["slice_type"] == 1:      # 1 == B (7.4.3)
            has_b = 1
            break
    return {
        "ok": 1,
        "profile": sps["profile_idc"],
        "level": sps["level_idc"],
        "chroma": sps["chroma_format_idc"],
        "bitdepth": sps["bit_depth_luma"],
        "mbs_only": sps["frame_mbs_only_flag"],
        "mbaff": sps["mb_adaptive_frame_field_flag"],
        "has_b": has_b,
        "aus": len(aus),
    }


if __name__ == "__main__":
    import sys as _sys
    if len(_sys.argv) == 3 and _sys.argv[1] == "--probe":
        try:
            info = probe(_sys.argv[2])
        except (OSError, ValueError):
            info = {"ok": 0}
        for k, v in info.items():
            print("%s=%s" % (k, v))
        _sys.exit(0 if info.get("ok") else 1)
    print("usage: h264_syntax.py --probe STREAM.264", file=_sys.stderr)
    _sys.exit(2)
