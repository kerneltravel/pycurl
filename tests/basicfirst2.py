# $Id: basicfirst2.py,v 1.5 2002/08/06 19:59:54 mfx Exp $

import sys
import pycurl

class Test:
    def __init__(self):
        self.contents = ''

    def body_callback(self, buf):
        self.contents = self.contents + buf

print 'Testing', pycurl.version

t = Test()
c = Curl()
c.setopt(c.URL, 'http://curl.haxx.se/dev/')
c.setopt(c.WRITEFUNCTION, t.body_callback)
c.setopt(c.HTTPHEADER, ["I-am-a-silly-programmer: yes indeed you are",
                        "User-Agent: Python interface for libcURL"])
c.perform()
c.close()

print t.contents
