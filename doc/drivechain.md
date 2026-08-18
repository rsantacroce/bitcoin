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
| `src/drivechain/state.{h,cpp}` | D1 and D2, the two lists an enforcing node maintains |
| `src/drivechain/diff.{h,cpp}` | what one block does to that state, and how to undo it |
| `src/drivechain/db.{h,cpp}` | persistence for both |
| `src/drivechain/params.{h,cpp}` | thresholds and activation, per network |
| `src/drivechain/validation.{h,cpp}` | the rules a block must satisfy |
| `src/rpc/drivechain.cpp` | read-only calls reporting the state |

The chainstate owns a `DrivechainDB` beside its coins database, and
`ConnectBlock` and `DisconnectBlock` take the sidechain state the way they take
the coins view.

What is still missing: the deposit sequence index and the two RPCs that read it.

Everything under `src/drivechain/` is new code; the diff against upstream files
is deliberately kept to a handful of call sites so that the patchset stays
reviewable and rebases cleanly across Core releases.

## Dependencies

**None.** `bitcoind` and `test_bitcoin` build and pass with no part of the
drivechain ecosystem present, and no Rust toolchain installed.

That is worth stating explicitly because the reference implementation is cited
throughout this patchset, and a reader could reasonably wonder whether any of it
is load-bearing at build or run time. It is not:

- Everything in `src/drivechain/` includes Bitcoin Core headers and the C++
  standard library, and nothing else.
- The reference implementation is named only in comments, explaining why a rule
  is what it is.
- The build system does not reference `contrib/`, and never invokes `cargo`.
- The differential test reads `src/test/data/drivechain_vectors.json`, a
  committed data file, through the same mechanism Core already uses for
  `script_tests.json` and `sighash.json`. It is data, not a dependency.

The one place the reference implementation is used is
`contrib/drivechain-vectors`, a standalone tool that regenerates that data file.
Nothing builds it and nothing links it. Delete the directory and the node and its
entire test suite still build and pass; the only thing lost is the ability to
refresh the vectors.

The direction of the arrow matters here. This implementation is checked *against*
the reference implementation. It does not depend *on* it, and there is no code
path, include, or build rule by which it could start to.

## Reviewing

Commits are small and ordered so each one builds and tests green on its own.

**Phase 1** is pure functions over bytes — no chain state, no database, nothing
that can reject a block:

1. treasury and deposit output scripts
2. BIP-300 coinbase messages (M1–M4)
3. BIP-301 messages (M7, M8)
4. `M6ID` and the blinded withdrawal

The parsers are the foundation everything later stands on, and a mistake here
misclassifies messages silently rather than failing loudly — which is why they
land first, and alone.

**Phase 2** is the state and how a block moves it. Still nothing that can reject
a block:

5. the sidechain and withdrawal state (D1, D2, treasury pointers)
6. the per-block diff, with apply and undo
7. serialization for the diff
8. persistence for the state and the diffs
9. a fuzz target over connect/disconnect sequences

The invariant the phase exists to establish is that undoing a block restores the
state exactly. Everything in Phase 4 depends on it, and a reorg that leaves the
state subtly wrong would not be visible until a block is rejected hundreds of
blocks later. That is why the fuzz target lands here rather than at the end.

**Phase 3** is the parameters and the relay policy:

10. the network thresholds, and the comparisons made against them
11. activation and thresholds per network in chainparams
12. relaying spends of treasury outputs
13. relaying creation of treasury outputs

Nothing activates by default. Choosing an activation is a deployment decision
nobody has made, and inventing a height here would be answering a question that
has not been asked. Phase 5 makes regtest opt in with
`-testactivationheight=drivechain@N`.

**Phase 4** is the rule set: all 24 conditions under which a block is invalid,
and the state transitions that go with them.

14. collect and check coinbase messages
15. M1 and M2 — the sidechain list
16. ageing out proposals and bundles
17. M3 — bundle proposals
18. M4 — bundle votes
19. M5 and M6 — deposits and withdrawals
20. M7 and M8 — blind merged mining
21. assembling them into a block check
22. a fuzz target over whole blocks

Nothing calls it yet: wiring it into `ConnectBlock` is the next phase, and it is
kept separate so the rule set can be read and argued about on its own.

The order the steps run in is load-bearing rather than incidental — coinbase
messages are applied to a running state so a later one sees the earlier ones,
ageing follows the messages, transactions follow both — and each consequence has
a test rather than a comment.

**Phase 5** puts it in the chainstate, where a block that breaks a rule is
rejected before it can be connected.

23. the state carries the block it describes
24. regtest chooses its activation height
25. enforcement in `ConnectBlock` and `DisconnectBlock`
26. a functional test

Nothing activates by default on any network, regtest included. A test turns the
rules on with `-testactivationheight=drivechain@N`, the way it turns on segwit.

**The state is a parameter, not a member of the chainstate**, and that is the
part most likely to be undone by accident. Core connects and disconnects blocks
in places that are not the tip — `VerifyDB` unwinds and replays at startup, and
the coins database may lag the chain and be caught up by `ReplayBlocks` — so
those callers hand over a scratch copy, exactly as they already do for coins. A
state read off the chainstate in those paths would be asked to undo a block it
never applied. It carries the block it describes so each caller can tell whether
it is in a position to act at all; when it is not, the sidechain work is skipped
rather than guessed at.

A chainstate loaded from a UTXO snapshot describes no block, so these rules are
not enforced on it: it has no history to derive the state from and the snapshot
carries none. Its blocks are validated by the background chainstate, which does.

**Phase 7** is proving it, from two directions at once.

29. differential vectors generated from the reference implementation
30. a functional test that deposits into a treasury and withdraws from it

The vectors say what the deployed enforcer decides; the functional test says
that coins actually move. Between them they cover the two ways this could be
wrong: disagreeing with the implementation it has to match, and agreeing with it
while not working.

The functional test computes the `M6ID` independently, from the specification
rather than from the C++, and asserts the node arrives at the same identifier
from the same transaction. That is a third implementation of the rule agreeing
with the other two.

**Phase 6** is mempool policy and the RPC surface.

27. keeping transactions that cannot be mined out of the mempool
28. read-only calls reporting the state

Neither is consensus. A node that admits too much to its mempool wastes a block
template it will itself reject; a node that admits too little only declines to
relay. Only the mempool's half of the BMM request rules can be decided there at
all, since the accept that would match a request does not exist yet — which is
also what the reference implementation does.

## Notes for implementers

**M7 and M8 are not parsed the same way, and the asymmetry is load-bearing.**
M7 — like every BIP-300 coinbase message — is parsed as script *instructions*, so
any valid encoding of the payload push is accepted. M8 is matched against a fixed
*byte prefix* (`6A 44 00 BF 00`), so its push opcode is pinned and an
`OP_PUSHDATA1` encoding of the same 68 payload bytes does not parse at all. A
single generic parser for both is wrong, and wrong in a way that tests over
well-formed messages will not catch.

**The diff carries more than the change.** Vote counts saturate at zero and
bundle positions are what an M4 votes by, so undo cannot recompute either from
the state it is handed — it has to be told which bundles actually lost a vote and
where in the list each one sat. Anything that looks like redundancy in
`diff.h` is almost certainly this.

**`OP_DRIVECHAIN` is `OP_NOP5`, and the treasury is anyone-can-spend — to
enforcing and non-enforcing nodes alike.** The treasury script
`OP_NOP5 OP_PUSHBYTES_1 <slot> OP_TRUE` evaluates true with an empty scriptSig,
and BIP-300 does not change that. It cannot: if the interpreter rejected a
treasury spend, it would reject the legitimate deposits and withdrawals too,
since `ConnectBlock` verifies input scripts before it has any chance to
authorise anything.

So there is **no consensus change to the script interpreter in this patchset**,
and there will not be one. Every rule that protects the treasury is a
block-level rule in `ConnectBlock`, which is why those rejections are the peg
itself rather than a convenience — and why relaxing any of them is a different
proposition from relaxing the rest.

The only script-layer change here is relay policy: a treasury output and its
spend are made standard, so deposits and withdrawals reach a miner. The
reference implementation cannot do that from outside the node, and tells its
users to run with `acceptnonstdtxn=1` instead.
