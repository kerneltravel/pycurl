#! /usr/bin/env python
# vi:ts=4:et
# $Id: test_memleak.py,v 1.1 2003/01/13 12:30:35 mfx Exp $

#
# just a simple self-test
# need Python 2.2 or better for garbage collection
#

import gc, pycurl, sys
gc.enable()


print "Python", sys.version
print "pycURL %s (compiled against 0x%x)" % (pycurl.version, pycurl.COMPILE_LIBCURL_VERSION_NUM)
##print "pycURL version info", pycurl.version_info()
print "  %s, compiled %s" % (pycurl.__file__, pycurl.COMPILE_DATE)


gc.collect()
flags = gc.DEBUG_COLLECTABLE | gc.DEBUG_UNCOLLECTABLE | gc.DEBUG_OBJECTS
if 1:
    flags = flags | gc.DEBUG_STATS
gc.set_debug(flags)
gc.collect()
gc.get_objects()    # dummy

print "Tracked objects:", len(gc.get_objects())

multi = pycurl.CurlMulti()
t = []
for a in range(100):
    curl = pycurl.Curl()
    multi.add_handle(curl)
    t.append(curl)

print "Tracked objects:", len(gc.get_objects())

for curl in t:
    curl.close()
    multi.remove_handle(curl)

print "Tracked objects:", len(gc.get_objects())

del t
del multi

print "Tracked objects:", len(gc.get_objects())
gc.collect()
print "Tracked objects:", len(gc.get_objects())


