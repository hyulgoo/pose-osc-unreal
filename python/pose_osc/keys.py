"""Non-blocking console key polling (Windows console via msvcrt; elsewhere disabled)."""

from __future__ import annotations

import logging
import sys

log = logging.getLogger(__name__)


class KeyPoller:
    def __init__(self) -> None:
        self._msvcrt = None
        if sys.platform == "win32" and sys.stdin is not None and sys.stdin.isatty():
            import msvcrt

            self._msvcrt = msvcrt
        else:
            log.debug("콘솔 키 입력을 쓸 수 없어요 (Windows 콘솔이 아님). --preview 창에서 키를 쓰세요")

    @property
    def available(self) -> bool:
        return self._msvcrt is not None

    def poll(self) -> list[str]:
        if self._msvcrt is None:
            return []
        keys: list[str] = []
        while self._msvcrt.kbhit():
            ch = self._msvcrt.getwch()
            if ch in ("\x00", "\xe0"):  # Function / arrow key prefix: consume the second code and ignore.
                self._msvcrt.getwch()
                continue
            keys.append(ch.lower())
        return keys
