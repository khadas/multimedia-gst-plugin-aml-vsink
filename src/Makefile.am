# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstamlvsink.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstamlvsink_la_SOURCES = gstamlvsink.c display.c v4l-dec.c
# compiler and linker flags used to compile this plugin, set in configure.ac
libgstamlvsink_la_CFLAGS = $(GST_CFLAGS) $(DRM_CFLAGS)
libgstamlvsink_la_LIBADD = $(GST_LIBS)
libgstamlvsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) $(DRM_LIBS) $(ASINK_LIBS)
libgstamlvsink_la_LIBADD += -L$(TARGET_DIR)/usr/lib -L$(TARGET_DIR)/usr/lib/gstreamer-1.0 -lamlavsync -ldrm_meson -lgstamlhalasink
libgstamlvsink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
#noinst_HEADERS = 
