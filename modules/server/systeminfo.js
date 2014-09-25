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

(function(cockpit, $) {

/* TODO: Localization */
function C_(x, y) { return y; }
function _(x) { return x; }
function N_(x) { return x; }

/* TODO: some utilities */
function F(format, args) {
    return format.replace(/%\{([^}]+)\}/g, function(_, key) { return args[key] || ""; });
}
function esc(str) {
    if (str === null || str === undefined)
        return "";
    var pre = document.createElement('pre');
    var text = document.createTextNode(str);
    pre.appendChild(text);
    return pre.innerHTML.replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}

var address = "localhost"; /* TODO: */
var client;
var manager;
var realm_manager;

$(document).ready(function() {
    var realms;
    
    function system_info_update() {
        var joined = realm_manager.Joined;

        $("#realms-list").empty();

        if (joined === undefined) {
            $("#realms-empty-text").hide();
            return;
        }

        if (joined.length === 0) {
            $("#realms-empty-text").show();
            return;
        } else {
            $("#realms-empty-text").hide();
        }

        (function() {
            var name = joined[0][0];
            var details = joined[0][1];
            $("#realms-list").append(('<li class="list-group-item" id="domain-list">' +
                        esc(name) +
                        '<button class="btn btn-default realms-leave-button" id="realms-leave" style="float:right">' +
                        _("Leave") + '</button>' +
                        '<div class="realms-leave-spinner waiting" id="realms-leave-spinner style="float:right"/>' +
                        '</li>'));
            $("#realms-leave").off("click");
            $("#realms-leave").on("click", function(e) {
                $("#realms-leave-spinner" ).show();
                leave_realm(name, details);
            });
        })();
    }

    function system_info_update_busy() {
        var busy = realm_manager.Busy;

        if (busy && busy[0]) {
            $(".realms-leave-button").prop('disabled', true);
        } else {
            $(".realms-leave-button").prop('disabled', false);
            $(".realms-leave-spinner").hide();
        }
    }

    function leave_realm(name, details) {
        $("#realms-leave-error").text("");
        var options = { 'server-software': details['server-software'],
                        'client-software': details['client-software']
                      };
        realm_manager.call("Leave", name, [ 'none', '', '' ], options, function(error, result) {
            $(".realms-leave-spinner").hide();
            if (error) {
                if (error.name == 'com.redhat.Cockpit.Error.AuthenticationFailed') {
                    realm_op = 'leave';
                    realm_name = name;
                    realm_details = details;
                    $("#realms-op").modal('show');
                } else {
                    $("#realms-leave-error").text(error.message);
                }
            }
        });
    }

    /* TODO: This code needs to be migrated away from dbus-json1 */
    client = cockpit.dbus_client(address, { payload: 'dbus-json1' });

    realm_manager = client.get("/com/redhat/Cockpit/Realms", "com.redhat.Cockpit.Realms");
    $(realm_manager).on("notify:Joined.realms", system_info_update);
    $(realm_manager).on("notify:Busy.realms", system_info_update);

    $("#realms-leave-error").text("");
    system_info_update();
    system_info_update_busy();
    manager = client.get("/com/redhat/Cockpit/Manager", "com.redhat.Cockpit.Manager");

    function bindf(sel, object, prop, func) {
        function upd() {
            $(sel).text(func(object[prop]));
        }
        $(object).on('notify:' + prop + '.system-information', upd);
        upd();
    }

    function bind(sel, object, prop) {
        bindf(sel, object, prop, function (s) { return s; });
    }

    bind("#system_information_hardware_text", manager, "System");
    bind("#system_information_asset_tag_text", manager, "SystemSerial");
    bind("#system_information_bios_text", manager, "BIOS");
    bind("#system_information_os_text", manager, "OperatingSystem");

    function hostname_text() {
        var pretty_hostname = manager.PrettyHostname;
        var hostname = manager.Hostname;
        var str;
        if (!pretty_hostname || pretty_hostname == hostname)
            str = hostname;
        else
            str = pretty_hostname + " (" + hostname + ")";
        return str;
    }

    bindf("#system_information_hostname_text", manager, "Hostname", hostname_text);
    bindf("#system_information_hostname_text", manager, "PrettyHostname", hostname_text);

    function realms_text(val) {
        if (!val)
            return "?";
        var res = [ ];
        for (var i = 0; i < val.length; i++)
            res.push(val[i][0]);
        return res.join (", ");
    }

    function hide_buttons(val) {
        if (!val)
            return;
        if(val[0] === undefined){
            $(".realms-leave-spinner").hide();
            $("#realms-leave").hide();
            $("#realms-join").show();
        } else {
            $("#realms-join").hide();
            $("#realms-leave").show();
            $(".realms-leave-button").prop('disabled', false);
            $("#realms-op").modal('hide');
        }

    }

    realms = client.get("/com/redhat/Cockpit/Realms", "com.redhat.Cockpit.Realms");
    bindf("#system_information_realms", realms, "Joined", realms_text);

    $(realms).on('notify:Joined.system-information', function() {
        hide_buttons(realms['Joined']);
    });

    hide_buttons(realms['Joined']);

    $("#realms-join").click(function (e) {
        if(realms.Joined[0] === undefined) {
            realm_op = 'join';
            realm_name = '';
            realm_details = { };
            $('#realms-op').modal('show');
        }
    });

    $('#system_information_change_hostname_button').on('click', function() {
        $('#system_information_change_hostname').modal('show');
    });

    $("#system_information").show();
});

/* Hostname dialog */
$(document).ready(function() {
    var always_update_from_pretty = false;
    var initial_pretty_hostname;
    var initial_hostname;

    $("#sich-apply-button").on("click", function host_name_on_apply_button() {
        var new_full_name = $("#sich-pretty-hostname").val();
        var new_name = $("#sich-hostname").val();
        manager.call("SetHostname",
                new_full_name, new_name, {},
                function(error, reply) {
                    $("#system_information_change_hostname").modal('hide');
                    if(error) {
                        cockpit.show_unexpected_error(error);
                    }
                });
    });

    $("#sich-pretty-hostname").on("input", function host_name_on_full_name_changed() {
        /* Whenever the pretty host name has changed (e.g. the user has edited it), we compute a new
         * simple host name (e.g. 7bit ASCII, no special chars/spaces, lower case) from it...
         */
        var pretty_hostname = $("#sich-pretty-hostname").val();
        if (always_update_from_pretty || initial_pretty_hostname != pretty_hostname) {
            var old_hostname = $("#sich-hostname").val();
            var first_dot = old_hostname.indexOf(".");
            var new_hostname = pretty_hostname.toLowerCase().replace(/['".]+/g, "").replace(/[^a-zA-Z0-9]+/g, "-");
            new_hostname = new_hostname.substr(0, 64);
            if (first_dot >= 0)
                new_hostname = new_hostname + old_hostname.substr(first_dot);
            $("#sich-hostname").val(new_hostname);
            always_update_from_pretty = true; // make sure we always update it from now-on
        }
        host_name_update();
    });

    $("#sich-hostname").on("input", function host_name_on_name_changed() {
        host_name_update();
    });

    $('#system_information_change_hostname').on('show.bs.modal', function() {
        initial_hostname = manager.Hostname || "";
        initial_pretty_hostname = manager.PrettyHostname || "";
        $("#sich-pretty-hostname").val(initial_pretty_hostname);
        $("#sich-hostname").val(initial_hostname);

        always_update_from_pretty = false;
        host_name_update();

        $("#sich-pretty-hostname").focus();
    });

    function host_name_update() {
        var apply_button = $("#sich-apply-button");
        var note1 = $("#sich-note-1");
        var note2 = $("#sich-note-2");
        var changed = false;
        var valid = false;
        var can_apply = false;

        var charError = "Real host name can only contain lower-case characters, digits, dashes, and periods (with populated subdomains)";
        var lengthError = "Real host name must be 64 characters or less";

        var validLength = $("#sich-hostname").val().length <= 64;
        var hostname = $("#sich-hostname").val();
        var pretty_hostname = $("#sich-pretty-hostname").val();
        var validSubdomains = true;
        var periodCount = 0;

        for(var i=0; i < $("#sich-hostname").val().length; i++) {
            if($("#sich-hostname").val()[i] == '.')
                periodCount++;
            else
                periodCount = 0;

            if(periodCount > 1) {
                validSubdomains = false;
                break;
            }
        }

        var validName = (hostname.match(/[.a-z0-9-]*/) == hostname) && validSubdomains;

        if ((hostname != initial_hostname ||
            pretty_hostname != initial_pretty_hostname) &&
            (hostname !== "" || pretty_hostname !== ""))
            changed = true;

        if (validLength && validName)
            valid = true;

        if (changed && valid)
            can_apply = true;

        if (valid) {
            $(note1).css("visibility", "hidden");
            $(note2).css("visibility", "hidden");
            $("#sich-hostname-error").removeClass("has-error");
        } else if (!validLength && validName) {
            $("#sich-hostname-error").addClass("has-error");
            $(note1).text(lengthError);
            $(note1).css("visibility", "visible");
            $(note2).css("visibility", "hidden");
        } else if (validLength && !validName) {
            $("#sich-hostname-error").addClass("has-error");
            $(note1).text(charError);
            $(note1).css("visibility", "visible");
            $(note2).css("visibility", "hidden");
        } else {
            $("#sich-hostname-error").addClass("has-error");

            if($(note1).text() === lengthError)
                $(note2).text(charError);
            else if($(note1).text() === charError)
                $(note2).text(lengthError);
            else {
                $(note1).text(lengthError);
                $(note2).text(charError);
            }
            $(note1).css("visibility", "visible");
            $(note2).css("visibility", "visible");
        }

        apply_button.prop('disabled', !can_apply);
    }
});

/* Join leave dialog */

var realm_op;
var realm_name;
var realm_details;

$(document).ready(function() {
    var never_show_software_choice;
    var realm_checking;
    var realm_checked;
    var realm_discovered_details;
    var realm_timeout;
    var realm_working;

    $('#realms-op').on('show.bs.modal', function() {
        $("#realms-op-diagnostics").hide();

        $(realm_manager).on("notify:Busy.realms-op", function() {
            realm_update_busy();
        });

        var title;
        if (realm_op == 'join') {
            never_show_software_choice = 1;
            title = C_("page-title", "Join a Domain");
            $("#realms-op-apply").text(_("Join"));
            $(".realms-op-join-only-row").show();
        } else if (realm_op == 'leave') {
            never_show_software_choice = 1;
            title = C_("page-title", "Leave Domain");
            $("#realms-op-apply").text(_("Leave"));
            $(".realms-op-join-only-row").hide();
        } else {
            $("#realms-op").modal('hide');
            return;
        }
    
        $("#realms-op-title").empty().append(title);

        $("#realms-op-spinner").hide();
        $("#realms-op-wait-message").hide();
        $("#realms-op-address-spinner").hide();
        $("#realms-op-address-error").hide();
        $("#realms-op-error").empty();
        $("#realms-op-diagnostics").empty();
        $(".realms-op-field").val("");

        realm_checking = 0;
        realm_checked = "";
        realm_discovered_details = [ ];

        if (realm_op == 'join') {
            realm_check_default();
            realm_maybe_check();
            realm_update_discovered_details();
        } else {
            realm_update_auth_methods();
        }

        realm_update_busy();
    });

    function realm_update_discovered_details() {
        var sel = $("#realms-op-software");
        sel.empty();
        for (var i = 0; i < realm_discovered_details.length; i++) {
            var d = realm_discovered_details[i];
            var txt = d['client-software'] + " / " + d['server-software'];
            sel.append('<option value="' + i + '">' + esc(txt) + '</option>');
        }
        realm_update_auth_methods();

        if (never_show_software_choice || realm_discovered_details.length < 2)
            $("#realms-op-software-row").hide();
        else
            $("#realms-op-software-row").show();
    }

    function realm_update_auth_methods() {
        var m = { };

        function add_from_details (d) {
            if (d) {
                var c = d['supported-join-credentials'];
                if (c) {
                    for (var i = 0; i < c.length; i++)
                        m[c[i]] = 1;
                }
            }
        }

        if (realm_op == 'leave') {
            add_from_details(realm_details);
        } else if (never_show_software_choice) {
            // Just merge them all and trust that realmd will do the
            // right thing.
            for (var i = 0; i < realm_discovered_details.length; i++)
                add_from_details(realm_discovered_details[i]);
        } else {
            var s = $("#realms-op-software").val();
            add_from_details(realm_discovered_details[s]);
        }

        var have_one = 0;
        var sel = $("#realms-op-auth");

        function add_choice (tag, text) {
            if (tag in m) {
                sel.append('<option value="' + tag + '">' + esc(text) + '</option>');
                have_one = 1;
            }
        }

        sel.empty();
        add_choice('admin', _('Administrator Password'));
        add_choice('user', _('User Password'));
        add_choice('otp', _('One Time Password'));
        add_choice('none', _('Automatic'));
        if (!have_one)
            sel.append('<option value="admin">' + _("Administrator Password") + '</option>');
        if($('[data-id="realms-op-auth"]').length <= 1)
            $("#realms-authentification-row").hide();
        else
            $("#realms-authentification-row").show();

        $('#realms-op-auth').selectpicker();
        realm_update_cred_fields();
    }

    function realm_update_cred_fields() {
        var a = $("#realms-op-auth").val();

        $("#realms-op-admin-row").hide();
        $("#realms-op-admin-password-row").hide();
        $("#realms-op-user-row").hide();
        $("#realms-op-user-password-row").hide();
        $("#realms-op-otp-row").hide();

        if (a == "admin") {
            $("#realms-op-admin-row").show();
            $("#realms-op-admin-password-row").show();
            var admin;
            if (realm_op == 'join') {
                var s = $("#realms-op-software").val();
                var d = s && realm_discovered_details[s];
                admin = d && d['suggested-administrator'];
            } else {
                admin = realm_details['suggested-administrator'];
            }
            if (admin && !$("#realms-op-admin").val())
                $("#realms-op-admin")[0].placeholder = _("e.g. \""+admin+"\"");
        } else if (a == "user") {
            $("#realms-op-user-row").show();
            $("#realms-op-user-password-row").show();
        } else if (a == "otp") {
            $("#realms-op-otp-row").show();
        }
    }

    function realm_update_busy() {
        var busy = realm_manager.Busy;

        if (busy && busy[0]) {
            $("#realms-op-spinner").show();
            $("#realms-op-wait-message").show();
            $(".realms-op-field").prop('disabled', true);
            $("#realms-op-apply").prop('disabled', true);
            $("#realms-op-software").prop('disabled', true);
            $('[data-id="realms-op-auth"]').prop('disabled', true);
        } else {
            $("#realms-op-spinner").hide();
            $("#realms-op-wait-message").hide();
            $(".realms-op-field").prop('disabled', false);
            $('[data-id="realms-op-auth"]').prop('disabled', false);
            $("#realms-op-apply").prop('disabled', false);
            $("#realms-op-software").prop('disabled', false);
        }
    }

    function realm_check_default() {
        realm_manager.call("Discover", "", { },
                           function (error, result, details) {
                               if (result) {
                                   $("#realms-op-address")[0].placeholder =
                                       F(_("e.g. %{address}"), { address: result });
                               }
                           });
    }

    function realm_maybe_check() {
        if ($("#realms-op-address").val() != realm_checked) {
            $("#realms-op-address-error").hide();
            if (realm_timeout)
                clearTimeout(realm_timeout);
            realm_timeout = setTimeout(function () { realm_check_realm(); }, 1000);
        }
    }

    function realm_check_realm() {
        var name = $("#realms-op-address").val();

        if (realm_checking || !name || realm_checked == name) {
            return;
        }

        $("#realms-op-address-spinner").show();
        realm_checking = 1;
        realm_checked = name;

        realm_manager.call("Discover", name, { },
                           function (error, result, details) {
                               if ($("#realms-op-address").val() != realm_checked) {
                                   realm_checking = 0;
                                   realm_check_realm();
                               } else {
                                   $("#realms-op-address-spinner").hide();
                                   realm_checking = 0;
                                   realm_discovered_details = [ ];
                                   if (error)
                                       $("#realms-op-error").empty().append(error.message);
                                   else if (!result) {
                                       $("#realms-op-address-error").show();
                                       $("#realms-op-address-error").attr('title',
                                               F(_("Domain %{domain} could not be contacted"), { 'domain': esc(name) }));
                                   } else {
                                       realm_discovered_details = details;
                                   }
                                   realm_update_discovered_details();
                               }
                           });
    }

    function realm_apply() {
        function handle_op_result (error, result) {
            realm_working = false;
            if (error && error.name != "com.redhat.Cockpit.Error.Cancelled") {
                $("#realms-op-error").empty().append(error.message);
                $("#realms-op-error").append((' <button id="realms-op-more-diagnostics" data-inline="true">' +
                            _("More") + '</button>'));
                $("#realms-op-more-diagnostics").click(function (e) {
                    realm_manager.call("GetDiagnostics",
                                       function (error, result) {
                                           $("#realms-op-more-diagnostics").hide();
                                           $("#realms-op-diagnostics").show();
                                           $("#realms-op-diagnostics").empty().append(result);
                                       });
                });
            } else {
                $("#realms-op").modal('hide');
            }
        }

        $("#realms-op-error").empty();
        $("#realms-op-diagnostics").empty();

        var a = $("#realms-op-auth").val();
        var creds;
        if (a == "user")
            creds = [ "user", $("#realms-op-user").val(), $("#realms-op-user-password").val() ];
        else if (a == "admin")
            creds = [ "admin", $("#realms-op-admin").val(), $("#realms-op-admin-password").val() ];
        else if (a == "otp")
            creds = [ "otp", "", $("#realms-op-ot-password").val() ];
        else
            creds = [ "none", "", "" ];

        var options;

        if (realm_op == 'join') {
            var details;
            if (never_show_software_choice)
                details = { };
            else {
                var s = $("#realms-join-software").val();
                details = realm_discovered_details[s];
            }

            options = { 'computer-ou': $("#realms-join-computer-ou").val() };

            if (details['client-software'])
                options['client-software'] = details['client-software'];
            if (details['server-software'])
                options['server-software'] = details['server-software'];

            realm_working = true;
            realm_manager.call("Join", $("#realms-op-address").val(), creds, options, handle_op_result);
        } else if (realm_op == 'leave') {
            options = { 'server-software': realm_details['server-software'],
                        'client-software': realm_details['client-software']
                      };
            realm_working = true;
            realm_manager.call("Leave", realm_name, creds, options, handle_op_result);
        }
    }

    $("#realms-op-cancel").on("click", function() {
        if (realm_working) {
            realm_manager.call("Cancel", function (error, result) { });
        } else {
            $("#realms-op").modal('hide');
        }
        $("#realms-op-spinner").hide();
        $("#realms-op-wait-message").hide();
    });

    $("#realms-op-apply").on("click", function() {
        realm_apply();
    });
    $("#realms-op-address").on("keyup", function() {
        realm_maybe_check();
    });
    $("#realms-op-address").on("change", function() {
        realm_maybe_check();
    });
    $(".realms-op-field").on("keydown", function(e) {
        if (e.which == 13)
            realm_apply();
    });
    $("#realms-op-auth").on('change', function() {
        realm_update_cred_fields();
    });
    $("#realms-op-software").on('change', function() {
        realm_update_auth_methods();
    });
});

}(cockpit, jQuery));
