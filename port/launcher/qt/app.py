# SPDX-License-Identifier: GPL-2.0-or-later
"""Qt Quick application: fonts, the hero image provider, the QML engine and the window."""
import math
import sys
from pathlib import Path

from PySide6.QtCore import QPointF, QRectF, QSize, Qt, QUrl
from PySide6.QtGui import (QBrush, QColor, QFontDatabase, QGuiApplication, QIcon, QImage, QLinearGradient, QPainter,
                           QPen, QRadialGradient)
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider

from . import logic
from .backend import Backend
from .controls import Controls
from .graphics import Graphics

HERE = Path(__file__).resolve().parent
QML_ROOT = HERE / 'qml'
FONT_DISPLAY = 'Big Shoulders Stencil Display'
FONT_TEXT = 'Barlow'


def load_fonts():
    """Loads the OFL fonts shipped in qt/fonts; returns (display family, text family), falling
    back to the system's sans-serif when a font file is missing or does not load."""
    families = {}
    for path in sorted((HERE / 'fonts').glob('*.ttf')):
        ident = QFontDatabase.addApplicationFont(str(path))
        if ident >= 0:
            for name in QFontDatabase.applicationFontFamilies(ident):
                families[name] = True
    return (FONT_DISPLAY if FONT_DISPLAY in families else 'sans-serif',
            FONT_TEXT if FONT_TEXT in families else 'sans-serif')


class HeroProvider(QQuickImageProvider):
    """image://hero/bg: the Nest's valley at dusk (warm sun through the mist, light shafts over the
    ruins), used when the player's pic1.png is absent (and under it while it loads). Drawn here so it
    needs no art file (no game art ships with the port)."""

    def __init__(self):
        super().__init__(QQuickImageProvider.ImageType.Image)

    def requestImage(self, ident, size, requested):
        w, h = (requested.width(), requested.height()) if requested.isValid() else (884, 392)
        w, h = max(w, 1), max(h, 1)
        if requested.isValid() is False:
            w, h = 884, 392
        img = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.transparent)
        p = QPainter(img)
        p.setRenderHint(QPainter.Antialiasing)
        base = QLinearGradient(0, 0, 0, h)
        base.setColorAt(0, QColor('#1b2a30'))
        base.setColorAt(0.55, QColor('#141c20'))
        base.setColorAt(1, QColor('#0c1012'))
        p.fillRect(0, 0, w, h, base)
        self.glow(p, w, h, 0.74 * w, 0.18 * h, 0.62 * w, 0.75 * h, QColor(255, 186, 102), 0.50)
        self.glow(p, w, h, 0.18 * w, 0.95 * h, 0.55 * w, 0.55 * h, QColor(111, 179, 212), 0.20)
        # light shafts: wide soft bands falling from the sun, tilted 24 degrees
        slope = math.tan(math.radians(24))
        for i, (offset, width, alpha) in enumerate(((0.52, 0.05, 16), (0.62, 0.09, 12), (0.71, 0.04, 18),
                                                    (0.80, 0.07, 10), (0.90, 0.05, 8))):
            x0 = offset * w
            shaft = QLinearGradient(QPointF(x0, 0), QPointF(x0 - h * slope, h))
            c0, c1 = QColor(255, 220, 160, alpha), QColor(255, 220, 160, 0)
            shaft.setColorAt(0, c0)
            shaft.setColorAt(1, c1)
            p.setPen(Qt.NoPen)
            p.setBrush(QBrush(shaft))
            band = width * w
            p.drawPolygon([QPointF(x0, 0), QPointF(x0 + band, 0),
                           QPointF(x0 + band - h * slope, h), QPointF(x0 - h * slope, h)])
        p.end()
        return img

    @staticmethod
    def glow(p, w, h, cx, cy, rx, ry, color, alpha):
        """Elliptical radial glow: alpha at the center, fading out at 62% of the radius."""
        p.save()
        p.translate(cx, cy)
        p.scale(1.0, ry / rx)
        gradient = QRadialGradient(QPointF(0, 0), rx)
        c0, c1 = QColor(color), QColor(color)
        c0.setAlphaF(alpha)
        c1.setAlphaF(0)
        gradient.setColorAt(0, c0)
        gradient.setColorAt(0.62, c1)
        p.fillRect(QRectF(-w * 2, -h * 4, w * 4, h * 8), QBrush(gradient))
        p.restore()


class App:
    """Everything the window needs; `show()` for the real launcher, `grab()` for screenshots."""

    def __init__(self, backend=None, argv=None):
        self.qt = QGuiApplication.instance() or QGuiApplication(argv or sys.argv)
        self.qt.setApplicationName('tlg-launcher')
        self.qt.setDesktopFileName('tlg-pc')
        self.backend = backend or Backend()
        self.backend.fonts = load_fonts()
        icon = HERE.parent / 'tlg.png'
        if icon.is_file():
            self.qt.setWindowIcon(QIcon(str(icon)))
        self.graphics = Graphics(self.backend, self.backend.probes.get('graphics'))
        self.controls = Controls(self.backend)
        self.engine = QQmlApplicationEngine()
        self.engine.addImageProvider('hero', HeroProvider())
        self.engine.addImportPath(str(QML_ROOT))
        self.engine.rootContext().setContextProperty('Backend', self.backend)
        self.engine.rootContext().setContextProperty('Saves', self.backend.saves)
        self.engine.rootContext().setContextProperty('Graphics', self.graphics)
        self.engine.rootContext().setContextProperty('PadControls', self.controls)
        self.engine.rootContext().setContextProperty('Support', self.backend.support)
        self.engine.rootContext().setContextProperty('Updates', self.backend.updates)
        self.engine.loadFromModule('Launcher', 'Window')
        if not self.engine.rootObjects():
            raise RuntimeError('the QML interface did not load (see the messages above)')
        self.window = self.engine.rootObjects()[0]

    def go(self, index):
        self.window.setProperty('current', index)

    def grab(self, path):
        """PNG of the current window contents (QQuickWindow.grabWindow)."""
        from PySide6.QtCore import QEventLoop, QTimer
        loop = QEventLoop()                       # let transitions (<= 250 ms) finish
        QTimer.singleShot(450, loop.quit)
        loop.exec()
        image = self.window.grabWindow()
        return image.save(str(path))

    def run(self):
        self.backend.start_probes()
        self.graphics.start()
        code = self.qt.exec()
        self.backend.shutdown()
        return code
