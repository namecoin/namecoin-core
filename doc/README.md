Namecoin Core
==============

Setup
---------------------
[Namecoin Core](http://namecoin.org/) is the official Namecoin client and it builds the backbone of the network. However, it downloads and stores the entire history of Namecoin transactions (which is currently several GBs); depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download Namecoin Core, visit [namecoin.org](https://namecoin.org/download/).

Running
---------------------
The following are some helpful notes on how to run Namecoin Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/namecoin-qt` (GUI) or
- `bin/namecoind` (headless)
- `bin/namecoin` (wrapper command)

The `namecoin` command supports subcommands like `namecoin gui`, `namecoin node`, and `namecoin rpc` exposing different functionality. Subcommands can be listed with `namecoin help`.

### Windows

Unpack the files into a directory, and then run namecoin-qt.exe.

### macOS

Drag Namecoin-Qt to your applications folder, and then run Namecoin-Qt.

### Need Help?

* See the documentation at the [Namecoin Site](https://namecoin.org)
for help and more information.
* Ask for help on [#namecoin](http://webchat.freenode.net?channels=namecoin) on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net?channels=namecoin).
* Ask for help on the [Namecoin forums](https://forum.namecoin.info/index.php), in the [Technical Support board](https://forum.namecoin.info/viewforum.php?f=7).

Building
---------------------
The following are developer notes on how to build Bitcoin Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows-native.md)

Development
---------------------
The Namecoin repo's [root README](https://github.com/namecoin/namecore/blob/master/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://doxygen.bitcoincore.org/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal Design Docs](design/)

### Resources
* Discuss on the [Namecoin forums](https://forum.namecoin.info/), in the [Development & Technical Discussion board](https://forum.namecoin.info/viewforum.php?f=8).
* Discuss on [#namecoin-dev](http://webchat.freenode.net/?channels=namecoin-dev) on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net/?channels=namecoin-dev).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [bitcoin.conf Configuration File](bitcoin-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Wallets](managing-wallets.md)
- [Multisig Tutorial](multisig-tutorial.md)
- [Offline Signing Tutorial](offline-signing-tutorial.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
