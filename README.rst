======
pycurl
======

Overview
========

pycurl is a Python interface to libcurl. pycurl can be used to fetch objects
identified by a URL from a Python program, similar to the urllib Python module.
pycurl is mature, very fast, and supports a lot of features.

Intended Audience
=================

pycurl is targeted at the advanced developer - if you need dozens of
concurrent fast and reliable connections or any of the sophisticated
features as listed above then pycurl is for you.

The main drawback with pycurl is that it is a relative thin layer over
libcurl without any of those nice Pythonic class hierarchies.  This means it
has a somewhat steep learning curve unless you are already familiar with
libcurl's C API.

To sum up, pycurl is very fast (esp. for multiple concurrent operations) and
very feature complete, but has a somewhat complex interface.  If you need
something simpler or prefer a pure Python module you might want to check out
urllib2 and urlgrabber.

Documentation
=============

pycurl now includes API documentation in the doc directory of the
distribution, as well as a number of test and example scripts in the tests
and examples directories of the distribution.

The real info, though, is located in the libcurl documentation, most
important being curl_easy_setopt.  The libcurl tutorial also provides a lot
of useful information.

For a quick start have a look at the high-performance URL downloader
retriever-multi.py.

For a list of changes consult the pycurl ChangeLog.

Download
========

This version of pycurl is maintained at GitHub,
https://github.com/saaros/pycurl .  The code was imported from the pycurl
SourceForge project at http://pycurl.sourceforge.net/ on Sep 23 2012.

libcurl code and documentation is available at http://curl.haxx.se/libcurl/

License
=======

Copyright (C) 2001-2008 Kjetil Jacobsen

Copyright (C) 2001-2008 Markus F.X.J. Oberhumer

pycurl is dual licensed under the LGPL and an MIT/X derivative license based
on the cURL license.  A full copy of the LGPL license is included in the
file COPYING.  A full copy of the MIT/X derivative license is included in
the file COPYING2.  You can redistribute and/or modify pycurl according to
the terms of either license.
