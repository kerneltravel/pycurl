#! /usr/bin/env python
# -*- coding: iso-8859-1 -*-
# vi:ts=4:et
# $Id: file_upload.py,v 1.4 2005/02/13 08:28:01 mfx Exp $

import os, sys
import pycurl

# Class which holds a file reference and the read callback
class FileReader:
    def __init__(self, fp):
        self.fp = fp
    def read_callback(self, size):
        return self.fp.read(size)

# Check commandline arguments
if len(sys.argv) < 3:
    print "Usage: %s <url> <file to upload>" % sys.argv[0]
    raise SystemExit
url = sys.argv[1]
filename = sys.argv[2]

if not os.path.exists(filename):
    print "Error: the file '%s' does not exist" % filename
    raise SystemExit

# Initialize pycurl
c = pycurl.Curl()
c.setopt(pycurl.URL, url)
c.setopt(pycurl.UPLOAD, 1)

# Two versions with the same semantics here, but the filereader version
# is useful when you have to process the data which is read before returning
if 1:
    c.setopt(pycurl.READFUNCTION, FileReader(open(filename, 'rb')).read_callback)
else:
    c.setopt(pycurl.READFUNCTION, open(filename, 'rb').read)

# Set size of file to be uploaded, we use xxx_LARGE option in case that
# file size is greater than 2GB
filesize = os.path.getsize(filename)
c.setopt(pycurl.INFILESIZE_LARGE, filesize)

# Start transfer
print 'Uploading file %s to url %s' % (filename, url)
c.perform()
c.close()
