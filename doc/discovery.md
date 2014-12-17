
Cockpit Discovery
=================

The Cockpit dashboard wants to know about:

 * Machines: Various hosts to display, including virtual/container machines
 * Objects: Services, applications or service containers running on machines
 * Alerts: That happen to either machines or objects

'Machines' are things that (can/should/do) run a cockpit-bridge and are
controlled independently. 'Objects' run on those hosts. 'Machines' can run
on other machines (think virtual machines). Containers are worth calling
out explicitly, some containers are service containers, and show up as
'objects'. Other containers are full blown machines (eg: running systemd
inside them) and show up as 'machines'. See below for more examples.
'Alerts' are things worth drawing the admins attention to. Generally
high priority items, always related to a 'machine' or an 'object'.

Each machine has the following properties. All are optional, although without
sufficient information the dasboard will not display the machine.

 * address: Address or host name to connect to the host
 * id: http://www.freedesktop.org/software/systemd/man/machine-id.html
 * label: A displayable label of the machine
 * state: One of 'running', 'waiting', 'failed', 'stopped'
 * problems: An array of Cockpit problem codes
 * present: Set to true if the machine has been explicitly added
 * masked: If set to true, hide the machine

 * machines: An array/object with nested machines
 * objects: An array/object with objects running on this machine (below)
 * alerts: An array/object with alerts as values (see below)

Each object has the following properties:

 * location: A relative Cockpit location including a first type: type/path/...
 * label: A displayable label
 * state: One of 'running', 'waiting', 'failed', 'stopped'
 * masked: If set to true, hide the object
 * alerts: An array/object with alerts as values (see below)

Each alert has the following properties:
 * timestamp: Optionally the timestamp (ms since epoch) when it occured
 * id: Optionally a unique identifier
 * message: Required, a message to display
 * priority: Textual priority indicator eg: "warn", "crit", "emerg", "alert"


Discovery Modules
-----------------

In the package manifest there is an optional 'disco' section listing the
modules that implement discovery.

   "disco": [ "module" ]

Each module implements a 'disco(address, callback)' function, which starts
discovery. It invokes callback(data) with discovered data, multiple times with
an array or dict that have machines as values.

The disco() function returns an object that has a close() method that
can be used to stop the discovery.

The address is a normal Cockpit host option address to run the discovery
against.  data is in the following form. Note that each discovery module
may only provide partial. The shell correlates things up based on machine
addresses and ids, and combines data as needed.

    {
        "module-specific-machine-handle": {
            "address": "localhost",
            "id": "45bb3b96146aa94f299b9eb43646eb35",
            "label": "Big Host System",
            "state": "running",
            "problems": [ ],

            "machines": {
          	"module-specific-machine-handle": {
                    "address": "10.11.11.11",
                    "label": "Atomic VM 1",
                    "state": "running",
                    "problems": [ "unknown-hostkey", "no-cockpit" ],
                    "data": ....

                    "objects: {
                        "some-object-handle": {
                            "location": "docker/a9132d3263b0",
                            "state": "running",
                            "label": "Container: fedora/apache"
                            "events": {
                            "data": ....
                        },
                        "another-object-handle": {
                            "location": "rolekit/domaincontroller",
                            "state": "running",
                            "label": "Container: fedora/apache"
                            "data": ....
                        }
                    }
                }
            }
        }
    }

Each machine or object can have an additional data field, containing additonal
javascript "data". Each machine or object should be representable as JSON. It
shouldn't contain functions.

Examples implementations
------------------------

 * Dashboard Machines: The manually added list of machines is a discovery
   module which returns the list of machines. Only returns results when invoked
   with a null (ie: local) parent machine. Always returns a self machine.

 * Systemd units: Returns specially marked services as objects. This marking
   should be configurable through Cockpit.

 * Systemd machines: Returns machined stuff as machines.

 * Docker: Containers returned as objects. Only containers that are running and/or
   set to start when Docker starts next are included. Docker containers are masked
   by other things like Kubernetes pods or Rolekit roles.

 * Kubernetes: Discovers minions as machines. Returns pods as objects. Only returns
   results when invoked on the Kubernetes master with a localhost address.
   Pods mask docker containers that they include.

 * Libvirt: Returns virtual machines as machines.

 * Rolekit: Returns any enabled roles as things. Roles mask Systemd services and
   containers that they include.

Dashboard Considerations
------------------------

Color, avatar, pretty name should be stored independently of the host list.
In addition there should be able to store a dashboard flag which hides a
host by uuid and address.
