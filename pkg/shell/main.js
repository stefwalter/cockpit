/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

define([
    "jquery",
    "latest/cockpit",
], function($, cockpit) {
    var main = { };

    function Machines(list) {
        var self = this;

        /* TODO: This should be migrated away from cockpitd */

        var client = cockpit.dbus("com.redhat.Cockpit", { host: "localhost", bus: "session", track: true });
        var proxies = client.proxies("com.redhat.Cockpit.Machine", "/com/redhat/Cockpit/Machines");

        function hostname_label(name) {
            if (!name || name == "localhost" || name == "localhost.localdomain")
                return window.location.hostname;
            return name;
        }

        function update() {
            var had = { };
            var machine;

            while(list.length > 0) {
                machine = list.pop();
                had[machine.address] = machine;
            }

            $.each(proxies, function(i, proxy) {
                machine = had[proxy.Address];
                if (!machine)
                    machine = { };
                machine.address = proxy.Address;
                machine.label = hostname_label(proxy.Name || proxy.Address);
                machine.color = proxy.Color;
                machine.avatar = proxy.Avatar;
                list.push(machine);
            });

            list.sort(function(m1, m2) {
                return m2.label.localeCompare(m2.label);
            });

            $(self).triggerHandler("changed");
        }

        proxies.wait(function() {
            $(hosts).on('added removed changed', update);
            update();
        });
    }

    function Components(list) {

        /* TODO: We should remove the hard coded settings here */

        list.push.apply([
            {
                path: null,
                label: _("System"),
                src: "shell/shell.html"
            },
            {
                path: "services",
                label: _("Services"),
                src: "shell/shell.html"
            },
            {
                path: "containers",
                label: _("Containers")
                src: "shell/shell.html"
            },
            {
                path: "journal",
                label: _("Journal"),
                src: "shell/shell.html"
            },
            {
                path: "networking",
                label: _("Networking"),
                src: "shell/shell.html"
            },
            {
                path: "storage",
                label: _("Storage"),
                src: "shell/shell.html"
            },
            {
                path: "accounts",
                label: _("Administrator Accounts"),
                src: "shell/shell.html"
            }
        ]);

        /* TODO: This needs to come from a different host */

        cockpit.packages.all(false).
            done(function(pkgs) {
                var l = $.map(pkgs, function(pkg) { return pkg; });
                l.sort(function(a, b) {
                    return a.name == b.name ? 0 : a.name < b.name ? -1 : 1;
                });

                var seen = { };
                $.each(l, function(i, pkg) {
                    var tools = pkg.manifest.tools;
                    if (!tools)
                        return;
                    $.each(tools, function(ident, info) {
                        if (seen[ident])
                            return;
                        list.push({
                            path: ident,
                            label: locale.gettext(info.label),
                            src: pkg.name + "/" + info.path;
                        });
                        seen[ident] = ident;
                        register_component([ ident ], pkg.name, info.path);
                        register_tool(ident, info.label);
                    });
                });

                maybe_init();
            })
            .fail(function(ex) {
                console.warn("Couldn't load package info: " + ex);
            })
            .always(function() {
                $(self).triggerHandler("changed");
            });
    }

    main.disco = function disco(callback, only) {

    };

    return main;
});
