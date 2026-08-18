# drivechain-vectors

Generates `src/test/data/drivechain_vectors.json` by running the **reference
implementation** of BIP-300/301 over a set of inputs and recording what it
decides.

The point is not to test the reference implementation. It is to have something
Bitcoin Core's own tests can check themselves against, so that "this port
matches the deployed enforcer" is a claim a test makes rather than a claim a
commit message makes. Where the two disagree, one of them is wrong, and the
vectors say which inputs to look at.

Inputs are chosen for the places the two implementations could plausibly drift:
the message tags, the encodings that are byte-exact where their neighbours are
not, the boundaries of every length rule, and the arithmetic behind `M6ID`.

## Regenerating

```
cargo run --manifest-path contrib/drivechain-vectors/Cargo.toml > src/test/data/drivechain_vectors.json
```

A vector file is only meaningful alongside the revision that produced it, so
regenerate and commit the two together, and say in the commit message which
revision it was.

To build against a local clone rather than the git source, add a patch:

```toml
[patch."https://github.com/LayerTwo-Labs/bip300301_enforcer.git"]
bip300301_enforcer_lib = { path = "../../../bip300301_enforcer/lib" }
```

Note that the reference implementation patches three of its own dependencies,
and a `[patch]` section only applies from the manifest cargo is invoked on. Those
patches are repeated in `Cargo.toml` here; without them cargo resolves crates
whose API the reference implementation does not compile against, which fails in
a way that looks nothing like the real cause.

## Byte order

Hashes are hex in **internal** byte order — the order they appear on the wire —
rather than the reversed order Bitcoin displays them in. The two
implementations agree on the wire and disagree on the display convention, so
recording the wire order is what keeps the comparison about the thing that
matters.
