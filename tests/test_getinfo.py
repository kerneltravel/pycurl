#! /usr/bin/env python
# vi:ts=4:et
# $Id: test_getinfo.py,v 1.15 2002/11/25 13:03:15 kjetilja Exp $

import time
import pycurl


## Callback function invoked when progress information is updated
def progress(download_t, download_d, upload_t, upload_d):
    print 'Total to download %d bytes, have %d bytes so far' % \
          (download_t, download_d)

url = 'http://www.cnn.com'

print 'Starting downloading', url
print
f = open('body', 'wb')
h = open('header', 'wb')
c = pycurl.Curl()
c.setopt(c.URL, url)
c.setopt(c.WRITEDATA, f)
c.setopt(c.NOPROGRESS, 0)
c.setopt(c.PROGRESSFUNCTION, progress)
c.setopt(c.FOLLOWLOCATION, 1)
c.setopt(c.MAXREDIRS, 5)
c.setopt(c.WRITEHEADER, h)
c.setopt(c.OPT_FILETIME, 1)
c.perform()

print
print 'Download speed: %.2f bytes/second' % c.getinfo(c.SPEED_DOWNLOAD)
print 'Document size: %d bytes' % c.getinfo(c.SIZE_DOWNLOAD)
print 'Effective URL:', c.getinfo(c.EFFECTIVE_URL)
print 'Content-type:', c.getinfo(c.CONTENT_TYPE)
print 'Redirect-time:', c.getinfo(c.REDIRECT_TIME)
print 'Redirect-count:', c.getinfo(c.REDIRECT_COUNT)
epoch = c.getinfo(c.INFO_FILETIME)
print 'Filetime: %d (%s)' % (epoch, time.ctime(epoch))
print
print "Header is in file 'header', body is in file 'body'"

c.close()
f.close()
h.close()
