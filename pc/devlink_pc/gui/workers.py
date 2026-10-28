"""Run blocking network calls off the GUI thread (QThreadPool) and get results back as Qt signals."""

from __future__ import annotations

import traceback
from collections.abc import Callable

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal


class _Signals(QObject):
    done = Signal(object)
    error = Signal(str)
    progress = Signal(object)


class Task(QRunnable):
    """fn(*args, **kwargs). With progress_kw set, fn gets a callback under that keyword that sends progress to the
    GUI (one argument as is, several as a tuple)."""

    def __init__(self, fn: Callable, *args, progress_kw: str | None = None, **kwargs):
        super().__init__()
        self.fn, self.args, self.kwargs, self.progress_kw = fn, args, kwargs, progress_kw
        self.signals = _Signals()

    def run(self) -> None:
        try:
            if self.progress_kw:
                emit = self.signals.progress.emit
                self.kwargs[self.progress_kw] = lambda *a: emit(a[0] if len(a) == 1 else a)
            r = self.fn(*self.args, **self.kwargs)
        except Exception as e:  # noqa: BLE001
            self.signals.error.emit(f"{e}\n{traceback.format_exc(limit=3)}")
            return
        self.signals.done.emit(r)


_live: set[Task] = set()  # keep tasks (and their signal objects) alive until they finish


def run(fn: Callable, *args, on_done: Callable | None = None, on_error: Callable | None = None,
        on_progress: Callable | None = None, progress_kw: str = "emit", **kwargs) -> Task:
    t = Task(fn, *args, progress_kw=progress_kw if on_progress is not None else None, **kwargs)
    t.setAutoDelete(False)
    _live.add(t)

    def finish(*_):
        _live.discard(t)

    if on_done:
        t.signals.done.connect(on_done)
    if on_error:
        t.signals.error.connect(on_error)
    if on_progress:
        t.signals.progress.connect(on_progress)
    t.signals.done.connect(finish)
    t.signals.error.connect(finish)
    QThreadPool.globalInstance().start(t)
    return t
