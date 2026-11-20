#!/usr/bin/env python3
"""Show where OpenGL textures are allocated on a Renoir system.

Creates uploaded textures and render targets through CGL. For each one it prints how much
VRAM and system memory the accelerator reports in use afterwards, and the debug.nred.ram
counters that changed. See docs/RAM-Validation.md.
"""
import ctypes
import re
import subprocess
import time

GL = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/OpenGL")
TEXTURE_2D, FRAMEBUFFER, COLOR0, QUADS = 0x0DE1, 0x8D40, 0x8CE0, 7
RGBA, UBYTE, HALF, RGBA8, RGBA16F = 0x1908, 0x1401, 0x140B, 0x8058, 0x881A
MIN_FILTER, MAG_FILTER, LINEAR = 0x2801, 0x2800, 0x2601
MIB = 1 << 20


def memory():
    out = subprocess.run(
        ["ioreg", "-r", "-c", "AMDRadeonX5000_AMDVega10GraphicsAccelerator", "-d", "1", "-w0", "-k",
         "PerformanceStatistics"], capture_output=True, text=True).stdout
    return {key: int(re.search(rf'"{key}"=(\d+)', out).group(1))
            for key in ("inUseVidMemoryBytes", "inUseSysMemoryBytes")}


def counters():
    out = subprocess.run(["sysctl", "debug.nred.ram"], capture_output=True, text=True).stdout
    return {name.split(".")[-1]: int(value) for name, value in re.findall(r"^(\S+): (\d+)$", out, re.M)}


def float32(value):
    return ctypes.c_float(value)


def sample(texture, framebuffer):
    target = ctypes.c_uint()
    GL.glGenTextures(1, ctypes.byref(target))
    GL.glBindTexture(TEXTURE_2D, target)
    GL.glTexImage2D(TEXTURE_2D, 0, RGBA8, 64, 64, 0, RGBA, UBYTE, None)
    GL.glBindFramebufferEXT(FRAMEBUFFER, framebuffer)
    GL.glFramebufferTexture2DEXT(FRAMEBUFFER, COLOR0, TEXTURE_2D, target, 0)
    GL.glViewport(0, 0, 64, 64)
    GL.glEnable(TEXTURE_2D)
    GL.glBindTexture(TEXTURE_2D, texture)
    GL.glBegin(QUADS)
    for x, y in ((0, 0), (1, 0), (1, 1), (0, 1)):
        GL.glTexCoord2f(float32(x), float32(y))
        GL.glVertex2f(float32(x * 2 - 1), float32(y * 2 - 1))
    GL.glEnd()
    GL.glDisable(TEXTURE_2D)
    GL.glFinish()
    GL.glBindFramebufferEXT(FRAMEBUFFER, 0)


def main():
    attributes = (ctypes.c_int * 4)(73, 99, 0x1000, 0)
    pixel_format, count, context = ctypes.c_void_p(), ctypes.c_int(), ctypes.c_void_p()
    assert GL.CGLChoosePixelFormat(attributes, ctypes.byref(pixel_format), ctypes.byref(count)) == 0
    assert GL.CGLCreateContext(pixel_format, None, ctypes.byref(context)) == 0
    GL.CGLSetCurrentContext(context)
    framebuffer = ctypes.c_uint()
    GL.glGenFramebuffersEXT(1, ctypes.byref(framebuffer))
    keep = []

    def case(label, create):
        before, before_counters = memory(), counters()
        keep.append(create())
        time.sleep(1.5)
        after, after_counters = memory(), counters()
        line = f"{label:26} vid {(after['inUseVidMemoryBytes'] - before['inUseVidMemoryBytes']) / MIB:+7.1f} MiB" \
               f"  sys {(after['inUseSysMemoryBytes'] - before['inUseSysMemoryBytes']) / MIB:+7.1f} MiB"
        for name, value in after_counters.items():
            delta = value - before_counters.get(name, 0)
            if delta and name != "failures":
                line += f"  {name} {delta / MIB:+.1f} MiB" if name.endswith("_bytes") else f"  {name} {delta:+d}"
        print(line)

    def upload(internal, kind, size, bytes_per_pixel):
        def create():
            texture = ctypes.c_uint()
            GL.glGenTextures(1, ctypes.byref(texture))
            GL.glBindTexture(TEXTURE_2D, texture)
            GL.glTexParameteri(TEXTURE_2D, MIN_FILTER, LINEAR)
            GL.glTexParameteri(TEXTURE_2D, MAG_FILTER, LINEAR)
            data = (ctypes.c_ubyte * (size * size * bytes_per_pixel))()
            GL.glTexImage2D(TEXTURE_2D, 0, internal, size, size, 0, RGBA, kind, data)
            sample(texture, framebuffer)
            return texture, data
        return create

    def render_target(internal, kind, size):
        def create():
            texture = ctypes.c_uint()
            GL.glGenTextures(1, ctypes.byref(texture))
            GL.glBindTexture(TEXTURE_2D, texture)
            GL.glTexImage2D(TEXTURE_2D, 0, internal, size, size, 0, RGBA, kind, None)
            GL.glBindFramebufferEXT(FRAMEBUFFER, framebuffer)
            GL.glFramebufferTexture2DEXT(FRAMEBUFFER, COLOR0, TEXTURE_2D, texture, 0)
            GL.glViewport(0, 0, size, size)
            GL.glClearColor(float32(0.2), float32(0.4), float32(0.6), float32(1))
            GL.glClear(0x4000)
            GL.glFinish()
            GL.glBindFramebufferEXT(FRAMEBUFFER, 0)
            return texture
        return create

    case("upload RGBA8 2048x2048", upload(RGBA8, UBYTE, 2048, 4))
    case("upload RGBA16F 2048x2048", upload(RGBA16F, HALF, 2048, 8))
    case("render target RGBA8", render_target(RGBA8, UBYTE, 2048))
    case("render target RGBA16F", render_target(RGBA16F, HALF, 2048))


if __name__ == "__main__":
    main()
