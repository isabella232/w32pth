# Makefile.am -  Makefile for W32PTH
# Copyright (C) 2007, 2008 g10 Code GmbH
# 
# This file is part of W32PTH.
# 
# W32PTH is free software; you can redistribute it and/or modify it
# under the terms of the GNU LEsser General Public License as
# published by the Free Software Foundation; either version 2.1 of the
# License, or (at your option) any later version.
# 
# W32PTH is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
# 
# You should have received a copy of the GNU Lesser General Public
# License along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.

## Process this file with automake to produce Makefile.in

ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = no-dist-gzip dist-bzip2
DISTCHECK_CONFIGURE_FLAGS = --host=i686-w64-mingw32

EXTRA_DIST = autogen.sh pth-config.in libw32pth.def

bin_SCRIPTS = pth-config
include_HEADERS = pth.h
lib_LTLIBRARIES = libw32pth.la

RCCOMPILE = $(RC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
        $(libgpg_error_la_CPPFLAGS) $(AM_CPPFLAGS) $(CPPFLAGS)
LTRCCOMPILE = $(LIBTOOL) --mode=compile --tag=RC $(RCCOMPILE)

SUFFIXES: .rc .lo

.rc.lo:
	$(LTRCCOMPILE) -i $< -o $@

w32pth_res = versioninfo.lo
no_undefined = -no-undefined
export_symbols = -export-symbols $(srcdir)/libw32pth.def

install-def-file:
	$(INSTALL) $(srcdir)/libw32pth.def $(DESTDIR)$(libdir)/libw32pth.def

uninstall-def-file:
	-rm $(DESTDIR)$(libdir)/libw32pth.def

# Note that the GPG_ERROR_CFLAGS are only required for W32CE and not
# defined on plain Windows.
libw32pth_la_CFLAGS = $(GPG_ERROR_CFLAGS)
libw32pth_la_LDFLAGS = $(no_undefined) $(export_symbols) \
      -version-info \
      @W32PTH_LT_CURRENT@:@W32PTH_LT_REVISION@:@W32PTH_LT_AGE@
libw32pth_la_DEPENDENCIES = $(w32pth_res) libw32pth.def
libw32pth_la_LIBADD = $(w32pth_res) @LTLIBOBJS@ $(NETLIBS) $(GPG_ERROR_LIBS)
libw32pth_la_SOURCES = pth.h debug.h w32-pth.c w32-io.h w32-io.c utils.h


install-data-local: install-def-file

uninstall-local: uninstall-def-file

dist-hook:
	@set -e; echo "$(VERSION)" > $(distdir)/VERSION

