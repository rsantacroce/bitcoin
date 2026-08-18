# BIP-300/301 (Drivechain) in Bitcoin Core

This patchset implements the BIP-300 (Hashrate Escrows) and BIP-301 (Blind Merged
Mining) consensus rules natively in `bitcoind`, so that running a drivechain node
takes one process instead of three.

Today those rules are enforced out-of-process by
[`bip300301_enforcer`](https://github.com/LayerTwo-Labs/bip300301_enforcer),
which follows the chain over ZMQ and calls `invalidateblock` when a block breaks
a rule. Enforcing in `ConnectBlock` instead means a non-compliant block is never
connected in the first place, which removes the entire class of races the
out-of-process design has to handle.

## Specifications

| | Document |
|---|---|
| BIP-300 | <https://github.com/LayerTwo-Labs/bip300_bip301_specifications/blob/master/bip300.md> |
| BIP-301 | <https://github.com/LayerTwo-Labs/bip300_bip301_specifications/blob/master/bip301.md> |
| Reference implementation | <https://github.com/LayerTwo-Labs/bip300301_enforcer> |

Source files in `src/drivechain/` cite the specification section they implement,
and — where the two differ — the enforcer function whose behaviour is normative.

## What this patchset follows

**Where the specifications and the reference implementation disagree, this
patchset follows the implementation.** That is a deliberate choice, not an
oversight: it keeps a differential test harness meaningful. With behaviour
matched rule for rule, any divergence between the enforcer and this code over
the same blocks is a bug rather than an intended difference.

The known divergences are recorded at each site with a `Spec divergence:` comment
so they are visible in review rather than buried in a design document. They are
corrections owed back to the specifications, and are expected to land there — and
then here — as a follow-up.

Two are worth knowing before reading any of the code:

- **The M2 message tag is `D6 E1 C5 DF`**, which is what the reference
  implementation uses. Both specifications say `D6 E1 C5 BF`. Which is correct is
  an open question with the specification authors; one of the three sources has a
  wire-format bug. This patchset follows the deployed implementation, since that
  is what interoperating software must match today.
- **Message payloads live inside a single data push.** The specifications write
  message headers as raw script bytes (`M2: 0x6A 0xD6 0xE1 0xC5 0xDF`) and give
  byte counts on that basis, but every message is in fact encoded as
  `OP_RETURN <push>` with the tag at the start of the pushed data. An M2 script
  is therefore 39 bytes, not the 38 the specification states.

## Structure

BIP-300 is a connect-time rule set. Almost every rule needs either the UTXO set
(M5/M6 compare a treasury output's value against the value it spent) or accrued
BIP-300 state (M1–M4 vote counts), so `CheckBlock` cannot carry them. The layout
follows that split:

| Path | Contains |
|---|---|
| `src/drivechain/messages.{h,cpp}` | wire formats: treasury and deposit scripts, M1–M4, M7, M8 |
| `src/drivechain/m6id.{h,cpp}` | the blinded withdrawal transaction and its `M6ID` |

Later phases add the state layer, the script flag, connect-time validation,
mempool policy and RPCs. This document grows with them.

Everything under `src/drivechain/` is new code; the diff against upstream files
is deliberately kept to a handful of call sites so that the patchset stays
reviewable and rebases cleanly across Core releases.

## Reviewing

Commits are small and ordered so each one builds and tests green on its own.
Phase 1 (this series) is pure functions over bytes — no chain state, no database,
nothing that can reject a block:

1. treasury and deposit output scripts
2. BIP-300 coinbase messages (M1–M4)
3. BIP-301 messages (M7, M8)
4. `M6ID` and the blinded withdrawal

The parsers are the foundation everything later stands on, and a mistake here
misclassifies messages silently rather than failing loudly — which is why they
land first, and alone.

## Notes for implementers

**M7 and M8 are not parsed the same way, and the asymmetry is load-bearing.**
M7 — like every BIP-300 coinbase message — is parsed as script *instructions*, so
any valid encoding of the payload push is accepted. M8 is matched against a fixed
*byte prefix* (`6A 44 00 BF 00`), so its push opcode is pinned and an
`OP_PUSHDATA1` encoding of the same 68 payload bytes does not parse at all. A
single generic parser for both is wrong, and wrong in a way that tests over
well-formed messages will not catch.

**`OP_DRIVECHAIN` is `OP_NOP5`, and the treasury is anyone-can-spend to a node
that does not enforce these rules.** The treasury script
`OP_NOP5 OP_PUSHBYTES_1 <slot> OP_TRUE` evaluates true with an empty scriptSig
under current consensus. That is what makes BIP-300 a soft fork, and it is also
why the rules that reject treasury spends are the peg itself rather than a
convenience.
