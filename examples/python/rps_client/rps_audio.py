"""
rps_audio — Pure-Python shared memory audio ring buffer client.

Uses mmap + struct to access the SharedAudioRing created by the RPS engine.
No external dependencies (no numpy).

On Windows, uses ctypes to call OpenFileMappingW/MapViewOfFile to properly
open the Boost.Interprocess shared memory segment.
"""

import array
import ctypes
import math
import mmap
import struct
import sys
import time
from typing import Optional


# ---------------------------------------------------------------------------
# AudioSegmentHeader layout — must match SharedAudioRing.hpp exactly.
#
# Fields (offset from start of mapped region):
#   0: uint32 version (=1)
#   4: uint32 sampleRate
#   8: uint32 blockSize
#  12: uint32 numChannels
#  16: uint32 ringBlocks
#  20: uint32 numSidechainInputs (reserved)
#  24: uint32 numSendOutputs (reserved)
#  28: uint32 latencySamples
#  32: uint32 flags (reserved)
#  36: uint8[20] _reserved
#  56: <padding to 64-byte cache line>
#  64: uint64 inputWritePos    (atomic, alignas(64))
# 128: uint64 inputReadPos     (atomic, alignas(64))
# 192: uint64 outputWritePos   (atomic, alignas(64))
# 256: uint64 outputReadPos    (atomic, alignas(64))
# 320: uint32 transportState   (atomic, alignas(64))
# 384: --- data rings start ---
# ---------------------------------------------------------------------------

# 9 uint32 fields + 20 bytes reserved = 56 bytes
HEADER_FORMAT = "<5I 3I I 20s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 56

# Offsets for the cache-line-aligned atomic positions
OFF_INPUT_WRITE_POS = 64
OFF_INPUT_READ_POS = 128
OFF_OUTPUT_WRITE_POS = 192
OFF_OUTPUT_READ_POS = 256

# sizeof(AudioSegmentHeader) in C++ with alignas(64) atomics
AUDIO_HEADER_SIZE = 384


def _read_u64(buf, offset: int) -> int:
    """Read a uint64 from the buffer."""
    if isinstance(buf, mmap.mmap):
        buf.seek(offset)
        return struct.unpack("<Q", buf.read(8))[0]
    # ctypes buffer
    return struct.unpack_from("<Q", buf, offset)[0]


def _write_u64(buf, offset: int, value: int) -> None:
    """Write a uint64 to the buffer."""
    if isinstance(buf, mmap.mmap):
        buf.seek(offset)
        buf.write(struct.pack("<Q", value))
    else:
        struct.pack_into("<Q", buf, offset, value)


# ---------------------------------------------------------------------------
# Windows-specific shared memory using kernel32 APIs
# ---------------------------------------------------------------------------

if sys.platform == "win32":
    _kernel32 = ctypes.windll.kernel32

    FILE_MAP_ALL_ACCESS = 0x000F001F
    EVENT_MODIFY_STATE = 0x0002
    SYNCHRONIZE = 0x00100000
    WAIT_OBJECT_0 = 0
    INFINITE = 0xFFFFFFFF

    _kernel32.OpenFileMappingW.restype = ctypes.c_void_p
    _kernel32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]

    _kernel32.MapViewOfFile.restype = ctypes.c_void_p
    _kernel32.MapViewOfFile.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t
    ]

    _kernel32.UnmapViewOfFile.restype = ctypes.c_int
    _kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]

    _kernel32.CloseHandle.restype = ctypes.c_int
    _kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    _kernel32.OpenEventW.restype = ctypes.c_void_p
    _kernel32.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]

    _kernel32.SetEvent.restype = ctypes.c_int
    _kernel32.SetEvent.argtypes = [ctypes.c_void_p]

    _kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    _kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


class AudioRing:
    """
    Pure-Python ring buffer client for RPS shared audio.

    On Windows, uses kernel32 OpenFileMappingW/MapViewOfFile.
    On POSIX, uses /dev/shm or shm_open.
    """

    def __init__(self, shm_name: str):
        self.shm_name = shm_name
        self._buf = None  # mmap.mmap or ctypes buffer
        self._win_handle = None
        self._win_view = None

        if sys.platform == "win32":
            self._open_windows(shm_name)
        else:
            self._open_posix(shm_name)

        # Open named OS events for cross-process signaling.
        # These must match the names created by SharedAudioRing::createEvents() in C++.
        self._input_event = None
        self._output_event = None
        if sys.platform == "win32":
            input_name = f"rps-audio-{shm_name}-input"
            output_name = f"rps-audio-{shm_name}-output"
            self._input_event = _kernel32.OpenEventW(
                EVENT_MODIFY_STATE | SYNCHRONIZE, False, input_name
            )
            self._output_event = _kernel32.OpenEventW(
                EVENT_MODIFY_STATE | SYNCHRONIZE, False, output_name
            )
            if not self._input_event or not self._output_event:
                # Events not available yet (race) or not supported — fall back to polling
                self._input_event = None
                self._output_event = None

        # Parse header
        raw = bytes(self._buf[0:HEADER_SIZE])
        fields = struct.unpack(HEADER_FORMAT, raw)
        self.version = fields[0]
        self.sample_rate = fields[1]
        self.block_size = fields[2]
        self.num_channels = fields[3]
        self.ring_blocks = fields[4]
        self.latency_samples = fields[7]

        if self.version != 1:
            raise ValueError(f"Unsupported SharedAudioRing version: {self.version}")
        if self.block_size == 0 or self.num_channels == 0:
            raise ValueError(
                f"Invalid header: block_size={self.block_size}, num_channels={self.num_channels}"
            )

        self.block_floats = self.block_size * self.num_channels
        self.block_bytes = self.block_floats * 4
        self.ring_bytes = self.ring_blocks * self.block_bytes

        self._input_ring_offset = AUDIO_HEADER_SIZE
        self._output_ring_offset = AUDIO_HEADER_SIZE + self.ring_bytes

    def _open_windows(self, name: str) -> None:
        """Open a named shared memory object on Windows via kernel32."""
        handle = _kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, name)
        if not handle:
            err = ctypes.get_last_error()
            raise OSError(f"OpenFileMappingW('{name}') failed: error {err}")

        self._win_handle = handle

        # Map entire view (size=0 means map the whole object)
        view = _kernel32.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0)
        if not view:
            _kernel32.CloseHandle(handle)
            self._win_handle = None
            err = ctypes.get_last_error()
            raise OSError(f"MapViewOfFile failed: error {err}")

        self._win_view = view

        # First read the header to get the total size
        header_buf = (ctypes.c_char * HEADER_SIZE).from_address(view)
        fields = struct.unpack(HEADER_FORMAT, bytes(header_buf))
        block_size = fields[2]
        num_channels = fields[3]
        ring_blocks = fields[4]
        block_bytes = block_size * num_channels * 4
        total_size = AUDIO_HEADER_SIZE + 2 * ring_blocks * block_bytes

        # Create a writable ctypes buffer over the mapped view
        self._buf = (ctypes.c_char * total_size).from_address(view)

    def _open_posix(self, name: str) -> None:
        """Open a named shared memory object on POSIX via /dev/shm."""
        import os
        shm_path = f"/dev/shm/{name}"
        fd = os.open(shm_path, os.O_RDWR)
        size = os.fstat(fd).st_size
        self._buf = mmap.mmap(fd, size, access=mmap.ACCESS_WRITE)
        os.close(fd)

    def close(self) -> None:
        """Unmap the shared memory and close event handles."""
        if sys.platform == "win32":
            if self._input_event:
                _kernel32.CloseHandle(self._input_event)
                self._input_event = None
            if self._output_event:
                _kernel32.CloseHandle(self._output_event)
                self._output_event = None
            if self._win_view:
                _kernel32.UnmapViewOfFile(self._win_view)
                self._win_view = None
            if self._win_handle:
                _kernel32.CloseHandle(self._win_handle)
                self._win_handle = None
            self._buf = None
        else:
            if isinstance(self._buf, mmap.mmap):
                self._buf.close()
            self._buf = None

    def write_input_block(self, data: bytes) -> bool:
        """Write one block of interleaved float32 audio (as raw bytes) to the input ring."""
        assert self._buf is not None
        assert len(data) == self.block_bytes

        wp = _read_u64(self._buf, OFF_INPUT_WRITE_POS)
        rp = _read_u64(self._buf, OFF_INPUT_READ_POS)

        if wp - rp >= self.ring_blocks:
            return False

        slot = wp % self.ring_blocks
        offset = self._input_ring_offset + int(slot) * self.block_bytes

        if isinstance(self._buf, mmap.mmap):
            self._buf.seek(offset)
            self._buf.write(data)
        else:
            ctypes.memmove(ctypes.addressof(self._buf) + offset, data, self.block_bytes)

        _write_u64(self._buf, OFF_INPUT_WRITE_POS, wp + 1)

        # Signal the consumer that input data is available
        if self._input_event:
            _kernel32.SetEvent(self._input_event)

        return True

    def read_output_block(self) -> Optional[bytes]:
        """Read one block of interleaved float32 audio from the output ring."""
        assert self._buf is not None

        rp = _read_u64(self._buf, OFF_OUTPUT_READ_POS)
        wp = _read_u64(self._buf, OFF_OUTPUT_WRITE_POS)

        if rp >= wp:
            return None

        slot = rp % self.ring_blocks
        offset = self._output_ring_offset + int(slot) * self.block_bytes

        if isinstance(self._buf, mmap.mmap):
            self._buf.seek(offset)
            result = self._buf.read(self.block_bytes)
        else:
            result = bytes(self._buf[offset : offset + self.block_bytes])

        _write_u64(self._buf, OFF_OUTPUT_READ_POS, rp + 1)
        return result

    def wait_for_output(self, timeout_ms: int = 1000) -> bool:
        """Wait until output data is available or timeout."""
        assert self._buf is not None

        # Fast path: data already available (no syscall)
        if _read_u64(self._buf, OFF_OUTPUT_WRITE_POS) > _read_u64(self._buf, OFF_OUTPUT_READ_POS):
            return True

        # OS event wait — ~1-5μs wake-up latency instead of 1-15ms polling
        if self._output_event:
            result = _kernel32.WaitForSingleObject(self._output_event, timeout_ms)
            if result == WAIT_OBJECT_0:
                return (
                    _read_u64(self._buf, OFF_OUTPUT_WRITE_POS)
                    > _read_u64(self._buf, OFF_OUTPUT_READ_POS)
                )
            return False

        # Fallback: polling (POSIX or if events unavailable)
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            if _read_u64(self._buf, OFF_OUTPUT_WRITE_POS) > _read_u64(self._buf, OFF_OUTPUT_READ_POS):
                return True
            time.sleep(0.0001)
        return False

    def __repr__(self) -> str:
        return (
            f"AudioRing(shm='{self.shm_name}', sr={self.sample_rate}, "
            f"bs={self.block_size}, ch={self.num_channels}, rings={self.ring_blocks})"
        )

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


# ---------------------------------------------------------------------------
# WAV I/O utilities (stdlib only, no numpy)
# ---------------------------------------------------------------------------

def read_wav(path: str) -> tuple[array.array, int, int]:
    """Read a WAV file. Returns (float32 samples as array, sample_rate, num_channels)."""
    import wave
    with wave.open(path, "rb") as wf:
        sr = wf.getframerate()
        ch = wf.getnchannels()
        n_frames = wf.getnframes()
        sw = wf.getsampwidth()
        raw = wf.readframes(n_frames)

    samples = array.array("f")  # float32

    if sw == 2:
        int_samples = struct.unpack(f"<{n_frames * ch}h", raw)
        for s in int_samples:
            samples.append(s / 32768.0)
    elif sw == 3:
        n = len(raw) // 3
        for i in range(n):
            b = raw[3*i : 3*i+3]
            val = int.from_bytes(b, byteorder="little", signed=True)
            samples.append(val / 8388608.0)
    elif sw == 4:
        int_samples = struct.unpack(f"<{n_frames * ch}i", raw)
        for s in int_samples:
            samples.append(s / 2147483648.0)
    else:
        raise ValueError(f"Unsupported sample width: {sw}")

    return samples, sr, ch


def write_wav(path: str, samples: array.array, sample_rate: int, num_channels: int) -> None:
    """Write interleaved float32 samples to a 16-bit WAV file."""
    import wave
    int_data = array.array("h")  # int16
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        int_data.append(int(clamped * 32767))

    with wave.open(path, "wb") as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(int_data.tobytes())


def rms(data: array.array) -> float:
    """Compute RMS of float samples."""
    if not data:
        return 0.0
    total = sum(s * s for s in data)
    return math.sqrt(total / len(data))


def rms_db(data: array.array) -> str:
    """Return RMS as a dB string."""
    r = rms(data)
    if r < 1e-10:
        return "-inf dB"
    return f"{20 * math.log10(r):.1f} dB"
