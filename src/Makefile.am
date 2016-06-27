# Note: plugindir is set in configure

plugin_LTLIBRARIES = libgstnxvideosink.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstnxvideosink_la_SOURCES = \
	gstnxvideosink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstnxvideosink_la_CFLAGS = \
	$(GST_CFLAGS)		\
	-I${includedir}		\
	-I${includedir}/libdrm

libgstnxvideosink_la_LIBADD = \
	$(GST_LIBS)			\
	-lgstvideo-1.0		\
	-lgstpbutils-1.0	\
	-lnxgstmeta			\
	-ldrm	\
	-lkms

libgstnxvideosink_la_LDFLAGS =	\
	$(GST_PLUGIN_LDFLAGS) -L$(libdir)

libgstnxvideosink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstnxvideosink.h
