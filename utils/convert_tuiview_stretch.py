#!/usr/bin/env python

"""
Script to convert the TuiView default rules into lines
suitable for putting into the gdalcacaview config file
(~/.gcv). 
"""

from __future__ import print_function
from tuiview import stretchdialog
from tuiview import viewerstretch

COMP_DICT = {viewerstretch.VIEWER_COMP_LT : "less",
    viewerstretch.VIEWER_COMP_GT : "greater",
    viewerstretch.VIEWER_COMP_EQ : "equal"}

MODE_DICT = {viewerstretch.VIEWER_MODE_COLORTABLE : "colortable",
    viewerstretch.VIEWER_MODE_GREYSCALE : "greyscale",
    viewerstretch.VIEWER_MODE_RGB : "rgb"}

STRETCHMODE_DICT = {viewerstretch.VIEWER_STRETCHMODE_NONE : "none",
    viewerstretch.VIEWER_STRETCHMODE_LINEAR : "linear",
    viewerstretch.VIEWER_STRETCHMODE_STDDEV : "stddev",
    viewerstretch.VIEWER_STRETCHMODE_HIST : "histogram"}

def doConversion():
    """
    Does the conversion and prints the result to stdout
    """
    stretchRules = stretchdialog.StretchDefaultsDialog.fromSettings()
    for rule in stretchRules:
        line = "Rule=" + COMP_DICT[rule.comp] + "," + str(rule.value)
        if rule.ctband is None:
            line += ",-1"
        else:
            line += "," + str(rule.ctband)

        line += "," + MODE_DICT[rule.stretch.mode]
        
        line += "," + STRETCHMODE_DICT[rule.stretch.stretchmode]

        if rule.stretch.stretchparam is None:
            line += ","
        else:
            line += "," + '|'.join([str(x) for x in rule.stretch.stretchparam])

        line += "," + '|'.join([str(x) for x in rule.stretch.bands])
        print(line)

if __name__ == '__main__':
    doConversion()

