serverdir = $(pkgdatadir)/server
server_DATA = \
	pkg/server/terminal.html \
	pkg/server/dashboard.html \
	pkg/server/dashboard.js \
	pkg/server/manifest.json \
	$(NULL)

noinst_DATA += \
	pkg/server/terminal.min.html \
	$(NULL)

CLEANFILES += \
	pkg/server/terminal.min.html \
	$(NULL)

EXTRA_DIST += \
	$(server_DATA) \
	$(NULL)
