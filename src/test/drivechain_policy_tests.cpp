// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <drivechain/messages.h>
#include <policy/policy.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace drivechain;

namespace {
//! Evaluate `script_pub_key` with an empty scriptSig, as a treasury spend does.
ScriptError Verify(const CScript& script_pub_key, script_verify_flags flags)
{
    ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
    const BaseSignatureChecker checker;
    VerifyScript(CScript{}, script_pub_key, nullptr, flags, checker, &error);
    return error;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_policy_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(treasury_spends_relay_only_under_the_flag)
{
    const CScript treasury{TreasuryScript(1)};

    // Consensus has no opinion: OP_NOP5 is a no-op, so the trailing OP_TRUE
    // leaves the script satisfied by an empty scriptSig. That is what makes
    // BIP-300 a soft fork, and why authorising a treasury spend belongs in
    // ConnectBlock rather than here.
    BOOST_CHECK_EQUAL(Verify(treasury, SCRIPT_VERIFY_NONE), SCRIPT_ERR_OK);

    // Under the relay discouragement alone the spend is non-standard, which is
    // why the reference implementation tells its users to set acceptnonstdtxn.
    BOOST_CHECK_EQUAL(Verify(treasury, SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS),
                      SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);

    // With the drivechain flag it relays.
    BOOST_CHECK_EQUAL(Verify(treasury, SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS | SCRIPT_VERIFY_DRIVECHAIN),
                      SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(only_the_exact_treasury_form_is_exempt)
{
    const script_verify_flags flags{SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS | SCRIPT_VERIFY_DRIVECHAIN};

    // A bare OP_NOP5 is still an unknown upgrade.
    BOOST_CHECK_EQUAL(Verify(CScript() << OP_NOP5 << OP_TRUE, flags), SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);

    // So is a script that merely begins as a treasury output. BIP-300 gives
    // OP_NOP5 a meaning only when the entire script is the four-byte form.
    const std::vector<unsigned char> trailing{OP_NOP5, 0x01, 0x01, OP_TRUE, OP_TRUE};
    BOOST_CHECK_EQUAL(Verify(CScript(trailing.begin(), trailing.end()), flags),
                      SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);

    // And so is every other NOP: the exemption is for OP_NOP5 in one shape,
    // not for upgradable NOPs in general.
    BOOST_CHECK_EQUAL(Verify(CScript() << OP_NOP4 << OP_TRUE, flags), SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
    BOOST_CHECK_EQUAL(Verify(CScript() << OP_NOP8 << OP_TRUE, flags), SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
}

BOOST_AUTO_TEST_CASE(every_slot_relays)
{
    const script_verify_flags flags{SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS | SCRIPT_VERIFY_DRIVECHAIN};
    for (int slot{0}; slot <= 0xFF; ++slot) {
        BOOST_CHECK_EQUAL(Verify(TreasuryScript(static_cast<SlotNum>(slot)), flags), SCRIPT_ERR_OK);
    }
}

BOOST_AUTO_TEST_CASE(the_flag_is_policy_only)
{
    // The flag only ever removes a discouragement, so it cannot make a script
    // fail that would otherwise pass -- but the guarantee that matters is that
    // consensus never sees it, since a policy flag that leaked into block
    // validation would be a chain split waiting to happen.
    BOOST_CHECK(!(MANDATORY_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_DRIVECHAIN));
    BOOST_CHECK(STANDARD_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_DRIVECHAIN);
}

BOOST_AUTO_TEST_SUITE_END()
