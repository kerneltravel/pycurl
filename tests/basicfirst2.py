# $Id: basicfirst2.py,v 1.3 2002/03/08 11:06:38 kjetilja Exp $

import sys
import pycurl


class test:
    def __init__(self):
        self.contents = ''

    def body_callback(self, buf):
        self.contents = self.contents + buf

print 'Testing', pycurl.version

t = test()
c = pycurl.init()
c.setopt(pycurl.URL, 'http://curl.haxx.se/dev/')
c.setopt(pycurl.WRITEFUNCTION, t.body_callback)
c.setopt(pycurl.HTTPHEADER, ["I-am-a-silly-programmer: yes indeed you are",
                             "User-Agent: Python interface for libcURL"])
c.perform()
c.cleanup()

print t.contents
