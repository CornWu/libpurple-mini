EXTRA_DIST = Makefile.mingw

pkgdir = $(libdir)/purple-$(PURPLE_MAJOR_VERSION)

IRCSOURCES = irc.c parse.c cmds.c msgs.c irc.h dcc_send.c

AM_CFLAGS = $(st)

libirc_la_LDFLAGS = -module -avoid-version

if STATIC_IRC

st = -DPURPLE_STATIC_PRPL
noinst_LIBRARIES = libirc.a
pkg_LTLIBRARIES =

libirc_a_SOURCES = $(IRCSOURCES)
libirc_a_CFLAGS  = $(AM_CFLAGS)

else

st =
pkg_LTLIBRARIES = libirc.la
noinst_LIBRARIES =

libirc_la_SOURCES = $(IRCSOURCES)
libirc_la_LIBADD  = $(GLIB_LIBS)

endif

AM_CPPFLAGS = \
	-I$(top_srcdir)/libpurple \
	-I$(top_builddir)/libpurple \
	$(GLIB_CFLAGS) \
	$(DEBUG_CFLAGS)
