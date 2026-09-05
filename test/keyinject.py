#!/usr/bin/env python3
"""XTEST key injection into the PPSSPP 'Teaching Deck' window on :1.

Reuses PPSSPP's Linux/QWERTY default deck bindings:
  square=A triangle=S circle=X cross=Z  L=Q  R=W
  START=space SELECT=Return  D-pad=arrows  analog=I/K/J/L
"""
import os
import time
from Xlib import X, XK, display

DISPLAY_NAME = os.environ.get("DISPLAY", ":1")


def get_display():
    return display.Display(DISPLAY_NAME)


def find_deck_window(d):
    """Return the real PPSSPPSDL client window (titled 'Teaching Deck');
    skip mutter's decoration frame (same title, no WM_STATE)."""
    def walk(w):
        try:
            name = w.get_wm_name()
        except Exception:
            name = None
        if name == "Teaching Deck":
            try:
                cls = w.get_wm_class()
            except Exception:
                cls = None
            if cls and cls[0] == "PPSSPPSDL":
                return w
        for c in w.query_tree().children:
            r = walk(c)
            if r:
                return r
        return None
    return walk(d.screen().root)


def _kcode(d, name):
    ks = XK.string_to_keysym(name)
    if ks == 0:
        return None
    return d.keysym_to_keycode(ks)


def inject(d, deck=None, press_keys=None, release_keys=None, hold=0.12):
    """press_keys/release_keys: list of keysym names."""
    from Xlib.ext import xtest
    if press_keys is None:
        press_keys = []
    if release_keys is None:
        release_keys = []
    if deck is not None:
        try:
            deck.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
            deck.raise_window()
            d.sync()
        except Exception:
            pass
    for k in press_keys:
        kc = _kcode(d, k)
        if kc:
            xtest.fake_input(d, X.KeyPress, kc)
    d.sync()
    time.sleep(hold)
    for k in release_keys:
        kc = _kcode(d, k)
        if kc:
            xtest.fake_input(d, X.KeyRelease, kc)
    d.sync()


def quit_combo(d):
    """L+R+START = q+w+space: the app's quit combo."""
    inject(d, press_keys=["q", "w", "space"])


def tap(d, name):
    inject(d, press_keys=[name], release_keys=[name], hold=0.06)