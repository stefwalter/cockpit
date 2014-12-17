/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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

define("shell/disco", [
    "jquery",
    "latest/cockpit",
], function($, cockpit) {
    "use strict";

    function Disco() {
        var self = this;

        var store = { };
        var keys = { };
        var happened = { };
        var running = [ ];
        var seed = 0;

        self.machines = [ ];
        self.alerts = [ ];

        self.lookup = function lookup(key) {
            return keys[key];
        };

        function add_module(address, module) {
            var unique = address + "/" + seed;
            seed += 1;

            var res = module.disco(address, function(data) {
                store[unique] = data;
                disco(top(), self.machines);
            });

            running.push(res);
        }

        self.dance = function dance(address, module) {

            /* Add just this one module */
            if (module) {
                add_module(address, module);
                return;
            }

            /* Add all possible modules for this address */
            cockpit.packages.all(false).done(function(packages) {
                $.each(packages, function(i, pkg) {
                    if (!pkg.manifest.disco)
                        return;
                    $.each(pkg.manifest.disco, function(i, module_name) {
                        var module_id = pkg.name + "/" + module_name;

                        /*
                         * The interface with the discovery module is very sparse and needs
                         * to be backwards compatible. It is documented in doc/discovery.md
                         */

                        /* TODO: No way to do this for other hosts yet */
                        require([module_id], function(module) {
                            add_module(address, module);
                        });
                    });
                });
            });
        };

        self.stop = function stop() {
            $.each(running, function(i, res) {
                res.close();
            });
        };

        function top() {
            var i, j, data, layers = [];
            for (i in store) {
                data = store[i];
                for (j in data)
                    layers.push(data[j]);
            }
            return layers;
        }

        function alert_sink(key, alerts) {
            var now = new Date().getTime();
            var count = 0;

            $.each(alerts, function(x, ev) {
                if (!ev.id) {
                    ev.id = now + ":" + seed;
                    seed++;
                }
                if (happened[ev.id]) {
                    ev = happened[ev.id];
                } else {
                    self.alerts.push(ev);
                    happened[ev.id] = true;
                    count += 1;
                }
                if (ev.key != key) {
                    ev.key = key;
                    count += 1;
                }
            });

            return count;
        }

        function disco(layers, machines, parent) {
            var seen = { };
            var added = { };
            var combine = { };
            var stage = { };

            machines.length = 0;

            var alerted = 0;

            if (!parent)
                keys = { };

            /* Group everything by address or machine id */
            $.each(layers, function(i, layer) {
                var key = layer.id || layer.address;
                if (key) {
                    if (!stage[key])
                        stage[key] = [];
                    stage[key].push(layer);
                }
                if (layer.id && layer.address)
                    combine[layer.address] = layer.id;
            });

            /* Combine address and machine id if possible */
            $.each(combine, function(one, two) {
                if (stage[one] && stage[two]) {
                    stage[two].push.apply(stage[two], stage[one]);
                    delete stage[one];
                }
            });

            $.each(stage, function(key, staged) {
                var machine = { key: "m:" + key, machines: { }, objects: [ ], problems: [ ] };
                machines.push(machine);
                keys[machine.key] = machine;

                $.each(staged, function(x, layer) {
                    if (layer.address)
                        keys["m:" + layer.address] = machine;
                });

                /*
                 * TODO: A custom extend method could tell us if something actually
                 * changed, and avoid copying objects which should be unique already
                 */
                staged.unshift(true, machine);
                $.extend.apply($, staged);

                /*
                 * Squash any child machines recursively. This is already a copy
                 * due to the deep extend above, so no worries about messing with the
                 * data
                 */
                if (machine.machines) {
                    var values = Object.keys(machine.machines).map(function(i) { return machine.machines[i]; });
                    var children = [ ];
                    disco(values, children, machine);
                    machine.machines = children;
                }

                /* Normalize the machine a bit */
                if (machine.problems && machine.problems.length)
                    machine.state = "failed";
                if (!machine.label)
                    machine.label = machine.address || "";
                if (machine.problems.length && !machine.message)
                    machine.message = machine.problems.map(shell.client_error_description);
                if (machine.state && !machine.message)
                    machine.message = machine.state;

                /* Bring in alerts */
                if (machine.alerts)
                    alerted += alert_sink(machine.key, machine.alerts);

                /* Squash and sort the machine's objects */
                machine.objects = Object.keys(machine.objects).map(function(i) { return machine.objects[i]; });
                machine.objects.sort(function(a1, a2) {
                    return (a1.label || "").localeCompare(a2.label || "");
                });
                $.each(machine.objects, function(i, object) {
                    object.key = "o:" + object.location;
                    object.machine = machine;
                    keys[object.key] = object;
                    if (object.alerts)
                        alerted += alert_sink(object.key, object.alerts);
                });
            });

            /* Sort any machines */
            machines.sort(function(a1, a2) {
                return (a1.label || "").localeCompare(a2.label || "");
            });

            if (!parent)
                $(self).triggerHandler("changed");
            if (alerted > 0)
                $(self).triggerHandler("alerts");
        }

        /*
         * And lastly (re)discover any machines for which we don't have a
         * machine-id. We load them after the fact and overlay this info.
         */
        store["machine-ids"] = { };
        $(self).on("changed", function() {
            $.each(self.machines, function(i, machine) {
                if (machine.id || !machine.address || machine.masked)
                    return;

                var addr = machine.address;
                if (addr in store["machine-ids"])
                    return;

                store["machine-ids"][addr] = { address: addr, problems: [] };

                /*
                 * TODO: Migrate this to cockpit.file() once that lands. In addition
                 * using the cockpit.file() machinery and superuser channels we can
                 * actually write a file here atomically if it doesn't already exist.
                 */

                var channel = cockpit.channel({ payload: "fsread1",
                                                host: addr,
                                                path: "/etc/machine-id" });
                var data = channel.buffer(null);
                $(channel).on("close", function(event, options) {
                    if (options.problem == "not-found") {
                        console.warn("no /etc/machine-id in host");
                    } else if (options.problem) {
                        console.warn(addr + ": couldn't get machine uuid: " + options.problem);
                        store["machine-ids"][addr].problems.push(options.problem);
                    } else {
                        store["machine-ids"][addr].id = $.trim(data.squash());
                    }

                    disco(top(), self.machines);
                });
            });
        });
    }

    return Disco;
});
