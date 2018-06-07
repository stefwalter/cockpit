console.log("CUPS");

/*
 * TODO:
 * 
 * - Detect cups service not started and offer to start
 * - Deal with inline scripts appropriately. All inline scripts
 *   are display and convenience scripts.
 * - Styling fixes
 * - Do we need jQuery for the serialize() funciton?
 * - Can we completely avoid inline scripts?
 * - Tests
 * - Documentation
 * - Packaging (in cups package itself?)
 */

var cockpit = require("cockpit");
var jq = require("jquery"); /* for jQuery.serialize() */

var headers = {
    "Host": "localhost"
};

var http = cockpit.http("/var/run/cups/cups.sock");
var parser = new DOMParser();

function populate(data) {
    let doc = parser.parseFromString(data, "text/html");
    let bodies = doc.getElementsByTagName("body");
    let children = bodies.length ? Array.from(bodies[0].children) : [];
    let app = document.getElementById("app");
    while (app.firstChild)
        app.removeChild(app.firstChild);
    for (var element of children) {
        element.parentNode.removeChild(element);
        app.appendChild(element);
    }
}

function cleanup(data) {
    data = data.replace(/<!--\r\n/g, '\r\n');
    data = data.replace(/\r\n-->/g, "\r\n");
    return data;
}

function request(method, path, params) {
    var options = {
        "method": method || "GET",
        "headers": cockpit.extend(headers),
        "path": path,
    };

    if (options.method.toUpperCase() == "GET") {
        options.params = params;
        options.body = "";
    } else {
        options.body = params || "";
        options.headers["Content-Type"] = "application/x-www-form-urlencoded";
    }

    if (http.cookie)
        options.headers["Cookie"] = http.cookie;

    console.log(JSON.stringify(options));
    return http.request(options)
            .response((status, headers) => {
                http.cookie = headers["Set-Cookie"];
                console.log(status, headers);
            })
            .then(data => {
                populate(cleanup(data));
            })
            .catch((ex, data) => {
                console.log(ex);
                populate(data);
            });
}

function navigate() {
    var path = cockpit.location.path.length == 0 ? "/admin" : cockpit.location.href
    request("GET", path);
}

function clicked(ev) {
    let target = ev.target;
    if (!target)
        return;

    if (target.tagName == "A") {
        let href = target.getAttribute("HREF");
        if (href) {
            cockpit.location.go(href);
            ev.preventDefault();
            ev.stopPropagation();
            return false;
        }
    }
}

function submitted(ev, target) {
    if (!target)
        target = ev.target;
    if (!target)
        return;

    let action = target.getAttribute("action");
    let params = jq(target).serialize();
    let method = (target.getAttribute("method") || "GET").toUpperCase();
    if (method == "GET") {
        let path = action;
        if (params)
            path += "?" + params;
        cockpit.location.go(path);
    } else {
        request(method, action, params);
    }

    ev.preventDefault();
    ev.stopPropagation();
    return false;
}

function ancestor(node, name) {
    let par = node.parentNode;
    if (!par)
        return null;
    if (par.nodeName == name)
        return par;
    return ancestor(par, name);
}

function changed(ev) {
    let target = ev.target;
    if (!target)
        return;

    if (target.tagName == "SELECT") {
        let onchange = target.getAttribute("ONCHANGE") || "";
        if (onchange.indexOf("submit") !== -1) {
            let form = ancestor(target, "FORM");
            if (form)
                submitted(ev, form);
        }
    }
}

var loaded = cockpit.defer();
cockpit.all(cockpit.user(), loaded).then((user) => {
    window.addEventListener("click", clicked, true);
    window.addEventListener("submit", submitted, true);
    window.addEventListener("change", changed, true);
    cockpit.addEventListener("locationchanged", navigate);
    headers["Authorization"] = "PeerCred " + user.name;
    navigate();
});

window.addEventListener("load", () => loaded.resolve());
