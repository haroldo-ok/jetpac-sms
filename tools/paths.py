"""Locations of the external inputs used by the tools.

JETPAC_SRC   directory with the Spec256 Jetpac files (JETPAC.SNA, JETPAC.GFX,
             ROM0.GFX, 48.rom, spec256.raw.pal); default: ../jetpac-src
JETPAC_SKOOL path of mrcook's jetpac.skool; default: ../jetpac-disassembly/jetpac.skool
"""
import os
TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)                      # the jetpac-sms repository
SRC = os.environ.get('JETPAC_SRC', os.path.normpath(os.path.join(ROOT, '..', 'jetpac-src')))
SKOOL = os.environ.get('JETPAC_SKOOL', os.path.normpath(os.path.join(ROOT, '..', 'jetpac-disassembly', 'jetpac.skool')))
OUT = os.environ.get('JETPAC_OUT', os.path.join(ROOT, 'shots'))   # preview images written by the analysis tools
