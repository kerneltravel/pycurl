#! /usr/bin/env python
# -*- coding: iso-8859-1 -*-
# vi:ts=4:et
# $Id: curl.py,v 1.12 2003/05/08 02:09:01 esr Exp $

import pycurl

if __name__ == "__main__":
    c = pycurl.HiCurl('http://curl.haxx.se/')
    file, info = c.retrieve()
    print file.read()
    print '='*74 + '\n'
    print info
    c.close()
