define([
    "jquery",
    "base1/cockpit"
], function($, cockpit) {
    var module = { };

    function PasswdFile() {

    }

    PasswdFile.parse = function parse(content) {
        var ret = [ ];
        var lines = content.split('\n');
        var column;

        for (var i = 0; i < lines.length; i++) {
            if (!lines[i])
                continue;
            column = lines[i].split(':');
            ret[i] = [];
            ret[i]["name"] = column[0];
            ret[i]["password"] = column[1];
            ret[i]["uid"] = column[2];
            ret[i]["gid"] = column[3];
            ret[i]["gecos"] = column[4];
            ret[i]["home"] = column[5];
            ret[i]["shell"] = column[6];
        }

        return ret;
    };

    module.PasswdFile = PasswdFile;

    function GroupFile() {

    }

    GroupFile.parse = function parse(content) {
        var ret = [ ];
        var lines = content.split('\n');
        var column;

        for (var i = 0; i < lines.length; i++) {
            if (! lines[i])
                continue;
            column = lines[i].split(':');
            ret[i] = [];
            ret[i]["name"] = column[0];
            ret[i]["password"] = column[1];
            ret[i]["gid"] = column[2];
            ret[i]["members"] = column[3].split(',');
        }

        return ret;
    };

    module.GroupFile = GroupFile;

    return module;
});
