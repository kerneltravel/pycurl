#! /usr/bin/env python
# -*- coding: utf-8 -*-
# vi:ts=4:et

"""Setup script for the pycurl module distribution."""

PACKAGE = "pycurl"
PY_PACKAGE = "curl"
VERSION = "7.19.2"

import glob, os, sys, subprocess
from distutils.core import setup
from distutils.extension import Extension
from distutils.util import split_quoted
try:
    import ctypes
except ImportError:
    ctypes = None

include_dirs = []
define_macros = []
library_dirs = []
libraries = []
runtime_library_dirs = []
extra_objects = []
extra_compile_args = []
extra_link_args = []


def scan_argv(s, default):
    p = default
    i = 1
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg.startswith(s):
            p = arg[len(s):]
            assert p, arg
            del sys.argv[i]
        else:
            i = i + 1
    return p


# append contents of an environment variable to library_dirs[]
def add_libdirs(envvar, sep, fatal=0):
    v = os.environ.get(envvar)
    if not v:
        return
    for dir in v.split(sep):
        dir = dir.strip()
        if not dir:
            continue
        dir = os.path.normpath(dir)
        if os.path.isdir(dir):
            if not dir in library_dirs:
                library_dirs.append(dir)
        elif fatal:
            print "FATAL: bad directory %s in environment variable %s" % (dir, envvar)
            sys.exit(1)


def config_win32():
    global include_dirs, extra_objects, extra_link_args, extra_compile_args
    # Windows users have to configure the curl_dir path parameter to match
    # their libcurl source installation.  The path set here is just an
    # example and thus unlikely to match your installation.
    curl_dir = r"c:\src\build\pycurl\curl-7.16.2.1"
    curl_dir = scan_argv("--curl-dir=", curl_dir)
    print "Using curl directory:", curl_dir
    assert os.path.isdir(curl_dir), "please check curl_dir in setup.py"
    include_dirs.append(os.path.join(curl_dir, "include"))
    extra_objects.append(os.path.join(curl_dir, "lib", "libcurl.lib"))
    extra_link_args.extend(["gdi32.lib", "wldap32.lib", "winmm.lib", "ws2_32.lib",])
    add_libdirs("LIB", ";")
    if "MSC" in sys.version:
        extra_compile_args.append("-O2")
        extra_compile_args.append("-GF")        # enable read-only string pooling
        extra_compile_args.append("-WX")        # treat warnings as errors
        extra_link_args.append("/opt:nowin98")  # use small section alignment

def config_unix():
    global include_dirs, library_dirs, extra_compile_args, extra_link_args, define_macros, libraries

    # check ssl library for locking
    ssltype = None
    if ctypes:
        curl_version = ctypes.cdll.LoadLibrary('libcurl.so').curl_version
        curl_version.restype = ctypes.c_char_p
        curl_verstr = curl_version()
        if ' NSS/' in curl_verstr:
            ssltype = 'NSS'
        elif ' GnuTLS/' in curl_verstr:
            ssltype = 'GnuTLS'
        elif ' OpenSSL/' in curl_verstr:
            ssltype = 'OpenSSL'

    # get relevant compile and link flags
    curl_config = scan_argv('--curl-config=', 'curl-config')
    d = os.popen("'%s' --version" % curl_config).read().strip()
    if not d:
        raise Exception("`%s' not found -- please install the libcurl development files" % curl_config)
    print "Using %s (%s)" % (curl_config, d)
    for e in split_quoted(os.popen("'%s' --cflags" % curl_config).read()):
        if e[:2] == '-I':
            # do not add /usr/include
            if e[2:].strip('/') != 'usr/include':
                include_dirs.append(e[2:])
        else:
            extra_compile_args.append(e)

    # Run curl-config --libs and --static-libs.  Some platforms may not
    # support one or the other of these curl-config options, so gracefully
    # tolerate failure of either, but not both.
    optbuf = ''
    for option in ['--libs', '--static-libs']:
        p = subprocess.Popen("'%s' %s" % (curl_config, option),
                             shell=True, stdout=subprocess.PIPE)
        (stdout, stderr) = p.communicate()
        if p.returncode == 0:
            optbuf += stdout
    if not optbuf:
        raise Exception('Neither of curl-config --libs or --static-libs'
                        'produced output')
    for e in split_quoted(optbuf):
        if e[:2] == '-l':
            libraries.append(e[2:])
            if e[2:] == 'ssl' and not ssltype:
                ssltype = 'OpenSSL'
            if e[2:] == 'gnutls' and not ssltype:
                ssltype = 'GnuTLS'
        elif e[:2] == '-L':
            library_dirs.append(e[2:])
        else:
            extra_link_args.append(e)
    if not libraries:
        libraries.append('curl')
    # Add extra compile flag for MacOS X
    if sys.platform[:-1] == 'darwin':
        extra_link_args.append('-flat_namespace')

    # add relevant ssl flags, and check if curl has SSL even if we could not detect the type
    if ssltype:
        define_macros.append(('HAVE_CURL_SSL', 1))
        define_macros.append(('HAVE_CURL_%s' % ssltype.upper(), 1))
        if ssltype == "OpenSSL":
            openssl_dir = scan_argv('--openssl-dir=', '')
            if openssl_dir:
                include_dirs.append(os.path.join(openssl_dir, 'include'))
    else:
        for e in split_quoted(os.popen("'%s' --features" % curl_config).read()):
            if e == 'SSL':
                define_macros.append(('HAVE_CURL_SSL', 1))
                break

###############################################################################

if sys.platform == 'win32':
    config_win32()
else:
    config_unix()

ext = Extension(
    name=PACKAGE,
    sources=[os.path.join('src', 'pycurl.c')],
    include_dirs=include_dirs,
    define_macros=define_macros,
    library_dirs=library_dirs,
    libraries=libraries,
    runtime_library_dirs=runtime_library_dirs,
    extra_objects=extra_objects,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

###############################################################################

# prepare data_files

def get_data_files():
    # a list of tuples with (path to install to, a list of local files)
    data_files = []
    if sys.platform == "win32":
        datadir = os.path.join("doc", PACKAGE)
    else:
        datadir = os.path.join("share", "doc", PACKAGE)
    #
    files = ["ChangeLog", "COPYING", "COPYING2", "INSTALL", "README.rst", "TODO",]
    if files:
        data_files.append((os.path.join(datadir), files))
    files = glob.glob(os.path.join("doc", "*.html"))
    if files:
        data_files.append((os.path.join(datadir, "html"), files))
    files = glob.glob(os.path.join("examples", "*.py"))
    if files:
        data_files.append((os.path.join(datadir, "examples"), files))
    files = glob.glob(os.path.join("tests", "*.py"))
    if files:
        data_files.append((os.path.join(datadir, "tests"), files))
    #
    assert data_files
    for install_dir, files in data_files:
        assert files
        for f in files:
            assert os.path.isfile(f), (f, install_dir)
    return data_files


###############################################################################

setup_args = dict(
    name=PACKAGE,
    version=VERSION,
    description="pycurl -- libcurl module for Python",
    author="Kjetil Jacobsen, Markus F.X.J. Oberhumer",
    author_email="kjetilja at gmail.com, markus at oberhumer.com",
    maintainer="Oskari Saarenmaa",
    maintainer_email="os@ohmu.fi",
    url="https://github.com/saaros/pycurl",
    license="LGPL/MIT",
    data_files=get_data_files(),
    ext_modules=[ext],
    long_description="Python bindings for libcurl.",
    packages=[PY_PACKAGE],
    package_dir={PY_PACKAGE: os.path.join('python', 'curl')},
    platforms="All",
    )

if __name__ == "__main__":
    for o in ext.extra_objects:
        assert os.path.isfile(o), o
    setup(**setup_args)
