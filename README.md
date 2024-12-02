# B1B - Bonding mode 1 bridge daemon

###

See [this Red Hat Bugzilla comment](https://bugzilla.redhat.com/show_bug.cgi?id=1381110#c0 )
for a description of the issue that this daemon addresses.

### Building

B1B requires the `json-c` and `libmnl` libraries, which should already be
packaged for just about any Linux distribution.  (The development packages are
required to build the executable.)

It also requires `libSAVL`, which can be found
[here](https://github.com/ipilcher/libsavl).

Once the dependencies are installed, the executable can be built by changing to
the `src` directory and running:

```
gcc -O2 -Wall -Wextra -Wcast-align=strict -o b1b *.c -lsavl -ljson-c -lmnl
```

### Running

`b1b` must be run with the `CAP_NET_RAW` capability (or as `root`).  It accepts
a number of command-line options.

* `-d` or `--debug` &mdash; Enable logging of debug-level messages.

* `-l` or `--syslog` &mdash; Prepend log messages with "syslog-style" priority.

* `-e` or `--stderr` &mdash; Do not prepend log messages with "syslog-style"
  priority.

> **NOTE**
>
> `b1b` always logs messages to `stderr`, and by default it will prepend
> priority levels to those messages when `stderr` is not a tty.  The `-l` and
> `-e` options can be used to change this default behavior (e.g. to suppress the
> priority labels if `stderr` is redirected to `tee`).

One or more mode 1 bond interfaces may be listed on the command line, after any
options.  If any interface names are present, only these interfaces will be
monitored.  All listed interfaces must be mode 1 bonds that are attached to
either a Linux or Open vSwitch bridge.  (`b1b` will exit with an error status if
any listed interface does not meet these criteria.)

If no interfaces are listed, `b1b` will automatically detect and
monitor all mode 1 bond interfaces that are attached to a bridge.  (If no such
interfaces exist on the system, `b1b` will exit with an error status.)

### Limitations

`b1b` does have some limitations.

* It does not detect changes in network configuration.  If a new bond or bridge
  is added, `b1b` must be restarted to detect the change.

* IP multicast is not supported.  `b1b` maintains connectivity to virtual
  machine (or other virtual interface attached to a bridge) by sending
  gratuitous ARP responses when it detects a link failover event.  This updates
  the MAC forwarding tables in the switches to which the host is attached, but
  it does not update multicast forwarding tables.
