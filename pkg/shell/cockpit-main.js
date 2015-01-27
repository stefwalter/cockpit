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

/* MAIN

   - client = shell.dbus(address, [options])
   - client.release()

   Manage the active D-Bus clients.  The 'dbus' function returns a
   client for the given 'address' with the given 'options'.  Use
   'client.release' to release it.  Typically, clients are gotten in
   the 'enter' method of a page and released again in 'leave'.

   A client returned by 'dbus' can be in any state; it isn't
   necessarily "ready".

   Clients stay around a while after they have been released by its
   last user.
*/

/* global jQuery   */
/* global cockpit  */

/* global phantom_checkpoint */

var shell = shell || { };
var modules = modules ||  { };

(function($, cockpit, shell, modules) {

shell.pages = [];
shell.dialogs = [];

var visited_dialogs = {};

shell.dbus = dbus;

/* Initialize cockpit when page is loaded and modules available */
var loaded = false;
modules.loaded = false;

function maybe_init() {
    if (loaded && modules.loaded)
        init();
}

/* HACK: Until all of the shell is loaded via AMD */
require([
    "server/server",
    "docker/docker"
], function(server, docker) {
    modules.server = server;
    modules.docker = docker;
    modules.loaded = true;
    maybe_init();
});

function init() {
    $('.dropdown-toggle').dropdown();
    content_init();
    content_show();
}

require(["latest/po"], function(po) {
    cockpit.locale(po);
});

var dbus_clients = shell.util.make_resource_cache();

function make_dict_key(dict) {
    function stringify_elt(k) { return JSON.stringify(k) + ':' + JSON.stringify(dict[k]); }
    return Object.keys(dict).sort().map(stringify_elt).join(";");
}

function dbus(address, options) {
    return dbus_clients.get(make_dict_key($.extend({host: address}, options)),
                            function () { return shell.dbus_client(address, options); });
}

/* current_params are the navigation parameters of the current page,
 * for example:
 *
 *   { host: "localhost",
 *     path: [ "storage-details" ],
 *     options: { type: "block", id: "/dev/vda" }
 *   }
 *
 * current_page_element is the HTML element for the current page.  For
 * modern pages, it is a iframe.
 *
 * current_legacy_page is the Page object for the current page if it
 * is a legacy page.  TODO: Remove this once there are no legacy pages
 * anymore.
 */
var current_params;
var current_page_element;
var current_legacy_page;

function content_init() {
    var current_visible_dialog = null;
    var pages = $('#content > div');
    pages.each (function (i, p) {
        $(p).hide();
    });
    current_params = null;

    $('div[role="dialog"]').on('show.bs.modal', function() {
        current_visible_dialog = $(this).attr("id");
        dialog_enter($(this).attr("id"));
    });
    $('div[role="dialog"]').on('shown.bs.modal', function() {
        dialog_show($(this).attr("id"));
    });
    $('div[role="dialog"]').on('hidden.bs.modal', function() {
        current_visible_dialog = null;
        dialog_leave($(this).attr("id"));
    });

    /* For legacy pages
     */
    $('#content-navbar a[data-page-id]').click(function () {
        cockpit.location.go([ cockpit.location.path[0], $(this).attr('data-page-id') ]);
    });

    /* For statically registered components
     */
    $('#content-navbar a[data-task-id]').click(function () {
        cockpit.location.go([ cockpit.location.path[0], $(this).attr('data-task-id') ]);
    });

    $(cockpit).on('locationchanged', function () {
        if (current_visible_dialog)
            $('#' + current_visible_dialog).modal('hide');
        display_location();
    });

    $(window).on('resize', function () {
        recalculate_layout();
    });

    shell.content_refresh();
    $('.selectpicker').selectpicker();

    hosts_init();
}

function content_show() {
    $("#content-navbar").show();
    display_location();
    phantom_checkpoint();
}

shell.content_refresh = function content_refresh() {
    if (current_params)
        display_params(current_params);
};

function recalculate_layout() {
    var $topnav = $('#topnav');
    var $sidebar = $('#cockpit-sidebar');
    var $extra = $('#shell-header-extra');
    var $body = $('body');

    var window_height = $(window).height();
    var topnav_width = $topnav.width();
    var topnav_height = $topnav.height()+2;
    var sidebar_width = $sidebar.is(':visible')? $sidebar.width() : 0;
    var extra_height = $extra.height();

    $sidebar.css('top', topnav_height);
    $sidebar.css('height', window_height - topnav_height);
    $extra.css('top', topnav_height);
    $extra.css('left', sidebar_width);
    $extra.css('width', topnav_width - sidebar_width);

    $body.css('padding-top', topnav_height + extra_height);
    $body.css('padding-left', sidebar_width);

    if (current_page_element && !current_legacy_page)
        current_page_element.height(window_height - topnav_height - extra_height);

    // This is mostly for the plots.
    $(cockpit).trigger('resize');
}

var local_account_proxies;

/* Information for each host, keyed by address.  hosts[addr] is an
 * object with at least these fields:
 *
 * - display_name
 * - avatar
 * - color
 * - state
 * - problem
 * - cockpitd
 * - compare(other_host_object)
 * - set_display_name(name)
 * - set_avatar(avatar)
 * - set_color(color)
 * - reconnect()
 * - show_problem_dialog()
 * - set_active()
 * - remove()
 *
 * $(shell.hosts).on("added removed changed", function (addr) { });
 */
shell.hosts = { };

shell.host_setup = function host_setup() {
    $('#dashboard_setup_server_dialog').modal('show');
};

shell.host_colors = [
    "#0099d3",
    "#67d300",
    "#d39e00",
    "#d3007c",
    "#00d39f",
    "#00d1d3",
    "#00618a",
    "#4c8a00",
    "#8a6600",
    "#9b005b",
    "#008a55",
    "#008a8a",
    "#00b9ff",
    "#7dff00",
    "#ffbe00",
    "#ff0096",
    "#00ffc0",
    "#00fdff",
    "#023448",
    "#264802",
    "#483602",
    "#590034",
    "#024830",
    "#024848"
];

shell.default_permission = cockpit.permission({ group: "wheel" });

function pick_unused_host_color() {
    function in_use(color) {
        var norm = $.color.parse(color).toString();
        for (var a in shell.hosts) {
            var h = shell.hosts[a];
            if (h.color && $.color.parse(h.color).toString() == norm)
                return true;
        }
        return false;
    }

    for (var i = 0; i < shell.host_colors.length; i++) {
        if (!in_use(shell.host_colors[i]))
            return shell.host_colors[i];
    }
    return "gray";
}

function update_servers_privileged() {
    shell.update_privileged_ui(
        shell.default_permission, ".servers-privileged",
        cockpit.format(
            _("The user <b>$0</b> is not permitted to add servers"),
            cockpit.user.name)
    );
}

function hosts_init() {

    var host_info = shell.hosts;
    var host_proxies;

    function update() {
        var want = { };
        for (var path in host_proxies) {
            var h = host_proxies[path];
            if (shell.find_in_array(h.Tags, "dashboard")) {
                want[h.Address] = h;
                if (!host_info[h.Address]) {
                    add_host(h.Address, h);
                    $(shell.hosts).trigger('added', [ h.Address ]);
                }
            }
        }
        for (var addr in host_info) {
            if (!want[addr]) {
                host_info[addr]._removed();
                delete host_info[addr];
                $(shell.hosts).trigger('removed', [ addr ]);
            }
        }
    }

    function remember_last_params() {
        var old_info = host_info[current_params.host];
        if (old_info && current_params.path[0] != "dashboard") {
            old_info.last_path = current_params.path;
            old_info.last_options = current_params.options;
        }
    }

    function add_host(addr, proxy) {
        var client = cockpit.dbus("com.redhat.Cockpit", { host: addr, bus: "session", track: true });
        var manager = client.proxy("com.redhat.Cockpit.Manager",
                                   "/com/redhat/Cockpit/Manager");

        var link, hostname_span, avatar_img;

        var info = { display_name: null,
                     avatar: null,
                     color: null,
                     state: "connecting",
                     cockpitd: client,
                     address: addr,

                     compare: compare,
                     set_display_name: set_display_name,
                     set_avatar: set_avatar,
                     set_color: set_color,
                     reconnect: reconnect,
                     show_problem_dialog: show_problem_dialog,
                     set_active: set_active,
                     remove: remove,

                     _removed: _removed
                   };

        function compare(other) {
            return info.display_name.localeCompare(other.display_name);
        }

        function remove() {
            proxy.RemoveTag("dashboard").
                fail(function (error) {
                    cockpit.show_unexpected_error(error);
                });
        }

        link = info._element = $('<a class="list-group-item">').
            append(
                avatar_img = $('<img width="32" height="32" class="host-avatar">'),
                hostname_span = $('<span>')).
            click(function () {
                if (info.state == "failed")
                    show_problem_dialog();
                else {
                    remember_last_params();
                    cockpit.location.go([ addr ].concat(info.last_path || [ "server" ]), info.last_options);
                }
            });

        function update_hostname() {
            var name;
            if (manager.valid)
                name = shell.util.hostname_for_display(manager);
            else if (proxy.Name)
                name = proxy.Name;
            else
                name = addr;
            if (name != info.display_name) {
                info.display_name = name;
                hostname_span.text(info.display_name);
                update_global_nav();
                show_hosts();
            }
        }

        function update_from_proxy() {
            if (proxy.Color)
                info.color = proxy.Color;
            else if (info.color === null) {
                info.color = pick_unused_host_color();
                proxy.SetColor(info.color).
                    fail(function (error) {
                        console.warn(error);
                    });
            }

            info.avatar = proxy.Avatar;
            if (info.state != "failed")
                avatar_img.attr('src', info.avatar || "images/server-small.png");
            update_hostname();
        }

        function update_from_manager() {
            var name = shell.util.hostname_for_display(manager);
            if (name != proxy.Name)
                proxy.SetName(name);
            update_hostname();
        }

        function set_active() {
            $('#hosts > a').removeClass("active");
            link.addClass("active");
        }

        function set_display_name(name) {
            if (manager.valid)
                return manager.SetHostname(name, manager.StaticHostname, {});
            else
                return $.Deferred().reject("not connected").promise();
        }

        function set_avatar(data) {
            return proxy.SetAvatar(data);
        }

        function set_color(color) {
            return proxy.SetColor(color);
        }

        function reconnect() {
            _removed();
            delete host_info[addr];
            $(shell.hosts).trigger('removed', [ addr ]);
            add_host(addr, proxy);
            $(shell.hosts).trigger('added', [ addr ]);
        }

        function show_problem_dialog() {
            $('#reconnect-dialog-summary').text(
                cockpit.format(_("Couldn't establish connection to $0."), info.display_name));
            $('#reconnect-dialog-problem').text(cockpit.message(info.problem));
            $('#reconnect-dialog-reconnect').off('click');
            $('#reconnect-dialog-reconnect').on('click', function () {
                $('#reconnect-dialog').modal('hide');
                reconnect();
            });
            $('#reconnect-dialog').modal('show');
        }

        function _removed() {
            link.remove();
            $(manager).off('.hosts');
            client.close();
        }

        host_info[addr] = info;

        $(client).on("close", function (event, problem) {
            info.state = "failed";
            info.problem = problem;
            avatar_img.attr('src', "images/server-error.png");
            $(shell.hosts).trigger('changed', [ addr ]);
        });

        $(proxy).on('changed', function (event, props) {
            if ("Color" in props || "Avatar" in props || "Name" in props) {
                update_from_proxy();
                $(shell.hosts).trigger('changed', [ addr ]);
            }
        });
        update_from_proxy();

        manager.wait(function () {
            if (manager.valid) {
                info.state = "connected";
                $(manager).on('changed.hosts', function (event, props) {
                    if ("PrettyHostname" in props || "StaticHostname" in props) {
                        update_from_manager();
                        $(shell.hosts).trigger('changed', [ addr ]);
                    }
                });
                update_from_manager();
            }
        });

        show_hosts();
    }

    var all_hosts = $('<a class="list-group-item">').
        append(
            $('<button class="btn btn-primary servers-privileged" data-container="body" data-placement="bottom"  style="float:right">').
                text("+").
                click(function () {
                    shell.host_setup();
                    return false;
                }),
            $('<span>').
                text("All Servers")).
        click(function () {
            remember_last_params();
            cockpit.location.go([]);
        });

    function show_hosts() {
        var sorted_hosts = (Object.keys(host_info).
                            map(function (addr) { return host_info[addr]; }).
                            sort(function (h1, h2) {
                                return h1.compare(h2);
                            }));
        $('#hosts').append(
            all_hosts,
            sorted_hosts.map(function (h) { return h._element; }));
        update_servers_privileged();
    }

    $('#hosts-button').click(function () {
        $('#cockpit-sidebar').toggle();
        recalculate_layout();
    });

    local_account_proxies = null;

    var cockpitd = cockpit.dbus("com.redhat.Cockpit", { host: "localhost", bus: "session", track: true });
    local_account_proxies = cockpitd.proxies("com.redhat.Cockpit.Account",
                                             "/com/redhat/Cockpit/Accounts");
    host_proxies = cockpitd.proxies("com.redhat.Cockpit.Machine",
                                    "/com/redhat/Cockpit/Machines");
    host_proxies.wait(function () {
        $(host_proxies).on('added removed changed', update);
        update();
    });


    $(shell.default_permission).on("changed", update_servers_privileged);
    update_servers_privileged();
}

function update_global_nav() {
    var info;
    var hostname = null;
    var page_title = null;

    if (current_params)
        info = shell.hosts[current_params.host];
    if (info) {
        hostname = info.display_name;
        info.set_active();
    }

    $('#content-navbar-hostname').text(hostname);

    $('#content-navbar > li').removeClass('active');
    if (current_legacy_page) {
        var section_id = current_legacy_page.section_id || current_legacy_page.id;
        page_title = current_legacy_page.getTitle();
        $('#content-navbar > li').has('a[data-page-id="' + section_id + '"]').addClass('active');
    } else if (current_page_element) {
        // TODO: change notification
        if (current_page_element[0].contentDocument)
            page_title = current_page_element[0].contentDocument.title;
        // TODO: hmm...
        var task_id = current_params.path[0];
        $('#content-navbar > li').has('a[data-task-id="' + task_id + '"]').addClass('active');
    }

    var doc_title;
    if (hostname && page_title)
        doc_title = hostname + " — " + page_title;
    else if (page_title)
        doc_title = page_title;
    else if (hostname)
        doc_title = hostname;
    else
        doc_title = _("Cockpit");

    document.title = doc_title;

    recalculate_layout();
}

/* A map from path prefixes such as [ "tools" "terminal" ] to
 * component description objects.  Such an object has the following
 * fields:
 *
 * - pkg
 * - entry
 *
 * The package to load and the file of that package to use as the
 * entry point.
 */
var components = { };

/* A map from host-plus-path-prefixes to page <iframe> objects.
 */
var page_iframes = { };

/* A map from id to the page object, for legacy pages that
 * have been visited already.
 */
var visited_legacy_pages = { };

function register_component(prefix, package_name, entry) {
    var key = JSON.stringify(prefix);
    components[key] = { pkg: package_name, entry: entry };
}

function register_tool(ident, label) {
    var link = $("<a>").attr("data-task-id", ident).text(label);
    $("<li>").append(link).appendTo("#tools-menu");
}

/* HACK: Mozilla will unescape 'location.hash' before returning
 * it, which is broken.
 *
 * https://bugzilla.mozilla.org/show_bug.cgi?id=135309
 */
function get_location_hash(location) {
    return location.href.split('#')[1] || '';
}

function legacy_page_from_id(id) {
    var n;
    for (n = 0; n < shell.pages.length; n++) {
        if (shell.pages[n].id == id)
            return shell.pages[n];
    }
    return null;
}

/* The display_params function is the main place where we deal with legacy
 * pages.  TODO: Remove that.
 */

function display_params(params) {
    var element = null;
    var legacy_page = null;

    /* Try to find a legacy page for path[0].
     */
    var id = params.path[0];
    legacy_page = visited_legacy_pages[id];
    if (!legacy_page) {
        legacy_page = legacy_page_from_id(id);
        if (legacy_page) {
            if (legacy_page.setup)
                legacy_page.setup();
            visited_legacy_pages[id] = legacy_page;
        }
    }
    if (legacy_page)
        element = $('#' + id);

    if (!element) {
        cockpit.location.go([ "server" ]);
        return;
    }

    var old_element = current_page_element;
    var old_legacy_page = current_legacy_page;

    if (old_legacy_page)
        old_legacy_page.leave();

    $('#shell-header-extra').empty();
    current_params = params;
    current_page_element = element;
    current_legacy_page = legacy_page;
    if (legacy_page)
        legacy_page.enter();

    if (old_element)
        old_element.hide();
    update_global_nav();
    element.show();
    if (legacy_page)
        legacy_page.show();
}

function display_location() {
    var host = cockpit.location.path[0];
    var path = cockpit.location.path.slice(1);
    var options = cockpit.location.options;

    if (!host || host == "local")
        host = "localhost";
    if (path.length < 1)
        path = [ "dashboard" ];

    display_params({ host: host, path: path, options: options });
}

function dialog_from_id(id) {
    var n;
    for (n = 0; n < shell.dialogs.length; n++) {
        if (shell.dialogs[n].id == id)
            return shell.dialogs[n];
    }
    return null;
}

function dialog_enter(id) {
    var dialog = dialog_from_id(id);
    var first_visit = true;
    if (visited_dialogs[id])
        first_visit = false;

    if (dialog) {
        if (first_visit && dialog.setup)
            dialog.setup();
        dialog.enter();
    }
    visited_dialogs[id] = true;
    phantom_checkpoint ();
}

function dialog_leave(id) {
    var dialog = dialog_from_id(id);
    if (dialog) {
        dialog.leave();
    }
    phantom_checkpoint ();
}

function dialog_show(id) {
    var dialog = dialog_from_id(id);
    if (dialog) {
        dialog.show();
    }
    phantom_checkpoint ();
}

shell.get_page_param = function get_page_param(key) {
    return current_params.options[key];
};

shell.get_page_machine = function get_page_machine() {
    if (current_params)
        return current_params.host;
    return null;
};

shell.set_page_param = function set_page_param(key, val) {
    if (val) {
        if (val == current_params.options[key])
            return;
        current_params.options[key] = val;
    } else {
        if (current_params.options[key] === undefined)
            return;
        delete current_params.options[key];
    }
    cockpit.location.replace(cockpit.location.path, current_params.options);
    current_params.options = cockpit.location.options;
};

shell.show_error_dialog = function show_error_dialog(title, message) {
    if (message) {
        $("#error-popup-title").text(title);
        $("#error-popup-message").text(message);
    } else {
        $("#error-popup-title").text(_("Error"));
        $("#error-popup-message").text(title);
    }

    $('.modal[role="dialog"]').modal('hide');
    $('#error-popup').modal('show');
};

shell.show_unexpected_error = function show_unexpected_error(error) {
    shell.show_error_dialog(_("Unexpected error"), error.message || error);
};

shell.confirm = function confirm(title, body, action_text) {
    var deferred = $.Deferred();

    $('#confirmation-dialog-title').text(title);
    if (typeof body == "string")
        $('#confirmation-dialog-body').text(body);
    else
        $('#confirmation-dialog-body').html(body);
    $('#confirmation-dialog-confirm').text(action_text);

    function close() {
        $('#confirmation-dialog button').off('click');
        $('#confirmation-dialog').modal('hide');
    }

    $('#confirmation-dialog-confirm').click(function () {
        close();
        deferred.resolve();
    });

    $('#confirmation-dialog-cancel').click(function () {
        close();
        deferred.reject();
    });

    $('#confirmation-dialog').modal('show');
    return deferred.promise();
};

})(jQuery, cockpit, shell, modules);

function N_(str) {
    return str;
}

var _ = cockpit.gettext;
var C_ = cockpit.gettext;
