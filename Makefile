#
# to use a specific python version call
#   `make PYTHON=python2.7'
#

SHELL = /bin/sh

PYTHON = python

all build:
	$(PYTHON) setup.py build

build-7.10.8:
	$(PYTHON) setup.py build --curl-config=/home/hosts/localhost/packages/curl-7.10.8/bin/curl-config

test: build
	$(PYTHON) tests/test_internals.py -q

# (needs GNU binutils)
strip: build
	strip -p --strip-unneeded build/lib*/*.so
	chmod -x build/lib*/*.so

install install_lib:
	$(PYTHON) setup.py $@

clean:
	$(RM) -r build dist
	$(RM) *.pyc *.pyo */*.pyc */*.pyo */*/*.pyc */*/*.pyo
	$(RM) MANIFEST src/pycurl.so

distclean: clean

maintainer-clean: distclean

dist sdist: distclean
	$(PYTHON) setup.py sdist

# target for maintainer
windist: distclean
	python2.6 setup.py bdist_wininst
	rm -rf build
	python2.7 setup.py bdist_wininst
	rm -rf build
	python2.6 setup_win32_ssl.py bdist_wininst
	rm -rf build
	python2.7 setup_win32_ssl.py bdist_wininst
	rm -rf build


.PHONY: all build test strip install install_lib clean distclean maintainer-clean dist sdist windist

.NOEXPORT:
