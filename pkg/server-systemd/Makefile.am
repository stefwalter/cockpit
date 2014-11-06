serversystemddir = $(pkgdatadir)/server-systemd
serversystemd_DATA = \
	pkg/server-systemd/terminal.html \
	pkg/server-systemd/manifest.json \
	$(NULL)

noinst_DATA += \
	pkg/server-systemd/terminal.min.html \
	$(NULL)

serversystemd_TESTS = \
	pkg/shell/test-journal-renderer.html \
	$(NULL)

TESTS += $(serversystemd_TESTS)

CLEANFILES += \
	pkg/server-systemd/terminal.min.html \
	$(NULL)

EXTRA_DIST += \
	$(serversystemd_DATA) \
	$(NULL)
