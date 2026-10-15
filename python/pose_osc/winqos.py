"""Opt this process out of Windows power throttling (EcoQoS) (TROUBLESHOOTING T-004).

On hybrid CPUs (e.g. i7-13700H: P + E cores) Windows 11 may run a process whose window is not in the
foreground at lower QoS (efficiency cores, lower clocks). During a demo Unreal is in the foreground, so the
inference process is exactly such a background process. ``SetProcessInformation(ProcessPowerThrottling)``
with ControlMask = EXECUTION_SPEED and StateMask = 0 asks for normal (high) QoS.
"""

from __future__ import annotations

import logging
import sys

log = logging.getLogger(__name__)

_PROCESS_POWER_THROTTLING = 4  # PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
_CURRENT_VERSION = 1
_EXECUTION_SPEED = 0x1
_IGNORE_TIMER_RESOLUTION = 0x4


def disable_power_throttling() -> bool:
    """True if the request succeeded. No-op (False) outside Windows."""
    if sys.platform != "win32":
        return False
    import ctypes
    from ctypes import wintypes

    class _State(ctypes.Structure):
        _fields_ = [("Version", wintypes.ULONG), ("ControlMask", wintypes.ULONG), ("StateMask", wintypes.ULONG)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.SetProcessInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel32.SetProcessInformation.restype = wintypes.BOOL

    # Control both mechanisms, state 0 for both = "do not throttle" / "do not ignore timer resolution".
    state = _State(_CURRENT_VERSION, _EXECUTION_SPEED | _IGNORE_TIMER_RESOLUTION, 0)
    ok = kernel32.SetProcessInformation(
        kernel32.GetCurrentProcess(), _PROCESS_POWER_THROTTLING, ctypes.byref(state), ctypes.sizeof(state)
    )
    if not ok:
        log.warning("전원 스로틀링 해제 실패 (WinError %d) — 백그라운드에서 추론이 느려질 수 있어요", ctypes.get_last_error())
        return False
    return True
