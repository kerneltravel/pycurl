# $Id: test_cb.py,v 1.9 2001/10/15 15:45:29 kjetilja Exp $

## System modules
import sys

## PycURL module
import pycurl

## Callback function invoked when body data is ready
def body(buf):
    # Print body data to stdout
    sys.stdout.write(buf)

## Callback function invoked when header data is ready
def header(buf):
    # Print header data to stderr
    sys.stderr.write(buf)

c = pycurl.init()
c.setopt(pycurl.URL, 'http://www.python.org/')
c.setopt(pycurl.WRITEFUNCTION, body)
c.setopt(pycurl.HEADERFUNCTION, header)
c.setopt(pycurl.NOPROGRESS, 1)
c.setopt(pycurl.FOLLOWLOCATION, 1)
c.setopt(pycurl.MAXREDIRS, 5)
c.perform()
c.setopt(pycurl.URL, 'http://curl.haxx.se/')
c.perform()
c.cleanup()
