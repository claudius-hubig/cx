#!/usr/bin/env python3
# sample.py — Python mode manual highlight check
"""Module-level docstring.

Spans multiple lines.  A fake close ''' inside should not end it,
and neither should a lone " character.
"""
import os
from typing import Optional


@dataclass
@app.route("/health")
class Widget:
    """Class docstring on one line."""

    count: int = 0
    ratio: float = 3.14e-2
    hexv: int = 0xDEADBEEF
    binv: int = 0b1010_1010
    octv: int = 0o755
    grouped: int = 1_000_000
    imag: complex = 3j

    def render(self, name: str) -> Optional[str]:
        prefix = r"raw\nstring"
        b = b"byte string"
        msg = f"hello {name}, count={self.count}"  # braces stay string
        if name and not self.count or name in ("a", "b"):
            return None
        try:
            return f"{prefix}: {msg}"
        except ValueError as e:
            raise RuntimeError("bad") from e
        finally:
            pass


async def main() -> None:
    async with open(os.devnull) as fh:
        await fh.read()
    match Widget():
        case Widget(count=0):
            lambda x: x + 1
