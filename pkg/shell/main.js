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
    var _ = cockpit.gettext;

    var main = { };

    function Machines(host, list) {
        var self = this;

        /* TODO: This should be migrated away from cockpitd */

        var client = cockpit.dbus("com.redhat.Cockpit", { host: host, bus: "session", track: true });
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

            self.loaded = true;
            $(self).triggerHandler("changed");
        }

        proxies.wait(function() {
            $(proxies).on('added removed changed', update);
            update();
        });

        self.loaded = true;

        self.close = function close() {
            if (proxies)
                $(proxies).off();
            if (client)
                client.close();
        };
    }

    function Components(host, list) {
        var self = this;
        var closed = false;

        /* TODO: We should remove the hard coded settings here */

        list.push.apply(list, [
            {
                path: "system/host",
                label: _("System"),
            },
            {
                path: "system/init",
                label: _("Services"),
            },
            {
                path: "docker/containers",
                label: _("Containers"),
            },
            {
                path: "server/log",
                label: _("Journal"),
            },
            {
                path: "network/interfaces",
                label: _("Networking"),
            },
            {
                path: "storage/devices",
                label: _("Storage"),
            },
            {
                path: "users/local",
                label: _("Administrator Accounts"),
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
                        if (!seen[ident]) {
                            list.push({
                                path: pkg.name + "/" + ident,
                                label: cockpit.gettext(info.label)
                            });
                        }
                    });
                });
            })
            .fail(function(ex) {
                console.warn("Couldn't load package info: " + ex);
            })
            .always(function() {
                self.loaded = true;
                if (!closed)
                    $(self).triggerHandler("changed");
            });

        self.loaded = false;

        self.close = function close() {
            closed = true;
        };
    }

    main.disco = function disco(host, what, callback) {
        var data = {
            machines: [ ],
            components: [ ],
        };

        /* What we should be loading by default */
        what = what || {
            machines: true,
            components: true,
        };

        /* Only trigger callback if all parts loaded */
        function update() {
            for (var i = 0; i < parts.length; i++) {
                if (!parts[i].loaded)
                    return;
            }
            callback(data);
        }

        var parts = [];
        if (what.machines)
            parts.push(new Machines(host, data.machines));

        if (what.components)
            parts.push(new Components(host, data.components));

        /* Listen for changes in all the parts */
        for (var i = 0; i < parts.length; i++)
            $(parts[i]).on("changed", update);

        return {
            close: function close() {
                for (var i = 0; i < parts.length; i++)
                    parts[i].close();
                parts = [ ];
            }
        };
    };

    return main;
});
