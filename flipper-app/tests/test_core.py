"""Native checks of the actual C generator/parser and USB descriptor bytes."""

import ctypes
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def core(tmp_path_factory):
    path = tmp_path_factory.mktemp("mic-core")
    wrapper = path / "wrapper.c"
    wrapper.write_text(
        '#include "usb_descriptors.h"\n'
        "const void* descriptors(void) { return mic_config_descriptor; }\n"
        "unsigned descriptor_size(void) { return sizeof(mic_config_descriptor); }\n"
    )
    library = path / "core.so"
    subprocess.run(
        [
            "cc",
            "-shared",
            "-fPIC",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT),
            str(ROOT / "mic_core.c"),
            str(wrapper),
            "-o",
            str(library),
        ],
        check=True,
    )
    lib = ctypes.CDLL(str(library))
    lib.mic_parse_command.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint16)]
    lib.mic_parse_command.restype = ctypes.c_int
    lib.mic_generate.argtypes = [
        ctypes.POINTER(ctypes.c_int16),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_uint16,
        ctypes.c_bool,
    ]
    lib.descriptors.restype = ctypes.c_void_p
    return lib


@pytest.mark.parametrize(
    "text,command",
    [
        ("help", 1),
        ("ping", 2),
        ("status", 3),
        ("reset", 4),
        ("noise", 5),
        ("silence", 6),
        ("level 0", 7),
        ("level 4096", 7),
        ("reconnect", 8),
        ("exit", 9),
        (" \texit\t ", 9),
    ],
)
def test_commands(core, text, command):
    level = ctypes.c_uint16(128)
    assert (
        core.mic_parse_command(
            ctypes.create_string_buffer(text.encode()), ctypes.byref(level)
        )
        == command
    )
    if text.startswith("level"):
        assert level.value == int(text.split()[1])


@pytest.mark.parametrize(
    "text",
    [
        "",
        "exit now",
        "exit;reboot",
        "EXIT",
        "level",
        "level ",
        "level -1",
        "level 4097",
        "level 999999999999999999999999999",
        "level 1x",
        "level +3",
    ],
)
def test_invalid_commands_do_not_change_level(core, text):
    level = ctypes.c_uint16(128)
    assert (
        core.mic_parse_command(
            ctypes.create_string_buffer(text.encode()), ctypes.byref(level)
        )
        == 0
    )
    assert level.value == 128


def test_signal(core):
    seed = ctypes.c_uint32(0x6D696321)
    buffer = (ctypes.c_int16 * 16)()
    values = []
    for _ in range(1000):
        core.mic_generate(buffer, ctypes.byref(seed), 128, False)
        values.extend(buffer)
    assert min(values) >= -128 and max(values) <= 128
    assert abs(sum(values) / len(values)) < 3
    assert 65 < (sum(x * x for x in values) / len(values)) ** 0.5 < 80
    for silent, level in [(True, 4096), (False, 0)]:
        core.mic_generate(buffer, ctypes.byref(seed), level, silent)
        assert list(buffer) == [0] * 16


def test_usb_descriptors(core):
    data = ctypes.string_at(core.descriptors(), core.descriptor_size())
    assert len(data) == int.from_bytes(data[2:4], "little") == 174
    assert data[4] == 4
    descriptors = []
    offset = 0
    while offset < len(data):
        length = data[offset]
        assert length >= 2 and offset + length <= len(data)
        descriptors.append(data[offset : offset + length])
        offset += length
    interfaces = [d for d in descriptors if d[1] == 4]
    assert [(d[2], d[3], d[4]) for d in interfaces] == [
        (0, 0, 0),
        (1, 0, 0),
        (1, 1, 1),
        (2, 0, 1),
        (3, 0, 2),
    ]
    ac = next(i for i, d in enumerate(descriptors) if d[1:3] == bytes([0x24, 1]))
    assert sum(len(d) for d in descriptors[ac : ac + 3]) == int.from_bytes(
        descriptors[ac][5:7], "little"
    )
    fmt = next(d for d in descriptors if len(d) == 11 and d[1:3] == bytes([0x24, 2]))
    assert list(fmt[3:8]) == [1, 1, 2, 16, 1]
    assert int.from_bytes(fmt[8:11], "little") == 16000
    endpoints = [d for d in descriptors if d[1] == 5]
    assert len({d[2] & 15 for d in endpoints}) == 4
    audio = endpoints[0]
    assert audio[2:4] == bytes([0x81, 0x0D])
    assert int.from_bytes(audio[4:6], "little") == 32 and audio[6] == 1
