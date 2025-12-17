# -*- Makefile -*-
#
#

lib_LTLIBRARIES = libsstwrapper.la

libsstwrapper_la_SOURCES = \
	wrapper/wrapper.hh \
	wrapper/wrapper.cc

libsstwrapper_la_LDFLAGS = -version-info 0:0:0

libsstwrapper_la_LIBADD = \
	$(PYTHON_LIBS) \
	$(MPILIBS)

# The wrapper uses SST symbols but they're provided by the main executables
# External programs need to link against the SST library
