#! /usr/bin/env python
# vi:ts=4:et
# $Id: test_cb.py,v 1.13 2002/08/29 14:39:20 mfx Exp $

import sys
import pycurl

## Callback function invoked when body data is ready
def body(buf):
    # Print body data to stdout
    sys.stdout.write(buf)

## Callback function invoked when header data is ready
def header(buf):
    # Print header data to stderr
    sys.stderr.write(buf)

c = pycurl.Curl()
c.setopt(pycurl.URL, 'http://www.python.org/')
c.setopt(pycurl.WRITEFUNCTION, body)
c.setopt(pycurl.HEADERFUNCTION, header)
c.setopt(pycurl.FOLLOWLOCATION, 1)
c.setopt(pycurl.MAXREDIRS, 5)
c.perform()
c.setopt(pycurl.URL, 'http://curl.haxx.se/')
c.perform()
c.close()
