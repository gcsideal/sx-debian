if BUILD_ZCOMP

AM_CPPFLAGS = -I $(top_srcdir)/../libsx/include
pkglib_LTLIBRARIES = libsxf_zcomp.la
libsxf_zcomp_la_SOURCES = zcomp.c
libsxf_zcomp_la_LDFLAGS = -module -release 10
libsxf_zcomp_la_LIBADD = @ZCOMP_LIBS@ $(top_srcdir)/../libsx/src/libsx.la

endif
