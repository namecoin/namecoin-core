Wallet
------

- `name_new` to a multisig address of the wallet now derives the `rand` value
  from one of the wallet's own private keys for that address, and
  `name_firstupdate` without `rand` tries all of them. Before, the value for
  such an address was derived from an empty key: it depended only on the name
  and was not secret. The wallet that sent the `name_new` can finish the
  registration without `rand`, also with the "Register Name…" button of the
  GUI. The wallet of a co-signer holds other keys and needs the `rand` value
  that `name_new` returned.
