/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
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

require([
    "jquery",
    "cockpit/latest/cockpit"
], function($, cockpit) {
"use strict";

function DockerLogs(parent, container_id) {
    var self = this;

    var channel;
    var pre = $("<pre class='logs'>");
    $(parent).empty().append(pre);

    var scroll;
    function write(data) {
        var span = $("<span>").text(data);
        pre.append(span);
        if (!scroll) {
            scroll = window.setTimeout(function() {
                pre.scrollTop(pre.prop("scrollHeight"));
            }, 50);
        }
    }

    /*
     * A raw channel over which we speak Docker's even stranger /logs
     * protocol. It starts with a HTTP GET, and then quickly
     * degenerates into a stream with framing. That framing is
     * unreadable from javascript, so we use a C helper to pre-digest
     * it a bit.
     */
    function attach() {
        channel = cockpit.channel({
            "host": machine,
            "payload": "stream",
            "binary": true,
            "unix": "/var/run/docker.sock"
        });

        var buffer = "";
        var headers = false;
        self.connected = true;

        $(channel).
            on("close.logs", function(ev, options) {
                write(options.reason || "disconnected");
                self.connected = false;
                $(channel).off("close.logs");
                $(channel).off("message.logs");
                channel = null;
            }).
            on("message.logs", function(ev, payload) {
                buffer += payload;
                /* Look for end of headers first */
                if (!headers) {
                    var pos = buffer.indexOf("\r\n\r\n");
                    if (pos == -1)
                        return;
                    headers = true;
                    buffer = buffer.substring(pos + 2);
                }

                /* Once headers are done then it's {1|2} base64 */
                var lines = buffer.split("\n");
                var last = lines.length - 1;
                $.each(lines, function(i, line) {
                    if (i == last) {
                        buffer = line;
                        return;
                    }
                    if (!line)
                        return;
                    var pos = line.indexOf(" ");
                    if (pos === -1)
                        return;
                    write(window.atob(line.substring(pos + 1)));
                });
            });

        var req =
            "POST /v1.10/containers/" + container_id + "/attach?logs=1&stream=1&stdin=1&stdout=1&stderr=1 HTTP/1.0\r\n" +
            "Content-Length: 0\r\n" +
            "\r\n";
        channel.send(req);
    }

    attach();

    /* Allows caller to cleanup nicely */
    this.close = function close() {
        if (self.connected)
            channel.close(null);
    };

    return this;
}

});
