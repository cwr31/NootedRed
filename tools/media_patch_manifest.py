#!/usr/bin/env python3
"""Derive/check the bounded VA/GVA page manifest; never scan the whole cache.

Only patch windows are normalized. Original and already patched windows are
accepted explicitly; other bytes are never "recovered" or described as stock.
The resulting SHA-256 fingerprints describe the observed bytes OUTSIDE those
windows. New OS/image identities require review and a new checked-in manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct
import uuid

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "NootedRed/MediaPatchManifest.json"
HEADER = ROOT / "NootedRed/MediaPatchManifest.inc"
GVA_PATH = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"

# Each required VA site is tied to a named function, not an arbitrary occurrence
# of a short constant-return stub in another image.
SITES = [
    ("Identify", "Identify", 17, "__ZN17VAAcceleratorInfo8identifyEjj"),
    ("AddrLibInit", "AddrLibInit", 9, "__ZN18VAAddrLibInterface4initEP11VAIOConnectP13sHardwareInfo"),
    ("AddrLibCreate", "AddrLibCreate", 2, "__ZN4Addr3Lib6CreateEPK18_ADDR_CREATE_INPUTP19_ADDR_CREATE_OUTPUT"),
    ("ConvertFamily", "ConvertFamily_Modern", 0, "__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj"),
    ("EngineAndBlt", "EngineAndBlt", 0, "__ZN9VAFactory20createGraphicsEngineEP9VAContext"),
    ("EngineAndBlt", "EngineAndBlt", 0, "__ZN9VAFactory14createImageBltEP9VAContextj"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory29createColourFormatConverterVPEi"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory17createDitheringVPEi"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory17createDeblockerVPEi"),
    ("EncFeedbackStride", "EncFeedbackStride", 4, "__ZN14Vcn2EncCommand15getFeedbackSizeEv"),
    ("StatusStride_Modern", "StatusStride_Modern", 3,
     "__ZN26AVDVcn2AvcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture"),
    ("StatusStride_Modern", "StatusStride_Modern", 3,
     "__ZN27AVDVcn2HevcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture"),
    ("SourcePitch_Modern", "SourcePitch_Modern", 0, "__ZN11Vcn2Encoder15setEncodeParamsEPvP15VAVendorTexture"),
]

# Exact instruction windows for the reviewed 25G332 VCN2 encoder. These are
# additional required sites, not a fallback signature search on another build.
HEVC_ENCODE_SITES = [
    # Pass the actual source texture to the format builder. The old second
    # argument was an unused command-info pointer; the context can remain NV12
    # even when the source texture is P010.
    ("__ZN12VAUveEncoder13EncodePictureEPvP15VAVendorTextureS0_", 0x14A,
     "4c89ee", "4c89fe"),
    # NV12 and P010 share texture enum 0x13. Detect the two-byte source elements
    # instead, and set both 10-bit depth and P010 packing to 1. RGB has four-byte
    # elements and retains its context-based branches/constants. Store the
    # shared colour flags from EAX to make room for the texture-field check.
    ("__ZN11Vcn2Encoder29setInputAndOutputFormatParamsEP11_UveCmdInfo", 0x1F,
     "8b879c0000003d3032347874293d7230316c74113d626772617523c5fb10058ce20400"
     "eb08c5fb100572e20400b001b901000000eb0cc5fa100509e9030031c931c0894dc4894dcc",
     "83be8400000002742c8b879c0000003d7230316c74113d626772617521c5fb10058ae20400"
     "eb08c5fb100570e2040031c0ffc0eb0b660f76c0660f72d01f31c08945c48945cc9090"),
    # This VCN2 path uses PPS chroma offsets, but does not apply the separate
    # per-slice deltas to encoded pixels. Explicitly signal zero slice deltas
    # rather than AppleGVA's Main10 Cr=-3, which produces a visible colour error.
    # These loads execute only when the PPS enables per-slice chroma offsets.
    # PPS offsets and all luma/rate-control/deblock settings remain native.
    ("__ZN23AvdVcn2HevcHeaderEncode17encodeSliceHeaderEPvS0_S0_", 0x520,
     "418bb784070000", "31f69090909090"),
    ("__ZN23AvdVcn2HevcHeaderEncode17encodeSliceHeaderEPvS0_S0_", 0x530,
     "418bb788070000", "31f69090909090"),
]
REQUIRED_SITES = 14 + len(HEVC_ENCODE_SITES)


def arrays():
    text = (ROOT / "NootedRed/VDASignatures.inc").read_text()
    return {
        name: bytes(int(n, 16) for n in re.findall(r"0x([0-9a-fA-F]{2})", re.sub(r"//[^\n]*", "", body)))
        for name, body in re.findall(r"UInt8\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S)
    }


def matches(data, context, mask, patch_offset, find, replace):
    result = []
    for offset in range(len(data) - len(context) + 1):
        window = data[offset:offset + len(context)]
        at = offset + patch_offset
        if data[at:at + len(find)] not in (find, replace):
            continue
        # Account only for the declared replacement; keep all surrounding guards.
        window = window[:patch_offset] + find + window[patch_offset + len(find):]
        if all((a & m) == (b & m) for a, b, m in zip(window, context, mask)):
            result.append(at)
    return result


def kernel_uuid(path):
    with path.open("rb") as file:
        header = file.read(32)
        if len(header) != 32 or struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
            raise ValueError("kernel must be a thin x86_64 Mach-O image")
        count, length = struct.unpack_from("<II", header, 16)
        if length > 0x10000:
            raise ValueError("unexpected kernel load-command size")
        data = file.read(length)
    offset, result = 0, None
    for _ in range(count):
        if len(data) - offset < 8:
            raise ValueError("truncated kernel load commands")
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or length > len(data) - offset:
            raise ValueError("invalid kernel load command")
        if command == 0x1B:
            if length != 24 or result is not None:
                raise ValueError("invalid/duplicate kernel UUID")
            result = str(uuid.UUID(bytes=data[offset + 8:offset + 24])).upper()
        offset += length
    if result is None or offset != len(data):
        raise ValueError("missing UUID or inconsistent kernel command size")
    return result


def collect(cache_dir, build, dump_pages=None, kernel=Path("/System/Library/Kernels/kernel")):
    a = arrays()
    with DyldCache(cache_dir) as cache:
        va, gva = MachOImage(cache, VA_IMAGE_PATH), MachOImage(cache, GVA_PATH)
        info = {
            "schema": 1, "build": build, "kernel_uuid": kernel_uuid(kernel),
            "cache_uuid": str(uuid.UUID(bytes=cache.uuid)).upper(),
            "images": [
                {"path": p, "uuid": str(uuid.UUID(bytes=m.uuid)).upper(),
                 "text_start": hex(m.text[2]), "text_size": m.text[3]}
                for p, m in ((VA_IMAGE_PATH, va), (GVA_PATH, gva))
            ],
            "pages": [],
        }
        sites = []
        for suffix, context_suffix, patch_offset, symbol in SITES:
            function = va.symbol(symbol)
            end = va.function_end(function)
            data = cache.read(function, end - function)
            find, replace = a["kVDAFind_" + suffix], a["kVDAReplace_" + suffix]
            candidates = matches(data, a["kVDACtx_" + context_suffix], a["kVDAMask_" + context_suffix],
                                 patch_offset, find, replace)
            if len(candidates) != 1:
                raise ValueError(f"{symbol}: expected one guarded site, found {len(candidates)}")
            sites.append((function + candidates[0], find, replace, symbol))
        for symbol, offset, find_hex, replace_hex in HEVC_ENCODE_SITES:
            function = va.symbol(symbol)
            find, replace = bytes.fromhex(find_hex), bytes.fromhex(replace_hex)
            address = function + offset
            if (len(find) != len(replace) or address + len(find) > va.function_end(function) or
                    cache.read(address, len(find)) not in (find, replace)):
                raise ValueError(f"{symbol}+{offset:#x}: reviewed HEVC encode window differs")
            sites.append((address, find, replace, symbol))
        # AppleGVA has stripped local symbols. Require exactly one guarded
        # selection branch inside its bounded __text section.
        suffix = "Tahoe"
        find, replace = a["kGVAFind_" + suffix], a["kGVAReplace_" + suffix]
        candidates = matches(gva.text_bytes(), a["kGVACtx_" + suffix], a["kGVAMask_" + suffix],
                             10, find, replace)
        if len(candidates) != 1:
            raise ValueError(f"AppleGVA: expected one guarded branch, found {len(candidates)}")
        sites.append((gva.text[2] + candidates[0], find, replace, "AVDGetGPUAccelerators"))
        pages = {}
        for address, find, replace, symbol in sites:
            page_address = address & ~4095
            start, _, file_offset, handle = cache.mapping_for(page_address)
            offset = file_offset + page_address - start
            key = (Path(handle.name).name, offset)
            if key not in pages:
                pages[key] = {"file": key[0], "offset": hex(offset), "sites": [],
                              "data": bytearray(cache.read_page(address))}
            page = pages[key]
            within = address - page_address
            if within + len(find) > 4096:
                raise ValueError(f"{symbol}: patch crosses a code-signing page")
            page["sites"].append({"offset": within, "find": find.hex(), "replace": replace.hex(), "symbol": symbol})
        for index, (_, page) in enumerate(sorted(pages.items())):
            page["sites"].sort(key=lambda s: s["offset"])
            data = page.pop("data")
            if dump_pages:
                dump_pages.mkdir(parents=True, exist_ok=True)
                (dump_pages / f"page-{index}.bin").write_bytes(data)
            previous_end = 0
            for site in page["sites"]:
                off, find, replace = site["offset"], bytes.fromhex(site["find"]), bytes.fromhex(site["replace"])
                if off < previous_end or len(find) != len(replace) or data[off:off + len(find)] not in (find, replace):
                    raise ValueError(f"invalid or overlapping site: {site['symbol']}")
                previous_end = off + len(find)
                data[off:previous_end] = bytes(len(find))
            page["masked_sha256"] = hashlib.sha256(data).hexdigest()
            info["pages"].append(page)
    if sum(len(p["sites"]) for p in info["pages"]) != REQUIRED_SITES:
        raise ValueError("incomplete VA/GVA required-site set")
    return info


def cbytes(data):
    return ", ".join(f"0x{x:02X}" for x in data)


def render(info):
    lines = [
        "// Generated by tools/media_patch_manifest.py; review the companion JSON.",
        "// Only VA/GVA patch windows are masked in page hashes; no stock-page recovery.",
        "// Image/cache UUIDs are checked by preflight. Runtime pins the kernel UUID,",
        "// checks the OS build once populated, and verifies",
        "// exact cache basename/file page and every other byte via SHA-256.",
        "#pragma once", "namespace MediaPatchManifest {",
    ]
    page_sites = []
    for index, page in enumerate(info["pages"]):
        for site_index, site in enumerate(page["sites"]):
            tag = f"Site{index}_{site_index}"
            lines += [
                f"    // {site['symbol']}",
                f"    inline constexpr UInt8 {tag}Find[] = {{{cbytes(bytes.fromhex(site['find']))}}};",
                f"    inline constexpr UInt8 {tag}Replace[] = {{{cbytes(bytes.fromhex(site['replace']))}}};",
            ]
        lines.append(f"    inline constexpr MediaPatchPlan::Site Page{index}Sites[] = {{")
        for site_index, site in enumerate(page["sites"]):
            tag = f"Site{index}_{site_index}"
            lines.append(f"        {{{hex(site['offset'])}, {tag}Find, {tag}Replace, sizeof({tag}Find)}},")
        lines.append("    };")
        page_sites.append(f"Page{index}Sites")
    lines.append("    inline constexpr MediaPatchPlan::Page Pages[] = {")
    for page, sites in zip(info["pages"], page_sites):
        digest = cbytes(bytes.fromhex(page["masked_sha256"]))
        lines.append(f'        {{"{page["file"]}", {page["offset"]}ULL, {{{digest}}}, {sites}, arrsize({sites})}},')
    lines += [
        "    };",
        f'    inline constexpr char Build[] = "{info["build"]}";',
        f'    inline constexpr UInt8 KernelUUID[] = {{{cbytes(uuid.UUID(info["kernel_uuid"]).bytes)}}};',
        f"    inline constexpr size_t RequiredSites = {REQUIRED_SITES};",
        "    static_assert(arrsize(Pages) > 0 && arrsize(Pages) <= 32);",
        "    inline constexpr UInt32 RequiredPageMask = ~UInt32{0} >> (32 - arrsize(Pages));",
        "}", "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    parser.add_argument("--build", help="required for a non-live cache")
    parser.add_argument("--kernel", type=Path, default=Path("/System/Library/Kernels/kernel"))
    parser.add_argument("--candidate", type=Path, help="write a review candidate JSON, never edit the committed files")
    parser.add_argument("--render", type=Path, help="render a reviewed JSON to stdout (no cache access)")
    parser.add_argument("--dump-pages", type=Path, help="save local test snapshots outside the repository")
    args = parser.parse_args()
    if args.render:
        print(render(json.loads(args.render.read_text())), end="")
        return 0
    build = args.build or subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip()
    info = collect(args.cache_dir, build, args.dump_pages, args.kernel)
    if args.cache_dir == DEFAULT_CACHE_DIR:
        running_uuid = subprocess.check_output(["sysctl", "-n", "kern.uuid"], text=True).strip().upper()
        if info["kernel_uuid"] != running_uuid:
            raise ValueError("on-disk kernel differs from the running kernel UUID")
    if args.candidate:
        args.candidate.write_text(json.dumps(info, indent=2) + "\n")
        print(f"Candidate: {len(info['pages'])} bounded pages, {REQUIRED_SITES} required sites. Review before use.")
        print("Fingerprints are observed bytes outside declared patch windows, not independent stock evidence.")
        return 0
    expected = json.loads(MANIFEST.read_text())
    if expected != info or HEADER.read_text() != render(expected):
        raise ValueError("OS/image identities, required sites, page fingerprints or generated header differ")
    print(f"PASS {build}: exact cache and both image UUIDs; {len(info['pages'])} page fingerprints, {REQUIRED_SITES} required sites")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        raise SystemExit(f"FAIL {error}")
