// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Copyright (c) 2017-2022 The Particl Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/kernel.h>

#include <chainparams.h>
#include <serialize.h>
#include <streams.h>
#include <hash.h>
#include <util/system.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <policy/policy.h>
#include <consensus/validation.h>
#include <coins.h>
#include <insight/insight.h>
#include <txmempool.h>
#include <node/transaction.h>
#include <validation.h>

/* Calculate the difficulty for a given block index.
 * Duplicated from rpc/blockchain.cpp for linking
 */
static double GetDifficulty(const CBlockIndex* blockindex)
{
    CHECK_NONFATAL(blockindex);

    int nShift = (blockindex->nBits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

double GetPoSKernelPS(CBlockIndex *pindex)
{
    LOCK(cs_main);

    CBlockIndex *pindexPrevStake = nullptr;

    int nBestHeight = pindex->nHeight;

    int nPoSInterval = 72; // blocks sampled
    double dStakeKernelsTriedAvg = 0;
    int nStakesHandled = 0, nStakesTime = 0;

    while (pindex && nStakesHandled < nPoSInterval) {
        if (pindex->IsProofOfStake()) {
            if (pindexPrevStake) {
                dStakeKernelsTriedAvg += GetDifficulty(pindexPrevStake) * 4294967296.0;
                nStakesTime += pindexPrevStake->nTime - pindex->nTime;
                nStakesHandled++;
            }
            pindexPrevStake = pindex;
        }
        pindex = pindex->pprev;
    }

    double result = 0;

    if (nStakesTime) {
        result = dStakeKernelsTriedAvg / nStakesTime;
    }

    result *= Params().GetStakeTimestampMask(nBestHeight) + 1;

    return result;
}

/**
 * Stake Modifier (hash modifier of proof-of-stake):
 * The purpose of stake modifier is to prevent a txout (coin) owner from
 * computing future proof-of-stake generated by this txout at the time
 * of transaction confirmation. To meet kernel protocol, the txout
 * must hash with a future stake modifier to generate the proof.
 */
uint256 ComputeStakeModifierV2(const CBlockIndex *pindexPrev, const uint256 &kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->bnStakeModifier;
    return Hash(ss);
}

/**
 * BlackCoin kernel protocol
 * coinstake must meet hash target according to the protocol:
 * kernel (input 0) must meet the formula
 *     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
 * this ensures that the chance of getting a coinstake is proportional to the
 * amount of coins one owns.
 * The reason this hash is chosen is the following:
 *   nStakeModifier: scrambles computation to make it very difficult to precompute
 *                   future proof-of-stake
 *   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
 *                       generate transaction for future advantage,
 *                       obsolete since v3
 *   txPrev.nTime: slightly scrambles computation
 *   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
 *                     generating coinstake at the same time
 *   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
 *                  generating coinstake at the same time
 *   nTime: current timestamp
 *   block/tx hash should not be used here as they can be generated in vast
 *   quantities so as to generate blocks faster, degrading the system back into
 *   a proof-of-work situation.
 */
bool CheckStakeKernelHash(const CBlockIndex *pindexPrev,
    uint32_t nBits, uint32_t nBlockFromTime,
    CAmount prevOutAmount, const COutPoint &prevout, uint32_t nTime,
    uint256 &hashProofOfStake, uint256 &targetProofOfStake,
    bool fPrintProofOfStake)
{
    // CheckStakeKernelHashV2

    if (nTime < nBlockFromTime) {  // Transaction timestamp violation
        return error("%s: nTime violation", __func__);
    }

    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0) {
        return error("%s: SetCompact failed.", __func__);
    }

    // Weighted target
    int64_t nValueIn = prevOutAmount;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    targetProofOfStake = ArithToUint256(bnTarget);

    const uint256 &bnStakeModifier = pindexPrev->bnStakeModifier;
    int nStakeModifierHeight = pindexPrev->nHeight;
    int64_t nStakeModifierTime = pindexPrev->nTime;

    CDataStream ss(SER_GETHASH, 0);
    ss << bnStakeModifier;
    ss << nBlockFromTime << prevout.hash << prevout.n << nTime;
    hashProofOfStake = Hash(ss);

    if (fPrintProofOfStake) {
        LogPrintf("%s: using modifier=%s at height=%d timestamp=%s\n",
            __func__, bnStakeModifier.ToString(), nStakeModifierHeight,
            FormatISO8601DateTime(nStakeModifierTime));
        LogPrintf("%s: check modifier=%s nTimeKernel=%u nPrevout=%u nTime=%u hashProof=%s\n",
            __func__, bnStakeModifier.ToString(),
            nBlockFromTime, prevout.n, nTime,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget) {
        return false;
    }

    if (LogAcceptCategory(BCLog::POS, BCLog::Level::Debug) && !fPrintProofOfStake) {
        LogPrintf("%s: using modifier=%s at height=%d timestamp=%s\n",
            __func__, bnStakeModifier.ToString(), nStakeModifierHeight,
            FormatISO8601DateTime(nStakeModifierTime));
        LogPrintf("%s: pass modifier=%s nTimeKernel=%u nPrevout=%u nTime=%u hashProof=%s\n",
            __func__, bnStakeModifier.ToString(),
            nBlockFromTime, prevout.n, nTime,
            hashProofOfStake.ToString());
    }

    return true;
}

bool GetKernelInfo(const CBlockIndex *blockindex, const CTransaction &tx, uint256 &hash, CAmount &value, CScript &script, uint256 &blockhash)
{
    if (!blockindex->pprev) {
        return false;
    }
    if (tx.vin.size() < 1) {
        return false;
    }
    const COutPoint &prevout = tx.vin[0].prevout;
    CTransactionRef txPrev;
    CBlock blockKernel; // block containing stake kernel, GetTransaction should only fill the header.
    if (!node::GetTransaction(prevout.hash, txPrev, Params().GetConsensus(), blockKernel)
        || prevout.n >= txPrev->vpout.size()) {
        return false;
    }
    const CTxOutBase *outPrev = txPrev->vpout[prevout.n].get();
    if (!outPrev->IsStandardOutput()) {
        return false;
    }
    value = outPrev->GetValue();
    script = *outPrev->GetPScriptPubKey();
    blockhash = blockKernel.GetHash();

    uint32_t nBlockFromTime = blockKernel.nTime;
    uint32_t nTime = blockindex->nTime;

    CDataStream ss(SER_GETHASH, 0);
    ss << blockindex->pprev->bnStakeModifier;
    ss << nBlockFromTime << prevout.hash << prevout.n << nTime;
    hash = Hash(ss);

    return true;
};

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CChainState &chain_state, BlockValidationState &state, const CBlockIndex *pindexPrev, const CTransaction &tx, int64_t nTime, unsigned int nBits, uint256 &hashProofOfStake, uint256 &targetProofOfStake)
{
    // pindexPrev is the current tip, the block the new block will connect on to
    // nTime is the time of the new/next block

    auto &pblocktree{chain_state.m_blockman.m_block_tree_db};

    if (!tx.IsCoinStake()
        || tx.vin.size() < 1) {
        LogPrintf("ERROR: %s: malformed-txn %s\n", __func__, tx.GetHash().ToString());
        return state.Invalid(BlockValidationResult::DOS_100, "malformed-txn");
    }

    CTransactionRef txPrev;

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn &txin = tx.vin[0];

    uint32_t nBlockFromTime;
    int nDepth;
    CScript kernelPubKey;
    CAmount amount;

    Coin coin;
    if (!chain_state.CoinsTip().GetCoin(txin.prevout, coin) || coin.IsSpent()) {
        // Read from spent cache
        SpentCoin spent_coin;
        if (!pblocktree->ReadSpentCache(txin.prevout, spent_coin)) {
            LogPrintf("ERROR: %s: prevout-not-found\n", __func__);
            return state.Invalid(BlockValidationResult::DOS_20, "prevout-not-found");
        }
        if (!fVerifyingDB &&
            (unsigned int)pindexPrev->nHeight > spent_coin.spent_height &&
            pindexPrev->nHeight - spent_coin.spent_height > MAX_REORG_DEPTH) {
            LogPrintf("ERROR: %s: Tried to stake kernel spent at height %d\n", __func__, spent_coin.spent_height);
            return state.Invalid(BlockValidationResult::DOS_100, "invalid-prevout");
        }
        coin = spent_coin.coin;
        state.nFlags |= BLOCK_STAKE_KERNEL_SPENT;
    }
    if (coin.nType != OUTPUT_STANDARD) {
        LogPrintf("ERROR: %s: invalid-prevout\n", __func__);
        return state.Invalid(BlockValidationResult::DOS_100, "invalid-prevout");
    }

    CBlockIndex *pindex = chain_state.m_chain[coin.nHeight];
    if (!pindex) {
        LogPrintf("ERROR: %s: invalid-prevout\n", __func__);
        return state.Invalid(BlockValidationResult::DOS_100, "invalid-prevout");
    }

    nDepth = pindexPrev->nHeight - coin.nHeight;
    int nRequiredDepth = std::min((int)(Params().GetStakeMinConfirmations()-1), (int)(pindexPrev->nHeight / 2));
    if (nRequiredDepth > nDepth) {
        LogPrintf("ERROR: %s: Tried to stake at depth %d\n", __func__, nDepth + 1);
        return state.Invalid(BlockValidationResult::DOS_100, "invalid-stake-depth");
    }

    kernelPubKey = coin.out.scriptPubKey;
    amount = coin.out.nValue;
    nBlockFromTime = pindex->GetBlockTime();

    const CScript &scriptSig = txin.scriptSig;
    const CScriptWitness *witness = &txin.scriptWitness;
    ScriptError serror = SCRIPT_ERR_OK;
    std::vector<uint8_t> vchAmount(8);
    part::SetAmount(vchAmount, amount);
    // Redundant: all inputs are checked later during CheckInputs
    if (!VerifyScript(scriptSig, kernelPubKey, witness, STANDARD_SCRIPT_VERIFY_FLAGS, TransactionSignatureChecker(&tx, 0, vchAmount, MissingDataBehavior::FAIL), &serror)) {
        LogPrintf("ERROR: %s: verify-script-failed, txn %s, reason %s\n", __func__, tx.GetHash().ToString(), ScriptErrorString(serror));
        return state.Invalid(BlockValidationResult::DOS_100, "verify-cs-script-failed");
    }

    if (!CheckStakeKernelHash(pindexPrev, nBits, nBlockFromTime,
        amount, txin.prevout, nTime, hashProofOfStake, targetProofOfStake, LogAcceptCategory(BCLog::POS, BCLog::Level::Debug))) {
        LogPrintf("WARNING: %s: Check kernel failed on coinstake %s, hashProof=%s\n", __func__, tx.GetHash().ToString(), hashProofOfStake.ToString());
        return state.Invalid(BlockValidationResult::DOS_1, "check-kernel-failed");
    }

    // Ensure the input scripts all match and that the total output value to the input script is not less than the total input value.
    // The treasury fund split is user selectable, making it difficult to check the blockreward here.
    // Leaving a window for compromised staking nodes to reassign the blockreward to an attacker's address.
    // If coin owners detect this, they can move their coin to a new address.
    if (HasIsCoinstakeOp(kernelPubKey)) {
        // Sum value from any extra inputs
        for (size_t k = 1; k < tx.vin.size(); ++k) {
            const CTxIn &txin = tx.vin[k];
            Coin coin;
            if (!chain_state.CoinsTip().GetCoin(txin.prevout, coin) || coin.IsSpent()) {
                SpentCoin spent_coin;
                if (!pblocktree->ReadSpentCache(txin.prevout, spent_coin)) {
                    LogPrintf("ERROR: %s: prevout-not-found\n", __func__);
                    return state.Invalid(BlockValidationResult::DOS_20, "prevout-not-in-chain");
                }
                coin = spent_coin.coin;
                LogPrint(BCLog::POS, "%s: Input %d of coinstake %s is spent.\n", __func__, k, tx.GetHash().ToString());
            }
            if (coin.nType != OUTPUT_STANDARD) {
                LogPrintf("ERROR: %s: invalid-prevout %d\n", __func__, k);
                return state.Invalid(BlockValidationResult::DOS_100, "invalid-prevout");
            }
            if (kernelPubKey != coin.out.scriptPubKey) {
                LogPrintf("ERROR: %s: mixed-prevout-scripts %d\n", __func__, k);
                return state.Invalid(BlockValidationResult::DOS_100, "mixed-prevout-scripts");
            }
            amount += coin.out.nValue;
        }

        CAmount nVerify = 0;
        for (const auto &txout : tx.vpout) {
            if (!txout->IsType(OUTPUT_STANDARD)) {
                if (!txout->IsType(OUTPUT_DATA)) {
                    LogPrintf("ERROR: %s: bad-output-type\n", __func__);
                    return state.Invalid(BlockValidationResult::DOS_100, "bad-output-type");
                }
                continue;
            }
            const CScript *pOutPubKey = txout->GetPScriptPubKey();

            if (pOutPubKey && *pOutPubKey == kernelPubKey) {
                nVerify += txout->GetValue();
            }
        }

        if (nVerify < amount) {
            LogPrintf("ERROR: %s: verify-amount-script-failed, txn %s\n", __func__, tx.GetHash().ToString());
            return state.Invalid(BlockValidationResult::DOS_100, "verify-amount-script-failed");
        }
    }

    return true;
}


// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock)
{
    return (nTimeBlock & Params().GetStakeTimestampMask(nHeight)) == 0;
}

// Used only when staking, not during validation
bool CheckKernel(CChainState &chain_state, const CBlockIndex *pindexPrev, unsigned int nBits, int64_t nTime, const COutPoint &prevout, int64_t *pBlockTime)
{
    uint256 hashProofOfStake, targetProofOfStake;

    Coin coin;
    {
        LOCK(::cs_main);
        if (!chain_state.CoinsTip().GetCoin(prevout, coin)) {
            return error("%s: prevout not found", __func__);
        }
    }
    if (coin.nType != OUTPUT_STANDARD) {
        return error("%s: prevout not standard output", __func__);
    }
    if (coin.IsSpent()) {
        return error("%s: prevout is spent", __func__);
    }

    CBlockIndex *pindex = chain_state.m_chain[coin.nHeight];
    if (!pindex) {
        return false;
    }

    int nRequiredDepth = std::min((int)(Params().GetStakeMinConfirmations()-1), (int)(pindexPrev->nHeight / 2));
    int nDepth = pindexPrev->nHeight - coin.nHeight;

    if (nRequiredDepth > nDepth) {
        return false;
    }
    if (pBlockTime) {
        *pBlockTime = pindex->GetBlockTime();
    }

    CAmount amount = coin.out.nValue;
    return CheckStakeKernelHash(pindexPrev, nBits, *pBlockTime,
        amount, prevout, nTime, hashProofOfStake, targetProofOfStake);
}

