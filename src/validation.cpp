// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <checkpoints.h>
#include <checkqueue.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <cuckoocache.h>
#include <hash.h>
#include <index/txindex.h>
#include <init.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <reverse_iterator.h>
#include <script/script.h>
#include <script/interpreter.h>
#include <script/sigcache.h>
#include <script/standard.h>
#include <timedata.h>
#include <tinyformat.h>
#include <txdb.h>
#include <txmempool.h>
#include <ui_interface.h>
#include <undo.h>
#include <util.h>
#include <utilmoneystr.h>
#include <utilstrencodings.h>
#include <validationinterface.h>
#include <warnings.h>
#include <smsg/smessage.h>
#include <pos/kernel.h>
#include <blind.h>
#include <anon.h>
#include <rctindex.h>

#include <secp256k1_rangeproof.h>

#include <future>
#include <sstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/thread.hpp>

#if defined(NDEBUG)
# error "Particl cannot be compiled without assertions."
#endif

#define MICRO 0.000001
#define MILLI 0.001

/**
 * Global state
 */
namespace {
    struct CBlockIndexWorkComparator
    {
        bool operator()(const CBlockIndex *pa, const CBlockIndex *pb) const {
            // First sort by most total work, ...
            if (pa->nChainWork > pb->nChainWork) return false;
            if (pa->nChainWork < pb->nChainWork) return true;

            // ... then by earliest time received, ...
            if (pa->nSequenceId < pb->nSequenceId) return false;
            if (pa->nSequenceId > pb->nSequenceId) return true;

            // Use pointer address as tie breaker (should only happen with blocks
            // loaded from disk, as those all have id 0).
            if (pa < pb) return false;
            if (pa > pb) return true;

            // Identical blocks.
            return false;
        }
    };
} // anon namespace
/*
enum DisconnectResult
{
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};
*/
class ConnectTrace;

/**
 * CChainState stores and provides an API to update our local knowledge of the
 * current best chain and header tree.
 *
 * It generally provides access to the current block tree, as well as functions
 * to provide new data, which it will appropriately validate and incorporate in
 * its state as necessary.
 *
 * Eventually, the API here is targeted at being exposed externally as a
 * consumable libconsensus library, so any functions added must only call
 * other class member functions, pure functions in other parts of the consensus
 * library, callbacks via the validation interface, or read/write-to-disk
 * functions (eventually this will also be via callbacks).
 */
class CChainState {
private:
    /**
     * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
     * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
     * missing the data for the block.
     */
    std::set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;

    /**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
    CCriticalSection cs_nBlockSequenceId;
    /** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
    int32_t nBlockSequenceId = 1;
    /** Decreasing counter (used by subsequent preciousblock calls). */
    int32_t nBlockReverseSequenceId = -1;
    /** chainwork for the last block that preciousblock has been applied to. */
    arith_uint256 nLastPreciousChainwork = 0;

    /** In order to efficiently track invalidity of headers, we keep the set of
      * blocks which we tried to connect and found to be invalid here (ie which
      * were set to BLOCK_FAILED_VALID since the last restart). We can then
      * walk this set and check if a new header is a descendant of something in
      * this set, preventing us from having to walk mapBlockIndex when we try
      * to connect a bad block and fail.
      *
      * While this is more complicated than marking everything which descends
      * from an invalid block as invalid at the time we discover it to be
      * invalid, doing so would require walking all of mapBlockIndex to find all
      * descendants. Since this case should be very rare, keeping track of all
      * BLOCK_FAILED_VALID blocks in a set should be just fine and work just as
      * well.
      *
      * Because we already walk mapBlockIndex in height-order at startup, we go
      * ahead and mark descendants of invalid blocks as FAILED_CHILD at that time,
      * instead of putting things in this set.
      */
    std::set<CBlockIndex*> m_failed_blocks;

public:
    CChain chainActive;
    BlockMap mapBlockIndex;
    std::multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;
    CBlockIndex *pindexBestInvalid = nullptr;

    bool LoadBlockIndex(const Consensus::Params& consensus_params, CBlockTreeDB& blocktree);

    bool ActivateBestChain(CValidationState &state, const CChainParams& chainparams, std::shared_ptr<const CBlock> pblock);

    /**
     * If a block header hasn't already been seen, call CheckBlockHeader on it, ensure
     * that it doesn't descend from an invalid block, and then add it to mapBlockIndex.
     */
    bool AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex);
    bool AcceptBlock(const std::shared_ptr<const CBlock>& pblock, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex, bool fRequested, const CDiskBlockPos* dbp, bool* fNewBlock);

    // Block (dis)connection on a given view:
    DisconnectResult DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view);
    bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex,
                    CCoinsViewCache& view, const CChainParams& chainparams, bool fJustCheck = false);

    // Block disconnection on our pcoinsTip:
    bool DisconnectTip(CValidationState& state, const CChainParams& chainparams, DisconnectedBlockTransactions *disconnectpool);

    // Manual block validity manipulation:
    bool PreciousBlock(CValidationState& state, const CChainParams& params, CBlockIndex *pindex);
    bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex *pindex);
    bool ResetBlockFailureFlags(CBlockIndex *pindex);

    bool ReplayBlocks(const CChainParams& params, CCoinsView* view);
    bool RewindBlockIndex(const CChainParams& params);
    bool LoadGenesisBlock(const CChainParams& chainparams);

    void PruneBlockIndexCandidates();

    void UnloadBlockIndex();

//private:
    bool ActivateBestChainStep(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexMostWork, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, ConnectTrace& connectTrace);
    bool ConnectTip(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexNew, const std::shared_ptr<const CBlock>& pblock, ConnectTrace& connectTrace, DisconnectedBlockTransactions &disconnectpool);

    CBlockIndex* AddToBlockIndex(const CBlockHeader& block);
    /** Create a new block index entry for a given block hash */
    CBlockIndex * InsertBlockIndex(const uint256& hash);
    /**
     * Make various assertions about the state of the block index.
     *
     * By default this only executes fully when using the Regtest chain; see: fCheckBlockIndex.
     */
    void CheckBlockIndex(const Consensus::Params& consensusParams);

    void InvalidBlockFound(CBlockIndex *pindex, const CBlock &block, const CValidationState &state);
    CBlockIndex* FindMostWorkChain();
    bool ReceivedBlockTransactions(const CBlock &block, CValidationState& state, CBlockIndex *pindexNew, const CDiskBlockPos& pos, const Consensus::Params& consensusParams);


    bool RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs, const CChainParams& params);
} g_chainstate;



CCriticalSection cs_main;

BlockMap& mapBlockIndex = g_chainstate.mapBlockIndex;
CChain& chainActive = g_chainstate.chainActive;

std::map<uint256, StakeConflict> mapStakeConflict;
std::map<COutPoint, uint256> mapStakeSeen;
std::list<COutPoint> listStakeSeen;

CoinStakeCache coinStakeCache;
std::set<CCmpPubKey> setConnectKi; // hacky workaround

CBlockIndex *pindexBestHeader = nullptr;
CWaitableCriticalSection g_best_block_mutex;
CConditionVariable g_best_block_cv;
uint256 g_best_block;
int nScriptCheckThreads = 0;
std::atomic_bool fImporting(false);
std::atomic_bool fReindex(false);
bool fSkipRangeproof = false;
bool fBusyImporting = false;        // covers ActivateBestChain too
bool fTxIndex = true;
bool fAddressIndex = false;
bool fTimestampIndex = false;
bool fSpentIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;
bool fEnableReplacement = DEFAULT_ENABLE_REPLACEMENT;
static bool fVerifyingDB = false;

uint256 hashAssumeValid;
arith_uint256 nMinimumChainWork;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

CBlockPolicyEstimator feeEstimator;
CTxMemPool mempool(&feeEstimator);
std::atomic_bool g_is_mempool_loaded{false};

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;


const std::string strMessageMagic = "Bitcoin Signed Message:\n";

extern bool IncomingBlockChecked(const CBlock &block, CValidationState &state);

// Internal stuff
namespace {
    CBlockIndex *&pindexBestInvalid = g_chainstate.pindexBestInvalid;

    /** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     * Pruned nodes may have entries where B is missing data.
     */
    std::multimap<CBlockIndex*, CBlockIndex*>& mapBlocksUnlinked = g_chainstate.mapBlocksUnlinked;

    CCriticalSection cs_LastBlockFile;
    std::vector<CBlockFileInfo> vinfoBlockFile;
    int nLastBlockFile = 0;
    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool fCheckForPruning = false;

    /** Dirty block index entries. */
    std::set<CBlockIndex*> setDirtyBlockIndex;

    /** Dirty block file entries. */
    std::set<int> setDirtyFileInfo;
} // anon namespace

int StakeConflict::Add(NodeId id)
{
    nLastUpdated = GetAdjustedTime();
    std::pair<std::map<NodeId, int>::iterator,bool> ret;
    ret = peerCount.insert(std::pair<NodeId, int>(id, 1));
    if (ret.second == false) // existing element
        ret.first->second++;

    return 0;
};

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    AssertLockHeld(cs_main);

    // Find the latest block common to locator and chain - we expect that
    // locator.vHave is sorted descending by height.
    for (const uint256& hash : locator.vHave) {
        CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            if (chain.Contains(pindex))
                return pindex;
            if (pindex->GetAncestor(chain.Height()) == chain.Tip()) {
                return chain.Tip();
            }
        }
    }
    return chain.Genesis();
}

std::unique_ptr<CCoinsViewDB> pcoinsdbview;
std::unique_ptr<CCoinsViewCache> pcoinsTip;
std::unique_ptr<CBlockTreeDB> pblocktree;

// See definition for documentation
//static bool FlushStateToDisk(const CChainParams& chainParams, CValidationState &state, FlushStateMode mode, int nManualPruneHeight=0);
static void FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight);
static void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight);
bool CheckInputs(const CTransaction& tx, CValidationState &state, const CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, bool cacheSigStore, bool cacheFullScriptStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck> *pvChecks = nullptr, bool fAnonChecks = true);
static FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly = false);

bool CheckFinalTx(const CTransaction &tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 requires that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                             ? chainActive.Tip()->GetMedianTimePast()
                             : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}


bool TestLockPointValidity(const LockPoints* lp)
{
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp->maxInputBlock) {
        // Check whether chainActive is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!chainActive.Contains(lp->maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

bool CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints* lp, bool useExistingLockPoints)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    CBlockIndex* tip = chainActive.Tip();
    assert(tip != nullptr);

    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints) {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    }
    else {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool viewMemPool(pcoinsTip.get(), mempool);
        std::vector<int> prevheights;
        prevheights.resize(tx.vin.size());
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
            const CTxIn& txin = tx.vin[txinIndex];

            if (txin.IsAnonInput())
            {
                prevheights[txinIndex] = tip->nHeight + 1;
                continue;
            };

            Coin coin;
            if (!viewMemPool.GetCoin(txin.prevout, coin)) {
                return error("%s: Missing input", __func__);
            }
            if (coin.nHeight == MEMPOOL_HEIGHT) {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->nHeight + 1;
            } else {
                prevheights[txinIndex] = coin.nHeight;
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp) {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of
            // all the blocks which have sequence locked prevouts.
            // This hash needs to still be on the chain
            // for these LockPoint calculations to be valid
            // Note: It is impossible to correctly calculate a maxInputBlock
            // if any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height
            // is 0, which is equivalent to no sequence lock. Since we assume
            // input height of tip+1 for mempool txs and test the resulting
            // lockPair from CalculateSequenceLocks against tip+1.  We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            for (int height : prevheights) {
                // Can ignore mempool inputs since we'll fail if they had non-zero locks
                if (height != tip->nHeight+1) {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}

// Returns the script flags which should be checked for a given block
static unsigned int GetBlockScriptFlags(const CBlockIndex* pindex, const Consensus::Params& chainparams);


static void LimitMempoolSize(CTxMemPool& pool, size_t limit, unsigned long age) {
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0) {
        LogPrint(BCLog::MEMPOOL, "Expired %i transactions from the memory pool\n", expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint& removed : vNoSpendsRemaining)
        pcoinsTip->Uncache(removed);
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state)
{
    return strprintf("%s%s (code %i)",
        state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", "+state.GetDebugMessage(),
        state.GetRejectCode());
}

static bool IsCurrentForFeeEstimation()
{
    AssertLockHeld(cs_main);
    if (IsInitialBlockDownload())
        return false;
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE))
        return false;
    if (chainActive.Height() < pindexBestHeader->nHeight - 1)
        return false;
    return true;
}

/* Make mempool consistent after a reorg, by re-adding or recursively erasing
 * disconnected block transactions from the mempool, and also removing any
 * other transactions from the mempool that are no longer valid given the new
 * tip/height.
 *
 * Note: we assume that disconnectpool only contains transactions that are NOT
 * confirmed in the current chain nor already in the mempool (otherwise,
 * in-mempool descendants of such transactions would be removed).
 *
 * Passing fAddToMempool=false will skip trying to add the transactions back,
 * and instead just erase from the mempool as needed.
 */

static void UpdateMempoolForReorg(DisconnectedBlockTransactions &disconnectpool, bool fAddToMempool)
{
    AssertLockHeld(cs_main);
    std::vector<uint256> vHashUpdate;
    // disconnectpool's insertion_order index sorts the entries from
    // oldest to newest, but the oldest entry will be the last tx from the
    // latest mined block that was disconnected.
    // Iterate disconnectpool in reverse, so that we add transactions
    // back to the mempool starting with the earliest transaction that had
    // been previously seen in a block.
    auto it = disconnectpool.queuedTx.get<insertion_order>().rbegin();
    while (it != disconnectpool.queuedTx.get<insertion_order>().rend()) {
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!fAddToMempool || (*it)->IsCoinBase() ||
            !AcceptToMemoryPool(mempool, stateDummy, *it, nullptr /* pfMissingInputs */,
                                nullptr /* plTxnReplaced */, true /* bypass_limits */, 0 /* nAbsurdFee */)) {
            // If the transaction doesn't make it in to the mempool, remove any
            // transactions that depend on it (which would now be orphans).
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
        } else if (mempool.exists((*it)->GetHash())) {
            vHashUpdate.push_back((*it)->GetHash());
        }
        ++it;
    }
    disconnectpool.queuedTx.clear();
    // AcceptToMemoryPool/addUnchecked all assume that new mempool entries have
    // no in-mempool children, which is generally not true when adding
    // previously-confirmed transactions back to the mempool.
    // UpdateTransactionsFromBlock finds descendants of any transactions in
    // the disconnectpool that were added back and cleans up the mempool state.
    mempool.UpdateTransactionsFromBlock(vHashUpdate);

    // We also need to remove any now-immature transactions
    mempool.removeForReorg(pcoinsTip.get(), chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    // Re-limit mempool size, in case we added any transactions
    LimitMempoolSize(mempool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
}

// Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
// were somehow broken and returning the wrong scriptPubKeys
static bool CheckInputsFromMempoolAndCache(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& view, const CTxMemPool& pool,
                 unsigned int flags, bool cacheSigStore, PrecomputedTransactionData& txdata) {
    AssertLockHeld(cs_main);

    // pool.cs should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view
    // and when we actually call through to CheckInputs
    LOCK(pool.cs);

    assert(!tx.IsCoinBase());
    for (const CTxIn& txin : tx.vin) {
        if (txin.IsAnonInput())
            continue;
        const Coin& coin = view.AccessCoin(txin.prevout);

        // At this point we haven't actually checked if the coins are all
        // available (or shouldn't assume we have, since CheckInputs does).
        // So we just return failure if the inputs are not available here,
        // and then only have to check equivalence for available inputs.
        if (coin.IsSpent()) return false;

        const CTransactionRef& txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->GetNumVOuts() > txin.prevout.n);
            if (txFrom->IsParticlVersion())
            {
                assert(coin.Matches(txFrom->vpout[txin.prevout.n].get()));
            } else
            {
                assert(txFrom->vout[txin.prevout.n] == coin.out);
            }
        } else {
            const Coin& coinFromDisk = pcoinsTip->AccessCoin(txin.prevout);
            assert(!coinFromDisk.IsSpent());
            assert(coinFromDisk.out == coin.out);
        }
    }

    return CheckInputs(tx, state, view, true, flags, cacheSigStore, true, txdata);
}

static bool AcceptToMemoryPoolWorker(const CChainParams& chainparams, CTxMemPool& pool, CValidationState& state, const CTransactionRef& ptx,
                              bool* pfMissingInputs, int64_t nAcceptTime, std::list<CTransactionRef>* plTxnReplaced,
                              bool bypass_limits, const CAmount& nAbsurdFee, std::vector<COutPoint>& coins_to_uncache, bool test_accept, bool ignore_locks)
{
    const CTransaction& tx = *ptx;
    const uint256 hash = tx.GetHash();
    AssertLockHeld(cs_main);
    LOCK(pool.cs); // mempool "read lock" (held through GetMainSignals().TransactionAddedToMempool())
    if (pfMissingInputs) {
        *pfMissingInputs = false;
    }

    if (!CheckTransaction(tx, state))
        return false; // state filled in by CheckTransaction

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "coinbase");

    // Coinstake is only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return state.DoS(100, false, REJECT_INVALID, "coinstake");

    // Reject transactions with witness before segregated witness activates (override with -prematurewitness)
    bool witnessEnabled = IsWitnessEnabled(chainActive.Tip(), chainparams.GetConsensus());
    if (!gArgs.GetBoolArg("-prematurewitness", false) && tx.HasWitness() && !witnessEnabled) {
        return state.DoS(0, false, REJECT_NONSTANDARD, "no-witness-yet", true);
    }

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason, witnessEnabled))
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);

    // Do not work on transactions that are too small.
    // A transaction with 1 segwit input and 1 P2WPHK output has non-witness size of 82 bytes.
    // Transactions smaller than this are not relayed to reduce unnecessary malloc overhead.
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) < MIN_STANDARD_TX_NONWITNESS_SIZE)
        return state.DoS(0, false, REJECT_NONSTANDARD, "tx-size-small");

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!test_accept || !ignore_locks)
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // is it already in the memory pool?
    if (pool.exists(hash)) {
        return state.Invalid(false, REJECT_DUPLICATE, "txn-already-in-mempool");
    }

    // Check for conflicts with in-memory transactions
    std::set<uint256> setConflicts;
    for (const CTxIn &txin : tx.vin)
    {
        if (txin.IsAnonInput())
            continue;

        auto itConflicting = pool.mapNextTx.find(txin.prevout);
        if (itConflicting != pool.mapNextTx.end())
        {
            const CTransaction *ptxConflicting = itConflicting->second;
            if (!setConflicts.count(ptxConflicting->GetHash()))
            {
                // Allow opt-out of transaction replacement by setting
                // nSequence > MAX_BIP125_RBF_SEQUENCE (SEQUENCE_FINAL-2) on all inputs.
                //
                // SEQUENCE_FINAL-1 is picked to still allow use of nLockTime by
                // non-replaceable transactions. All inputs rather than just one
                // is for the sake of multi-party protocols, where we don't
                // want a single party to be able to disable replacement.
                //
                // The opt-out ignores descendants as anyone relying on
                // first-seen mempool behavior should be checking all
                // unconfirmed ancestors anyway; doing otherwise is hopelessly
                // insecure.
                bool fReplacementOptOut = true;
                if (fEnableReplacement)
                {
                    for (const CTxIn &_txin : ptxConflicting->vin)
                    {
                        if (_txin.nSequence <= MAX_BIP125_RBF_SEQUENCE)
                        {
                            fReplacementOptOut = false;
                            break;
                        }
                    }
                }
                if (fReplacementOptOut) {
                    return state.Invalid(false, REJECT_DUPLICATE, "txn-mempool-conflict");
                }

                setConflicts.insert(ptxConflicting->GetHash());
            }
        }
    }

    state.fHasAnonInput = false;
    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        LockPoints lp;
        CCoinsViewMemPool viewMemPool(pcoinsTip.get(), pool);
        view.SetBackend(viewMemPool);

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // and only helps with filling in pfMissingInputs (to determine missing vs spent).
        for (const auto &txin : tx.vin)
        {
            if (txin.IsAnonInput())
            {
                state.fHasAnonInput = true;
                continue;
            };

            if (!pcoinsTip->HaveCoinInCache(txin.prevout)) {
                coins_to_uncache.push_back(txin.prevout);
            }
            if (!view.HaveCoin(txin.prevout)) {
                // Are inputs missing because we already have the tx?
                for (size_t out = 0; out < tx.GetNumVOuts(); out++) {
                    // Optimistically just do efficient check of cache for outputs
                    if (pcoinsTip->HaveCoinInCache(COutPoint(hash, out))) {
                        return state.Invalid(false, REJECT_DUPLICATE, "txn-already-known");
                    }
                }
                // Otherwise assume this might be an orphan tx for which we just haven't seen parents yet
                if (pfMissingInputs) {
                    *pfMissingInputs = true;
                }
                return false; // fMissingInputs and !state.IsInvalid() is used to detect this condition, don't set state.Invalid()
            }
        };

        if (state.fHasAnonInput
             && (chainActive.Height() < GetNumBlocksOfPeers()-1))
        {
            LogPrintf("%s: Ignoring anon transaction while chain syncs height %d - peers %d.\n",
                __func__, chainActive.Height(), GetNumBlocksOfPeers());
            return false; // Might be missing inputs
        };

        if (!AllAnonOutputsUnknown(tx, state)) // set state.fHasAnonOutput
            return false; // Already in the blockchain, containing block could have been received before loose tx

        // Bring the best block into scope
        view.GetBestBlock();
        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);

        // Only accept BIP68 sequence locked transactions that can be mined in the next
        // block; we don't want our mempool filled up with transactions that can't
        // be mined yet.
        // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
        // CoinsViewCache instead of create its own
        if (!test_accept || !ignore_locks)
        if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp))
            return state.DoS(0, false, REJECT_NONSTANDARD, "non-BIP68-final");

        CAmount nFees = 0;
        if (!Consensus::CheckTxInputs(tx, state, view, GetSpendHeight(view), nFees)) {
            return error("%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
            return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");

        // Check for non-standard witness in P2WSH
        if (tx.HasWitness() && fRequireStandard && !IsWitnessStandard(tx, view))
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-witness-nonstandard", true);

        int64_t nSigOpsCost = GetTransactionSigOpCost(tx, view, STANDARD_SCRIPT_VERIFY_FLAGS);

        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        CAmount nModifiedFees = nFees;
        pool.ApplyDelta(hash, nModifiedFees);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (const CTxIn &txin : tx.vin)
        {
            if (txin.IsAnonInput())
                continue;
            const Coin &coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }


        CTxMemPoolEntry entry(ptx, nFees, nAcceptTime, chainActive.Height(),
                              fSpendsCoinbase, nSigOpsCost, lp);
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        if (nSigOpsCost > MAX_STANDARD_TX_SIGOPS_COST)
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-txns-too-many-sigops", false,
                strprintf("%d", nSigOpsCost));

        CAmount mempoolRejectFee = pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(nSize);
        if (state.fHasAnonOutput)
            mempoolRejectFee *= ANON_FEE_MULTIPLIER;

        if (!bypass_limits && mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met", false, strprintf("%d < %d", nModifiedFees, mempoolRejectFee));
        }

        // No transactions are allowed below minRelayTxFee except from disconnected blocks
        if (!bypass_limits && nModifiedFees < ::minRelayTxFee.GetFee(nSize)) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "min relay fee not met", false, strprintf("%d < %d", nModifiedFees, ::minRelayTxFee.GetFee(nSize)));
        }

        if (nAbsurdFee && nFees > nAbsurdFee)
            return state.Invalid(false,
                REJECT_HIGHFEE, "absurdly-high-fee",
                strprintf("%d > %d", nFees, nAbsurdFee));

        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT)*1000;
        size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT)*1000;
        std::string errString;
        if (!pool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize, nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, false, REJECT_NONSTANDARD, "too-long-mempool-chain", false, errString);
        }

        // A transaction that spends outputs that would be replaced by it is invalid. Now
        // that we have the set of all ancestors we can detect this
        // pathological case by making sure setConflicts and setAncestors don't
        // intersect.
        for (CTxMemPool::txiter ancestorIt : setAncestors)
        {
            const uint256 &hashAncestor = ancestorIt->GetTx().GetHash();
            if (setConflicts.count(hashAncestor))
            {
                return state.DoS(10, false,
                                 REJECT_INVALID, "bad-txns-spends-conflicting-tx", false,
                                 strprintf("%s spends conflicting transaction %s",
                                           hash.ToString(),
                                           hashAncestor.ToString()));
            }
        }

        // Check if it's economically rational to mine this transaction rather
        // than the ones it replaces.
        CAmount nConflictingFees = 0;
        size_t nConflictingSize = 0;
        uint64_t nConflictingCount = 0;
        CTxMemPool::setEntries allConflicting;

        // If we don't hold the lock allConflicting might be incomplete; the
        // subsequent RemoveStaged() and addUnchecked() calls don't guarantee
        // mempool consistency for us.
        const bool fReplacementTransaction = setConflicts.size();
        if (fReplacementTransaction)
        {
            CFeeRate newFeeRate(nModifiedFees, nSize);
            std::set<uint256> setConflictsParents;
            const int maxDescendantsToVisit = 100;
            CTxMemPool::setEntries setIterConflicting;
            for (const uint256 &hashConflicting : setConflicts)
            {
                CTxMemPool::txiter mi = pool.mapTx.find(hashConflicting);
                if (mi == pool.mapTx.end())
                    continue;

                // Save these to avoid repeated lookups
                setIterConflicting.insert(mi);

                // Don't allow the replacement to reduce the feerate of the
                // mempool.
                //
                // We usually don't want to accept replacements with lower
                // feerates than what they replaced as that would lower the
                // feerate of the next block. Requiring that the feerate always
                // be increased is also an easy-to-reason about way to prevent
                // DoS attacks via replacements.
                //
                // The mining code doesn't (currently) take children into
                // account (CPFP) so we only consider the feerates of
                // transactions being directly replaced, not their indirect
                // descendants. While that does mean high feerate children are
                // ignored when deciding whether or not to replace, we do
                // require the replacement to pay more overall fees too,
                // mitigating most cases.
                CFeeRate oldFeeRate(mi->GetModifiedFee(), mi->GetTxSize());
                if (newFeeRate <= oldFeeRate)
                {
                    return state.DoS(0, false,
                            REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                            strprintf("rejecting replacement %s; new feerate %s <= old feerate %s",
                                  hash.ToString(),
                                  newFeeRate.ToString(),
                                  oldFeeRate.ToString()));
                }

                for (const CTxIn &txin : mi->GetTx().vin)
                {
                    setConflictsParents.insert(txin.prevout.hash);
                }

                nConflictingCount += mi->GetCountWithDescendants();
            }
            // This potentially overestimates the number of actual descendants
            // but we just want to be conservative to avoid doing too much
            // work.
            if (nConflictingCount <= maxDescendantsToVisit) {
                // If not too many to replace, then calculate the set of
                // transactions that would have to be evicted
                for (CTxMemPool::txiter it : setIterConflicting) {
                    pool.CalculateDescendants(it, allConflicting);
                }
                for (CTxMemPool::txiter it : allConflicting) {
                    nConflictingFees += it->GetModifiedFee();
                    nConflictingSize += it->GetTxSize();
                }
            } else {
                return state.DoS(0, false,
                        REJECT_NONSTANDARD, "too many potential replacements", false,
                        strprintf("rejecting replacement %s; too many potential replacements (%d > %d)\n",
                            hash.ToString(),
                            nConflictingCount,
                            maxDescendantsToVisit));
            }

            for (unsigned int j = 0; j < tx.vin.size(); j++)
            {
                if (tx.vin[j].IsAnonInput())
                    continue;
                // We don't want to accept replacements that require low
                // feerate junk to be mined first. Ideally we'd keep track of
                // the ancestor feerates and make the decision based on that,
                // but for now requiring all new inputs to be confirmed works.
                if (!setConflictsParents.count(tx.vin[j].prevout.hash))
                {
                    // Rather than check the UTXO set - potentially expensive -
                    // it's cheaper to just check if the new input refers to a
                    // tx that's in the mempool.
                    if (pool.mapTx.find(tx.vin[j].prevout.hash) != pool.mapTx.end())
                        return state.DoS(0, false,
                                         REJECT_NONSTANDARD, "replacement-adds-unconfirmed", false,
                                         strprintf("replacement %s adds unconfirmed input, idx %d",
                                                  hash.ToString(), j));
                }
            }

            // The replacement must pay greater fees than the transactions it
            // replaces - if we did the bandwidth used by those conflicting
            // transactions would not be paid for.
            if (nModifiedFees < nConflictingFees)
            {
                return state.DoS(0, false,
                                 REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                                 strprintf("rejecting replacement %s, less fees than conflicting txs; %s < %s",
                                          hash.ToString(), FormatMoney(nModifiedFees), FormatMoney(nConflictingFees)));
            }

            // Finally in addition to paying more fees than the conflicts the
            // new transaction must pay for its own bandwidth.
            CAmount nDeltaFees = nModifiedFees - nConflictingFees;
            if (nDeltaFees < ::incrementalRelayFee.GetFee(nSize))
            {
                return state.DoS(0, false,
                        REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                        strprintf("rejecting replacement %s, not enough additional fees to relay; %s < %s",
                              hash.ToString(),
                              FormatMoney(nDeltaFees),
                              FormatMoney(::incrementalRelayFee.GetFee(nSize))));
            }
        }

        unsigned int scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (!chainparams.RequireStandard()) {
            scriptVerifyFlags = gArgs.GetArg("-promiscuousmempoolflags", scriptVerifyFlags);
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        PrecomputedTransactionData txdata(tx);

        state.fEnforceSmsgFees = nAcceptTime >= chainparams.GetConsensus().nPaidSmsgTime;
        if (!CheckInputs(tx, state, view, true, scriptVerifyFlags, true, false, txdata)) {
            // SCRIPT_VERIFY_CLEANSTACK requires SCRIPT_VERIFY_WITNESS, so we
            // need to turn both off, and compare against just turning off CLEANSTACK
            // to see if the failure is specifically due to witness validation.
            CValidationState stateDummy; // Want reported failures to be from first CheckInputs
            if (!tx.HasWitness() && CheckInputs(tx, stateDummy, view, true, scriptVerifyFlags & ~(SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_CLEANSTACK), true, false, txdata) &&
                !CheckInputs(tx, stateDummy, view, true, scriptVerifyFlags & ~SCRIPT_VERIFY_CLEANSTACK, true, false, txdata)) {
                // Only the witness is missing, so the transaction itself may be fine.
                state.SetCorruptionPossible();
            }
            return false; // state filled in by CheckInputs
        }

        // Check again against the current block tip's script verification
        // flags to cache our script execution flags. This is, of course,
        // useless if the next block has different script flags from the
        // previous one, but because the cache tracks script flags for us it
        // will auto-invalidate and we'll just have a few blocks of extra
        // misses on soft-fork activation.
        //
        // This is also useful in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks (using TestBlockValidity), however allowing such
        // transactions into the mempool can be exploited as a DoS attack.
        unsigned int currentBlockScriptVerifyFlags = GetBlockScriptFlags(chainActive.Tip(), Params().GetConsensus());
        if (!CheckInputsFromMempoolAndCache(tx, state, view, pool, currentBlockScriptVerifyFlags, true, txdata)) {
            // If we're using promiscuousmempoolflags, we may hit this normally
            // Check if current block has some flags that scriptVerifyFlags
            // does not before printing an ominous warning
            if (!(~scriptVerifyFlags & currentBlockScriptVerifyFlags)) {
                return error("%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against latest-block but not STANDARD flags %s, %s",
                    __func__, hash.ToString(), FormatStateMessage(state));
            } else {
                if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, false, txdata)) {
                    return error("%s: ConnectInputs failed against MANDATORY but not STANDARD flags due to promiscuous mempool %s, %s",
                        __func__, hash.ToString(), FormatStateMessage(state));
                } else {
                    LogPrintf("Warning: -promiscuousmempool flags set to not include currently enforced soft forks, this may break mining or otherwise cause instability!\n");
                }
            }
        }

        if (test_accept) {
            // Tx was accepted, but not added
            return true;
        }

        // Remove conflicting transactions from the mempool
        for (const CTxMemPool::txiter it : allConflicting)
        {
            LogPrint(BCLog::MEMPOOL, "replacing tx %s with %s for %s PART additional fees, %d delta bytes\n",
                    it->GetTx().GetHash().ToString(),
                    hash.ToString(),
                    FormatMoney(nModifiedFees - nConflictingFees),
                    CURRENCY_UNIT.c_str(),
                    (int)nSize - (int)nConflictingSize);
            if (plTxnReplaced)
                plTxnReplaced->push_back(it->GetSharedTx());
        }
        pool.RemoveStaged(allConflicting, false, MemPoolRemovalReason::REPLACED);

        // This transaction should only count for fee estimation if:
        // - it isn't a BIP 125 replacement transaction (may not be widely supported)
        // - it's not being re-added during a reorg which bypasses typical mempool fee limits
        // - the node is not behind
        // - the transaction is not dependent on any other transactions in the mempool
        bool validForFeeEstimation = !fReplacementTransaction && !bypass_limits && IsCurrentForFeeEstimation() && pool.HasNoInputsOf(tx);

        // Store transaction in memory
        pool.addUnchecked(hash, entry, setAncestors, validForFeeEstimation);

        // Add memory address index
        if (fAddressIndex)
            pool.addAddressIndex(entry, view);

        // Add memory spent index
        if (fSpentIndex)
            pool.addSpentIndex(entry, view);

        // trim mempool and check if tx was trimmed
        if (!bypass_limits) {
            LimitMempoolSize(pool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(hash))
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
        }
    }

    if (!AddKeyImagesToMempool(tx, pool))
        return state.DoS(100, error("%s: AddKeyImagesToMempool failed.", __func__), REJECT_MALFORMED, "bad-anonin-keyimages");

    GetMainSignals().TransactionAddedToMempool(ptx);

    return true;
}

/** (try to) add transaction to memory pool with a specified acceptance time **/
static bool AcceptToMemoryPoolWithTime(const CChainParams& chainparams, CTxMemPool& pool, CValidationState &state, const CTransactionRef &tx,
                        bool* pfMissingInputs, int64_t nAcceptTime, std::list<CTransactionRef>* plTxnReplaced,
                        bool bypass_limits, const CAmount nAbsurdFee, bool test_accept, bool ignore_locks)
{
    std::vector<COutPoint> coins_to_uncache;
    bool res = AcceptToMemoryPoolWorker(chainparams, pool, state, tx, pfMissingInputs, nAcceptTime, plTxnReplaced, bypass_limits, nAbsurdFee, coins_to_uncache, test_accept, ignore_locks);
    if (!res) {
        for (const COutPoint& hashTx : coins_to_uncache)
            pcoinsTip->Uncache(hashTx);
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    CValidationState stateDummy;
    FlushStateToDisk(chainparams, stateDummy, FlushStateMode::PERIODIC);
    return res;
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState &state, const CTransactionRef &tx,
                        bool* pfMissingInputs, std::list<CTransactionRef>* plTxnReplaced,
                        bool bypass_limits, const CAmount nAbsurdFee, bool test_accept, bool ignore_locks)
{
    const CChainParams& chainparams = Params();
    return AcceptToMemoryPoolWithTime(chainparams, pool, state, tx, pfMissingInputs, GetTime(), plTxnReplaced, bypass_limits, nAbsurdFee, test_accept, ignore_locks);
}

bool GetTimestampIndex(const unsigned int &high, const unsigned int &low, const bool fActiveOnly, std::vector<std::pair<uint256, unsigned int> > &hashes)
{
    if (!fTimestampIndex)
        return error("Timestamp index not enabled");

    if (!pblocktree->ReadTimestampIndex(high, low, fActiveOnly, hashes))
        return error("Unable to get hashes for timestamps");

    return true;
}

bool GetSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value)
{
    if (!fSpentIndex)
        return false;

    if (mempool.getSpentIndex(key, value))
        return true;

    if (!pblocktree->ReadSpentIndex(key, value))
        return false;

    return true;
}

bool HashOnchainActive(const uint256 &hash)
{
    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (!chainActive.Contains(pblockindex)) {
        return false;
    }

    return true;
}

bool GetAddressIndex(uint256 addressHash, int type,
                     std::vector<std::pair<CAddressIndexKey, CAmount> > &addressIndex, int start, int end)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressIndex(addressHash, type, addressIndex, start, end))
        return error("unable to get txids for address");

    return true;
}

bool GetAddressUnspent(uint256 addressHash, int type,
                       std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > &unspentOutputs)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressUnspentIndex(addressHash, type, unspentOutputs))
        return error("unable to get txids for address");

    return true;
}

/**
 * Return transaction in txOut, and if it was found inside a block, its hash is placed in hashBlock.
 * If blockIndex is provided, the transaction is fetched from the corresponding block.
 */
bool GetTransaction(const uint256& hash, CTransactionRef& txOut, const Consensus::Params& consensusParams, uint256& hashBlock, bool fAllowSlow, CBlockIndex* blockIndex)
{
    CBlockIndex* pindexSlow = blockIndex;

    LOCK(cs_main);

    if (!blockIndex) {
        CTransactionRef ptx = mempool.get(hash);
        if (ptx) {
            txOut = ptx;
            return true;
        }

        if (g_txindex) {
            return g_txindex->FindTx(hash, hashBlock, txOut);
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            const Coin& coin = AccessByTxid(*pcoinsTip, hash);
            if (!coin.IsSpent()) pindexSlow = chainActive[coin.nHeight];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


/** Retrieve a transaction and block header from disk
  * If blockIndex is provided, the transaction is fetched from the corresponding block.
  */
bool GetTransaction(const uint256 &hash, CTransactionRef &txOut, const Consensus::Params &consensusParams, CBlock &block, bool fAllowSlow, CBlockIndex* blockIndex)
{
    CBlockIndex *pindexSlow = blockIndex;

    LOCK(cs_main);

    if (fTxIndex) {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(hash, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            } catch (const std::exception& e) {
                return error("%s: Deserialize or I/O error - %s", __func__, e.what());
            }
            block = CBlock(header);
            if (txOut->GetHash() != hash)
                return error("%s: txid mismatch", __func__);
            return true;
        }
    }

    if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
        const Coin& coin = AccessByTxid(*pcoinsTip, hash);
        if (!coin.IsSpent()) pindexSlow = chainActive[coin.nHeight];
    }

    if (pindexSlow) {
        // read and return entire block
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    txOut = tx;
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static bool WriteBlockToDisk(const CBlock& block, CDiskBlockPos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << messageStart << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    // Read block
    try {
        filein >> block;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    // Check the header
    if (fParticlMode)
    {
        // only CheckProofOfWork for genesis blocks
        if (block.hashPrevBlock.IsNull()
            && !CheckProofOfWork(block.GetHash(), block.nBits, consensusParams, 0, Params().GetLastImportHeight()))
            return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());
    } else
    {
        if (!CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
            return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());
    }

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    CDiskBlockPos blockPos;
    {
        LOCK(cs_main);
        blockPos = pindex->GetBlockPos();
    }

    if (!ReadBlockFromDisk(block, blockPos, consensusParams))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

bool ReadTransactionFromDiskBlock(const CBlockIndex* pindex, int nIndex, CTransactionRef &txOut)
{
    const CDiskBlockPos &pos = pindex->GetBlockPos();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenBlockFile failed for %s", __func__, pos.ToString());

    CBlockHeader blockHeader;
    try {
        filein >> blockHeader;

        int nTxns = ReadCompactSize(filein);

        if (nTxns <= nIndex || nIndex < 0)
            return error("%s: Block %s, txn %d not in available range %d.", __func__, pindex->GetBlockPos().ToString(), nIndex, nTxns);

        for (int k = 0; k <= nIndex; ++k)
            filein >> txOut;
    } catch (const std::exception& e)
    {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    if (blockHeader.GetHash() != pindex->GetBlockHash())
        return error("%s: Hash doesn't match index for %s at %s",
                __func__, pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}


CAmount GetBlockSubsidy(int nHeight, const Consensus::Params& consensusParams)
{
    int halvings = nHeight / consensusParams.nSubsidyHalvingInterval;
    // Force block reward to zero when right shift is undefined.
    if (halvings >= 64)
        return 0;

    CAmount nSubsidy = 50 * COIN;
    // Subsidy is cut in half every 210,000 blocks which will occur approximately every 4 years.
    nSubsidy >>= halvings;
    return nSubsidy;
}

int GetNumPeers()
{
    //return g_connman->GetNodeCount(CConnman::CONNECTIONS_IN); // doesn't seem accurate
    return g_connman ? g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) : 0;
}

int GetNumBlocksOfPeers()
{
    LOCK(cs_main);

    int nPeerBlocks = g_connman ? g_connman->cPeerBlockCounts.median() : 0;
    CBlockIndex *pcheckpoint = Checkpoints::GetLastCheckpoint(Params().Checkpoints());
    if (pcheckpoint)
        return std::max(nPeerBlocks, pcheckpoint->nHeight);
    return nPeerBlocks;
}

bool IsInitialBlockDownload()
{
    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;
    if (fImporting || fReindex)
        return true;
    if (chainActive.Tip() == nullptr)
        return true;
    if (chainActive.Tip()->nChainWork < nMinimumChainWork)
        return true;
    if (fRequireStandard // fRequireStandard is false on testnet
        && chainActive.Tip()->nHeight > COINBASE_MATURITY
        && chainActive.Tip()->GetBlockTime() < (GetTime() - nMaxTipAge))
        return true;
    if (GetNumPeers() < 1
        || chainActive.Tip()->nHeight < GetNumBlocksOfPeers())
        return true;

    LogPrintf("Leaving InitialBlockDownload (latching to false)\n");
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

CBlockIndex *pindexBestForkTip = nullptr, *pindexBestForkBase = nullptr;

static void AlertNotify(const std::string& strMessage)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = gArgs.GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote+safeStatus+singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    std::thread t(runCommand, strCmd);
    t.detach(); // thread runs free
}

static void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial sync)
    if (IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 12 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = nullptr;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (GetBlockProof(*chainActive.Tip()) * 6)))
    {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase)
        {
            std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            AlertNotify(warning);
        }
        if (pindexBestForkTip && pindexBestForkBase)
        {
            LogPrintf("%s: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n", __func__,
                   pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                   pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
            SetfLargeWorkForkFound(true);
        }
        else
        {
            LogPrintf("%s: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n", __func__);
            SetfLargeWorkInvalidChainFound(true);
        }
    }
    else
    {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

static void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger)
    {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at least 7 blocks
    // with a tip within 72 blocks (+/- 12 hours if no one mines it) of ours
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || pindexNewForkTip->nHeight > pindexBestForkTip->nHeight) &&
            pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
            chainActive.Height() - pindexNewForkTip->nHeight < 72)
    {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("%s: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      log(pindexNew->nChainWork.getdouble())/log(2.0), FormatISO8601DateTime(pindexNew->GetBlockTime()));
    CBlockIndex *tip = chainActive.Tip();
    assert (tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      tip->GetBlockHash().ToString(), chainActive.Height(), log(tip->nChainWork.getdouble())/log(2.0),
      FormatISO8601DateTime(tip->GetBlockTime()));
    CheckForkWarningConditions();
}

void CChainState::InvalidBlockFound(CBlockIndex *pindex, const CBlock &block, const CValidationState &state) {
    if (state.GetRejectReason() == "bad-cs-duplicate")
    {
        pindex->SetProofOfStake();
        pindex->prevoutStake = block.vtx[0]->vin[0].prevout;
        if (pindex->pprev && pindex->pprev->bnStakeModifier.IsNull())
            LogPrintf("Warning: %s - Previous stake modifier is null.\n", __func__);
        else
            pindex->bnStakeModifier = ComputeStakeModifierV2(pindex->pprev, pindex->prevoutStake.hash);

        pindex->nFlags |= BLOCK_FAILED_DUPLICATE_STAKE;
        setDirtyBlockIndex.insert(pindex);
    };

    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        m_failed_blocks.insert(pindex);
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo &txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn &txin : tx.vin)
        {
            if (txin.IsAnonInput())
                continue;

            txundo.vprevout.emplace_back();
            bool is_spent = inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
            assert(is_spent);
        }
    }
    // add outputs
    AddCoins(inputs, tx, nHeight);
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, int nHeight)
{
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()() {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    const CScriptWitness *witness = &ptxTo->vin[nIn].scriptWitness;

    return VerifyScript(scriptSig, scriptPubKey, witness, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, vchAmount, cacheStore, *txdata), &error);
    //return VerifyScript(scriptSig, m_tx_out.scriptPubKey, witness, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, m_tx_out.nValue, cacheStore, *txdata), &error);
}

int GetSpendHeight(const CCoinsViewCache& inputs)
{
    LOCK(cs_main);

    const CBlockIndex* pindexPrev = LookupBlockIndex(inputs.GetBestBlock());

    if (!pindexPrev)
        return 0;

    return pindexPrev->nHeight + 1;
}

static CuckooCache::cache<uint256, SignatureCacheHasher> scriptExecutionCache;
static uint256 scriptExecutionCacheNonce(GetRandHash());

void InitScriptExecutionCache() {
    // nMaxCacheSize is unsigned. If -maxsigcachesize is set to zero,
    // setup_bytes creates the minimum possible cache (2 elements).
    size_t nMaxCacheSize = std::min(std::max((int64_t)0, gArgs.GetArg("-maxsigcachesize", DEFAULT_MAX_SIG_CACHE_SIZE) / 2), MAX_MAX_SIG_CACHE_SIZE) * ((size_t) 1 << 20);
    size_t nElems = scriptExecutionCache.setup_bytes(nMaxCacheSize);
    LogPrintf("Using %zu MiB out of %zu/2 requested for script execution cache, able to store %zu elements\n",
            (nElems*sizeof(uint256)) >>20, (nMaxCacheSize*2)>>20, nElems);
}

/**
 * Check whether all inputs of this transaction are valid (no double spends, scripts & sigs, amounts)
 * This does not modify the UTXO set.
 *
 * If pvChecks is not nullptr, script checks are pushed onto it instead of being performed inline. Any
 * script checks which are not necessary (eg due to script execution cache hits) are, obviously,
 * not pushed onto pvChecks/run.
 *
 * Setting cacheSigStore/cacheFullScriptStore to false will remove elements from the corresponding cache
 * which are matched. This is useful for checking blocks where we will likely never need the cache
 * entry again.
 *
 * Non-static (and re-declared) in src/test/txvalidationcache_tests.cpp
 */
bool CheckInputs(const CTransaction& tx, CValidationState &state, const CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, bool cacheSigStore, bool cacheFullScriptStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck> *pvChecks, bool fAnonChecks)
{
    if (!tx.IsCoinBase())
    {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip script verification when connecting blocks under the
        // assumevalid block. Assuming the assumevalid block is valid this
        // is safe because block merkle hashes are still computed and checked,
        // Of course, if an assumed valid block is invalid due to false scriptSigs
        // this optimization would allow an invalid chain to be accepted.
        if (fScriptChecks) {
            bool fHasAnonInput = false;
            // First check if script executions have been cached with the same
            // flags. Note that this assumes that the inputs provided are
            // correct (ie that the transaction hash which is in tx's prevouts
            // properly commits to the scriptPubKey in the inputs view of that
            // transaction).
            uint256 hashCacheEntry;
            // We only use the first 19 bytes of nonce to avoid a second SHA
            // round - giving us 19 + 32 + 4 = 55 bytes (+ 8 + 1 = 64)
            static_assert(55 - sizeof(flags) - 32 >= 128/8, "Want at least 128 bits of nonce for script execution cache");
            CSHA256().Write(scriptExecutionCacheNonce.begin(), 55 - sizeof(flags) - 32).Write(tx.GetWitnessHash().begin(), 32).Write((unsigned char*)&flags, sizeof(flags)).Finalize(hashCacheEntry.begin());
            AssertLockHeld(cs_main); //TODO: Remove this requirement by making CuckooCache not require external locks

            if (scriptExecutionCache.contains(hashCacheEntry, !cacheFullScriptStore)) {
                return true;
            }

            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.vin[i].IsAnonInput())
                {
                    fHasAnonInput = true;
                    continue;
                };

                const COutPoint &prevout = tx.vin[i].prevout;
                const Coin& coin = inputs.AccessCoin(prevout);
                assert(!coin.IsSpent());

                // We very carefully only pass in things to CScriptCheck which
                // are clearly committed to by tx' witness hash. This provides
                // a sanity check that our caching is not introducing consensus
                // failures through additional data in, eg, the coins being
                // spent being checked as a part of CScriptCheck.
                const CScript& scriptPubKey = coin.out.scriptPubKey;
                const CAmount amount = coin.out.nValue;

                std::vector<uint8_t> vchAmount;
                if (coin.nType == OUTPUT_STANDARD)
                {
                    vchAmount.resize(8);
                    memcpy(vchAmount.data(), &amount, sizeof(amount));
                } else
                if (coin.nType == OUTPUT_CT)
                {
                    vchAmount.resize(33);
                    memcpy(vchAmount.data(), coin.commitment.data, 33);
                };

                // Verify signature
                CScriptCheck check(scriptPubKey, vchAmount, tx, i, flags, cacheSigStore, &txdata);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(scriptPubKey, vchAmount, tx, i,
                                flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheSigStore, &txdata);

                        if (check2())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. an invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after soft-fork
                    // super-majority signaling has occurred.
                    return state.DoS(100,false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }

            if (fHasAnonInput && fAnonChecks
                && !VerifyMLSAG(tx, state))
                    return false;

            if (cacheFullScriptStore && !pvChecks) {
                // We executed all of the provided scripts, and were told to
                // cache the result. Do so now.
                scriptExecutionCache.insert(hashCacheEntry);
            }
        }
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo& blockundo, CDiskBlockPos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, blockundo);
    fileout << messageStart << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

static bool UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex *pindex)
{
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        return error("%s: no undo data available", __func__);
    }

    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Read block
    uint256 hashChecksum;
    CHashVerifier<CAutoFile> verifier(&filein); // We need a CHashVerifier as reserializing may lose data
    try {
        verifier << pindex->pprev->GetBlockHash();
        verifier >> blockundo;
        filein >> hashChecksum;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash())
        return error("%s: Checksum mismatch", __func__);

    return true;
}

/** Abort with a message */
static bool AbortNode(const std::string& strMessage, const std::string& userMessage="")
{
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

static bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage="")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

} // namespace

/**
 * Restore the UTXO in a Coin at a given COutPoint
 * @param undo The Coin to be restored.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return A DisconnectResult as an int
 */
int ApplyTxInUndo(Coin&& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    if (view.HaveCoin(out)) fClean = false; // overwriting transaction output

    if (undo.nHeight == 0) {
        // Missing undo metadata (height and coinbase). Older versions included this
        // information only in undo records for the last spend of a transactions'
        // outputs. This implies that it must be present for some other output of the same tx.
        const Coin& alternate = AccessByTxid(view, out.hash);
        if (!alternate.IsSpent()) {
            undo.nHeight = alternate.nHeight;
            undo.fCoinBase = alternate.fCoinBase;
        } else {
            return DISCONNECT_FAILED; // adding output for transaction without known metadata
        }
    }
    // The potential_overwrite parameter to AddCoin is only allowed to be false if we know for
    // sure that the coin did not already exist in the cache. As we have queried for that above
    // using HaveCoin, we don't need to guess. When fClean is false, a coin already existed and
    // it is an overwrite.
    view.AddCoin(out, std::move(undo), !fClean);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  When FAILED is returned, view is left in an indeterminate state. */
DisconnectResult CChainState::DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view)
{
    if (LogAcceptCategory(BCLog::HDWALLET))
        LogPrintf("%s: hash %s, height %d\n", __func__, block.GetHash().ToString(), pindex->nHeight);

    assert(pindex->GetBlockHash() == view.GetBestBlock());

    bool fClean = true;

    CBlockUndo blockUndo;
    if (!UndoReadFromDisk(blockUndo, pindex)) {
        error("DisconnectBlock(): failure reading undo data");
        return DISCONNECT_FAILED;
    }

    if (!fParticlMode)
    {
        if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
            error("DisconnectBlock(): block and undo data inconsistent");
            return DISCONNECT_FAILED;
        }
    } else
    {
        if (blockUndo.vtxundo.size() != block.vtx.size())
        {
            // Count non coinbase txns, this should only happen in early blocks.
            size_t nExpectTxns = 0;
            for (auto &tx : block.vtx)
                if (!tx->IsCoinBase())
                    nExpectTxns++;

            if (blockUndo.vtxundo.size() != nExpectTxns) {
                error("DisconnectBlock(): block and undo data inconsistent");
                return DISCONNECT_FAILED;
            };
        };
    };

    int nVtxundo = blockUndo.vtxundo.size()-1;
    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--)
    {
        const CTransaction &tx = *(block.vtx[i]);
        uint256 hash = tx.GetHash();

        for (const auto &txin : tx.vin)
        {
            if (txin.IsAnonInput())
            {
                uint32_t nInputs, nRingSize;
                txin.GetAnonInfo(nInputs, nRingSize);
                if (txin.scriptData.stack.size() != 1
                    || txin.scriptData.stack[0].size() != 33 * nInputs) {
                    error("%s: Bad scriptData stack, %s.", __func__, hash.ToString());
                    return DISCONNECT_FAILED;
                };

                const std::vector<uint8_t> &vKeyImages = txin.scriptData.stack[0];
                for (size_t k = 0; k < nInputs; ++k)
                {
                    const CCmpPubKey &ki = *((CCmpPubKey*)&vKeyImages[k*33]);

                    view.keyImages.push_back(std::make_pair(ki, hash));
                };
            };
        };

        bool is_coinbase = tx.IsCoinBase() || tx.IsCoinStake();

        for (size_t k = tx.vpout.size(); k-- > 0;)
        {
            const CTxOutBase *out = tx.vpout[k].get();

            if (out->IsType(OUTPUT_RINGCT))
            {
                CTxOutRingCT *txout = (CTxOutRingCT*)out;

                if (view.nLastRCTOutput == 0)
                {
                    view.nLastRCTOutput = pindex->nAnonOutputs;
                    // Verify data matches
                    CAnonOutput ao;
                    if (!pblocktree->ReadRCTOutput(view.nLastRCTOutput, ao)) {
                        error("%s: RCT output missing, txn %s, %d, index %d.", __func__, hash.ToString(), k, view.nLastRCTOutput);
                        if (!view.fForceDisconnect)
                            return DISCONNECT_FAILED;
                    } else
                    if (ao.pubkey != txout->pk) {
                        error("%s: RCT output mismatch, txn %s, %d, index %d.", __func__, hash.ToString(), k, view.nLastRCTOutput);
                        if (!view.fForceDisconnect)
                            return DISCONNECT_FAILED;
                    };
                };

                view.anonOutputLinks[txout->pk] = view.nLastRCTOutput;
                view.nLastRCTOutput--;

                continue;
            };

            // Check that all outputs are available and match the outputs in the block itself
            // exactly.
            if (out->IsType(OUTPUT_STANDARD) || out->IsType(OUTPUT_CT))
            {
                const CScript *pScript = out->GetPScriptPubKey();
                if (!pScript->IsUnspendable()) {
                    COutPoint op(hash, k);
                    Coin coin;

                    CTxOut txout(0, *pScript);

                    if (out->IsType(OUTPUT_STANDARD))
                        txout.nValue = out->GetValue();
                    bool is_spent = view.SpendCoin(op, &coin);
                    if (!is_spent || txout != coin.out || pindex->nHeight != coin.nHeight || is_coinbase != coin.fCoinBase) {
                        fClean = false; // transaction output mismatch
                    }
                }
            };

            if (!fAddressIndex
                || (!out->IsType(OUTPUT_STANDARD)
                && !out->IsType(OUTPUT_CT)))
                continue;

            const CScript *pScript;
            std::vector<unsigned char> hashBytes;
            int scriptType = 0;
            CAmount nValue;
            if (!ExtractIndexInfo(out, scriptType, hashBytes, nValue, pScript)
                || scriptType == 0)
                continue;
            // undo receiving activity
            view.addressIndex.push_back(std::make_pair(CAddressIndexKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), pindex->nHeight, i, hash, k, false), nValue));
            // undo unspent index
            view.addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), hash, k), CAddressUnspentValue()));
        };


        if (fParticlMode)
        {
            // restore inputs
            if (!tx.IsCoinBase())
            {
                if (nVtxundo < 0 || nVtxundo >= (int)blockUndo.vtxundo.size())
                {
                    error("DisconnectBlock(): transaction undo data offset out of range.");
                    return DISCONNECT_FAILED;
                };

                size_t nExpectUndo = 0;
                for (const auto &txin : tx.vin)
                if (!txin.IsAnonInput())
                    nExpectUndo++;

                CTxUndo &txundo = blockUndo.vtxundo[nVtxundo--];
                if (txundo.vprevout.size() != nExpectUndo)
                {
                    error("DisconnectBlock(): transaction and undo data inconsistent");
                    return DISCONNECT_FAILED;
                };

                for (unsigned int j = tx.vin.size(); j-- > 0;)
                {
                    if (tx.vin[j].IsAnonInput())
                        continue;

                    const COutPoint &out = tx.vin[j].prevout;
                    int res = ApplyTxInUndo(std::move(txundo.vprevout[j]), view, out);
                    if (res == DISCONNECT_FAILED) return DISCONNECT_FAILED;
                    fClean = fClean && res != DISCONNECT_UNCLEAN;

                    const CTxIn input = tx.vin[j];

                    if (fSpentIndex) // undo and delete the spent index
                        view.spentIndex.push_back(std::make_pair(CSpentIndexKey(input.prevout.hash, input.prevout.n), CSpentIndexValue()));

                    if (fAddressIndex)
                    {
                        const Coin &coin = view.AccessCoin(tx.vin[j].prevout);
                        const CScript *pScript = &coin.out.scriptPubKey;

                        CAmount nValue = coin.nType == OUTPUT_CT ? 0 : coin.out.nValue;
                        std::vector<uint8_t> hashBytes;
                        int scriptType = 0;
                        if (!ExtractIndexInfo(pScript, scriptType, hashBytes)
                            || scriptType == 0)
                            continue;

                        // undo spending activity
                        view.addressIndex.push_back(std::make_pair(CAddressIndexKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), pindex->nHeight, i, hash, j, true), nValue * -1));
                        // restore unspent index
                        view.addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), input.prevout.hash, input.prevout.n), CAddressUnspentValue(nValue, *pScript, coin.nHeight)));
                    }; // if (fAddressIndex)
                }; // for (unsigned int j = tx.vin.size(); j-- > 0;)
            };
        } else
        {
            // Check that all outputs are available and match the outputs in the block itself
            // exactly.
            for (size_t o = 0; o < tx.vout.size(); o++) {
                if (!tx.vout[o].scriptPubKey.IsUnspendable()) {
                    COutPoint out(hash, o);
                    Coin coin;
                    bool is_spent = view.SpendCoin(out, &coin);
                    if (!is_spent || tx.vout[o] != coin.out || pindex->nHeight != coin.nHeight || is_coinbase != coin.fCoinBase) {
                        fClean = false; // transaction output mismatch
                    }
                }
            }

            if (i > 0) { // not coinbases
                CTxUndo &txundo = blockUndo.vtxundo[i-1];
                if (txundo.vprevout.size() != tx.vin.size()) {
                    error("DisconnectBlock(): transaction and undo data inconsistent");
                    return DISCONNECT_FAILED;
                }
                for (unsigned int j = tx.vin.size(); j-- > 0;) {
                    const COutPoint &out = tx.vin[j].prevout;
                    int res = ApplyTxInUndo(std::move(txundo.vprevout[j]), view, out);
                    if (res == DISCONNECT_FAILED) return DISCONNECT_FAILED;
                    fClean = fClean && res != DISCONNECT_UNCLEAN;
                }
            }
            // At this point, all of txundo.vprevout should have been moved out.
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash(), pindex->pprev->nHeight);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex,
    CCoinsViewCache& view, const CChainParams& chainparams, bool fJustCheck)
{
    return g_chainstate.ConnectBlock(block, state, pindex, view, chainparams, fJustCheck);
};

DisconnectResult DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view)
{
    return g_chainstate.DisconnectBlock(block, pindex, view);
};

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);
    bool status = true;

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            status &= TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        status &= FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            status &= TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        status &= FileCommit(fileOld);
        fclose(fileOld);
    }

    if (!status) {
        AbortNode("Flushing block file to disk failed. This is likely the result of an I/O error.");
    }
}

static bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static bool WriteUndoDataForBlock(const CBlockUndo& blockundo, CValidationState& state, CBlockIndex* pindex, const CChainParams& chainparams)
{
    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull()) {
        CDiskBlockPos _pos;
        if (!FindUndoPos(state, pindex->nFile, _pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
            return error("ConnectBlock(): FindUndoPos failed");
        if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(), chainparams.MessageStart()))
            return AbortNode(state, "Failed to write undo data");

        // update nUndoPos in block index
        pindex->nUndoPos = _pos.nPos;
        pindex->nStatus |= BLOCK_HAVE_UNDO;
        setDirtyBlockIndex.insert(pindex);
    }

    return true;
}

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("particl-scriptch");
    scriptcheckqueue.Thread();
}

// Protected by cs_main
VersionBitsCache versionbitscache;

int32_t ComputeBlockVersion(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        ThresholdState state = VersionBitsState(pindexPrev, params, static_cast<Consensus::DeploymentPos>(i), versionbitscache);
        if (state == ThresholdState::LOCKED_IN || state == ThresholdState::STARTED) {
            nVersion |= VersionBitsMask(params, static_cast<Consensus::DeploymentPos>(i));
        }
    }

    return nVersion;
}

/**
 * Threshold condition checker that triggers when unknown versionbits are seen on the network.
 */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker
{
private:
    int bit;

public:
    explicit WarningBitsConditionChecker(int bitIn) : bit(bitIn) {}

    int64_t BeginTime(const Consensus::Params& params) const override { return 0; }
    int64_t EndTime(const Consensus::Params& params) const override { return std::numeric_limits<int64_t>::max(); }
    int Period(const Consensus::Params& params) const override { return params.nMinerConfirmationWindow; }
    int Threshold(const Consensus::Params& params) const override { return params.nRuleChangeActivationThreshold; }

    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override
    {
        return ((pindex->nVersion & VERSIONBITS_TOP_MASK) == VERSIONBITS_TOP_BITS) &&
               ((pindex->nVersion >> bit) & 1) != 0 &&
               ((ComputeBlockVersion(pindex->pprev, params) >> bit) & 1) == 0;
    }
};

// Protected by cs_main
static ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

// 0.13.0 was shipped with a segwit deployment defined for testnet, but not for
// mainnet. We no longer need to support disabling the segwit deployment
// except for testing purposes, due to limitations of the functional test
// environment. See test/functional/p2p-segwit.py.
static bool IsScriptWitnessEnabled(const Consensus::Params& params)
{
    return params.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout != 0;
}

static unsigned int GetBlockScriptFlags(const CBlockIndex* pindex, const Consensus::Params& consensusparams) {
    AssertLockHeld(cs_main);

    if (fParticlMode)
    {
        unsigned int flags = SCRIPT_VERIFY_P2SH;
        flags |= SCRIPT_VERIFY_DERSIG;
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
        flags |= SCRIPT_VERIFY_WITNESS;
        flags |= SCRIPT_VERIFY_NULLDUMMY;
        return flags;
    };

    unsigned int flags = SCRIPT_VERIFY_NONE;

    // BIP16 didn't become active until Apr 1 2012 (on mainnet, and
    // retroactively applied to testnet)
    // However, only one historical block violated the P2SH rules (on both
    // mainnet and testnet), so for simplicity, always leave P2SH
    // on except for the one violating block.
    if (consensusparams.BIP16Exception.IsNull() || // no bip16 exception on this chain
        pindex->phashBlock == nullptr || // this is a new candidate block, eg from TestBlockValidity()
        *pindex->phashBlock != consensusparams.BIP16Exception) // this block isn't the historical exception
    {
        flags |= SCRIPT_VERIFY_P2SH;
    }

    // Enforce WITNESS rules whenever P2SH is in effect (and the segwit
    // deployment is defined).
    if (flags & SCRIPT_VERIFY_P2SH && IsScriptWitnessEnabled(consensusparams)) {
        flags |= SCRIPT_VERIFY_WITNESS;
    }

    // Start enforcing the DERSIG (BIP66) rule
    if (pindex->nHeight >= consensusparams.BIP66Height) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Start enforcing CHECKLOCKTIMEVERIFY (BIP65) rule
    if (pindex->nHeight >= consensusparams.BIP65Height) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Start enforcing BIP68 (sequence locks) and BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    if (VersionBitsState(pindex->pprev, consensusparams, Consensus::DEPLOYMENT_CSV, versionbitscache) == ThresholdState::ACTIVE) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    }

    if (IsNullDummyEnabled(pindex->pprev, consensusparams)) {
        flags |= SCRIPT_VERIFY_NULLDUMMY;
    }

    return flags;
}



static int64_t nTimeCheck = 0;
static int64_t nTimeForks = 0;
static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;
static int64_t nBlocksTotal = 0;

/** Apply the effects of this block (with given index) on the UTXO set represented by coins.
 *  Validity checks that depend on the UTXO set are also done; ConnectBlock()
 *  can fail if those validity checks fail (among other reasons). */
bool CChainState::ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex,
                  CCoinsViewCache& view, const CChainParams& chainparams, bool fJustCheck)
{
    AssertLockHeld(cs_main);
    assert(pindex);
    assert(*pindex->phashBlock == block.GetHash());
    int64_t nTimeStart = GetTimeMicros();

    // Check it again in case a previous version let a bad block in
    // NOTE: We don't currently (re-)invoke ContextualCheckBlock() or
    // ContextualCheckBlockHeader() here. This means that if we add a new
    // consensus rule that is enforced in one of those two functions, then we
    // may have let in a block that violates the rule prior to updating the
    // software, and we would NOT be enforcing the rule here. Fully solving
    // upgrade from one software version to the next after a consensus rule
    // change is potentially tricky and issue-specific (see RewindBlockIndex()
    // for one general approach that was used for BIP 141 deployment).
    // Also, currently the rule against blocks more than 2 hours in the future
    // is enforced in ContextualCheckBlockHeader(); we wouldn't want to
    // re-enforce that rule here (at least until we make it impossible for
    // GetAdjustedTime() to go backward).
    if (!CheckBlock(block, state, chainparams.GetConsensus(), !fJustCheck, !fJustCheck)) {
        if (state.CorruptionPossible()) {
            // We don't write down blocks to disk if they may have been
            // corrupted, so this should be impossible unless we're having hardware
            // problems.
            return AbortNode(state, "Corrupt block found indicating potential hardware failure; shutting down");
        }
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));
    }

    if (block.IsProofOfStake())
    {
        pindex->bnStakeModifier = ComputeStakeModifierV2(pindex->pprev, pindex->prevoutStake.hash);
        setDirtyBlockIndex.insert(pindex);

        uint256 hashProof, targetProofOfStake;
        if (!CheckProofOfStake(pindex->pprev, *block.vtx[0], block.nTime, block.nBits, hashProof, targetProofOfStake))
            return state.DoS(100, error("%s: Check proof of stake failed.", __func__), REJECT_INVALID, "bad-proof-of-stake");
    };

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == nullptr ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    uint256 blockHash = block.GetHash();
    bool fIsGenesisBlock = blockHash == chainparams.GetConsensus().hashGenesisBlock;
    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (!fParticlMode  // genesis coinbase is spendable when in Particl mode
        && fIsGenesisBlock) {
        if (!fJustCheck)
            view.SetBestBlock(pindex->GetBlockHash(), pindex->nHeight);
        return true;
    }

    nBlocksTotal++;

    bool fScriptChecks = true;
    if (!hashAssumeValid.IsNull()) {
        // We've been configured with the hash of a block which has been externally verified to have a valid history.
        // A suitable default value is included with the software and updated from time to time.  Because validity
        //  relative to a piece of software is an objective fact these defaults can be easily reviewed.
        // This setting doesn't force the selection of any particular chain but makes validating some faster by
        //  effectively caching the result of part of the verification.
        BlockMap::const_iterator  it = mapBlockIndex.find(hashAssumeValid);
        if (it != mapBlockIndex.end()) {
            if (it->second->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->nChainWork >= nMinimumChainWork) {
                // This block is a member of the assumed verified chain and an ancestor of the best header.
                // The equivalent time check discourages hash power from extorting the network via DOS attack
                //  into accepting an invalid block through telling users they must manually set assumevalid.
                //  Requiring a software change or burying the invalid block, regardless of the setting, makes
                //  it hard to hide the implication of the demand.  This also avoids having release candidates
                //  that are hardly doing any signature verification at all in testing without having to
                //  artificially set the default assumed verified block further back.
                // The test against nMinimumChainWork prevents the skipping when denied access to any chain at
                //  least as good as the expected chain.
                fScriptChecks = (GetBlockProofEquivalentTime(*pindexBestHeader, *pindex, *pindexBestHeader, chainparams.GetConsensus()) <= 60 * 60 * 24 * 7 * 2);
            }
        }
    }

    int64_t nTime1 = GetTimeMicros(); nTimeCheck += nTime1 - nTimeStart;
    LogPrint(BCLog::BENCH, "    - Sanity checks: %.2fms [%.2fs (%.2fms/blk)]\n", MILLI * (nTime1 - nTimeStart), nTimeCheck * MICRO, nTimeCheck * MILLI / nBlocksTotal);

    bool fEnforceBIP30 = true;
    if (!fParticlMode)
    {
        // Do not allow blocks that contain transactions which 'overwrite' older transactions,
        // unless those are already completely spent.
        // If such overwrites are allowed, coinbases and transactions depending upon those
        // can be duplicated to remove the ability to spend the first instance -- even after
        // being sent to another address.
        // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
        // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
        // already refuses previously-known transaction ids entirely.
        // This rule was originally applied to all blocks with a timestamp after March 15, 2012, 0:00 UTC.
        // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
        // two in the chain that violate it. This prevents exploiting the issue against nodes during their
        // initial block download.
        bool fEnforceBIP30 = !((pindex->nHeight==91842 && pindex->GetBlockHash() == uint256S("0x00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec")) ||
                               (pindex->nHeight==91880 && pindex->GetBlockHash() == uint256S("0x00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721")));

        // Once BIP34 activated it was not possible to create new duplicate coinbases and thus other than starting
        // with the 2 existing duplicate coinbase pairs, not possible to create overwriting txs.  But by the
        // time BIP34 activated, in each of the existing pairs the duplicate coinbase had overwritten the first
        // before the first had been spent.  Since those coinbases are sufficiently buried it's no longer possible to create further
        // duplicate transactions descending from the known pairs either.
        // If we're on the known chain at height greater than where BIP34 activated, we can save the db accesses needed for the BIP30 check.

        // BIP34 requires that a block at height X (block X) has its coinbase
        // scriptSig start with a CScriptNum of X (indicated height X).  The above
        // logic of no longer requiring BIP30 once BIP34 activates is flawed in the
        // case that there is a block X before the BIP34 height of 227,931 which has
        // an indicated height Y where Y is greater than X.  The coinbase for block
        // X would also be a valid coinbase for block Y, which could be a BIP30
        // violation.  An exhaustive search of all mainnet coinbases before the
        // BIP34 height which have an indicated height greater than the block height
        // reveals many occurrences. The 3 lowest indicated heights found are
        // 209,921, 490,897, and 1,983,702 and thus coinbases for blocks at these 3
        // heights would be the first opportunity for BIP30 to be violated.

        // There is no potential to create a duplicate coinbase at block 209,921
        // because this is still before the BIP34 height and so explicit BIP30
        // checking is still active.

        // The final case is block 176,684 which has an indicated height of
        // 490,897. Unfortunately, this issue was not discovered until about 2 weeks
        // before block 490,897 so there was not much opportunity to address this
        // case other than to carefully analyze it and determine it would not be a
        // problem. Block 490,897 was, in fact, mined with a different coinbase than
        // block 176,684, but it is important to note that even if it hadn't been or
        // is remined on an alternate fork with a duplicate coinbase, we would still
        // not run into a BIP30 violation.  This is because the coinbase for 176,684
        // is spent in block 185,956 in transaction
        // d4f7fbbf92f4a3014a230b2dc70b8058d02eb36ac06b4a0736d9d60eaa9e8781.  This
        // spending transaction can't be duplicated because it also spends coinbase
        // 0328dd85c331237f18e781d692c92de57649529bd5edf1d01036daea32ffde29.  This
        // coinbase has an indicated height of over 4.2 billion, and wouldn't be
        // duplicatable until that height, and it's currently impossible to create a
        // chain that long. Nevertheless we may wish to consider a future soft fork
        // which retroactively prevents block 490,897 from creating a duplicate
        // coinbase. The two historical BIP30 violations often provide a confusing
        // edge case when manipulating the UTXO and it would be simpler not to have
        // another edge case to deal with.

        // testnet3 has no blocks before the BIP34 height with indicated heights
        // post BIP34 before approximately height 486,000,000 and presumably will
        // be reset before it reaches block 1,983,702 and starts doing unnecessary
        // BIP30 checking again.
        assert(pindex->pprev);
        CBlockIndex *pindexBIP34height = pindex->pprev->GetAncestor(chainparams.GetConsensus().BIP34Height);
        //Only continue to enforce if we're below BIP34 activation height or the block hash at that height doesn't correspond.
        fEnforceBIP30 = fEnforceBIP30 && (!pindexBIP34height || !(pindexBIP34height->GetBlockHash() == chainparams.GetConsensus().BIP34Hash));
    };

    // The search reveals a great many blocks which have an indicated height
    // greater than 1,983,702, so we simply remove the optimization to skip
    // BIP30 checking for blocks at height 1,983,702 or higher.  Before we reach
    // that block in another 25 years or so, we should take advantage of a
    // future consensus change to do a new and improved version of BIP34 that
    // will actually prevent ever creating any duplicate coinbases in the
    // future.
    static constexpr int BIP34_IMPLIES_BIP30_LIMIT = 1983702;

    // TODO: Remove BIP30 checking from block height 1,983,702 on, once we have a
    // consensus change that ensures coinbases at those heights can not
    // duplicate earlier coinbases.
    if (fEnforceBIP30 || pindex->nHeight >= BIP34_IMPLIES_BIP30_LIMIT) {
        for (const auto& tx : block.vtx) {
            for (size_t o = 0; o < tx->GetNumVOuts(); o++) {
                if (view.HaveCoin(COutPoint(tx->GetHash(), o))) {
                    return state.DoS(100, error("ConnectBlock(): tried to overwrite transaction"),
                                     REJECT_INVALID, "bad-txns-BIP30");
                }
            }
        }
    }

    int nLockTimeFlags = 0;
    if (fParticlMode || VersionBitsState(pindex->pprev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_CSV, versionbitscache) == ThresholdState::ACTIVE) {
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    // Get the script flags for this block
    unsigned int flags = GetBlockScriptFlags(pindex, chainparams.GetConsensus());

    int64_t nTime2 = GetTimeMicros(); nTimeForks += nTime2 - nTime1;
    LogPrint(BCLog::BENCH, "    - Fork checks: %.2fms [%.2fs (%.2fms/blk)]\n", MILLI * (nTime2 - nTime1), nTimeForks * MICRO, nTimeForks * MILLI / nBlocksTotal);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : nullptr);

    std::vector<int> prevheights;
    CAmount nFees = 0;
    int nInputs = 0;
    int64_t nSigOpsCost = 0;
    int64_t nAnonIn = 0;
    int64_t nStakeReward = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());

    blockundo.vtxundo.reserve(block.vtx.size() - (fParticlMode ? 0 : 1));

    std::vector<PrecomputedTransactionData> txdata;
    txdata.reserve(block.vtx.size()); // Required so that pointers to individual PrecomputedTransactionData don't get invalidated

    // NOTE: Be careful tracking coin created, block reward is based on nMoneySupply
    CAmount nMoneyCreated = 0;

    state.fEnforceSmsgFees = block.nTime >= chainparams.GetConsensus().nPaidSmsgTime;
    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        const CTransaction &tx = *(block.vtx[i]);
        const uint256 txhash = tx.GetHash();
        nInputs += tx.vin.size();

        if (!tx.IsCoinBase())
        {
            CAmount txfee = 0;
            if (!Consensus::CheckTxInputs(tx, state, view, pindex->nHeight, txfee)) {
                control.Wait();
                return error("%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
            }
            if (tx.IsCoinStake())
            {
                // Stake reward is passed back in txfee (nPlainValueOut - nPlainValueIn)
                nStakeReward += txfee;
                nMoneyCreated += nStakeReward;
            } else
            {
                nFees += txfee;
            };
            if (!MoneyRange(nFees)) {
                control.Wait();
                return state.DoS(100, error("%s: accumulated fee in the block out of range.", __func__),
                                 REJECT_INVALID, "bad-txns-accumulated-fee-outofrange");
            }

            // Check that transaction is BIP68 final
            // BIP68 lock checks (as opposed to nLockTime checks) must
            // be in ConnectBlock because they require the UTXO set

            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                if (tx.vin[j].IsAnonInput())
                    prevheights[j] = 0;
                else
                    prevheights[j] = view.AccessCoin(tx.vin[j].prevout).nHeight;
            }

            if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *pindex)) {
                control.Wait();
                return state.DoS(100, error("%s: contains a non-BIP68-final transaction", __func__),
                                 REJECT_INVALID, "bad-txns-nonfinal");
            }

            if (tx.IsParticlVersion()
                && (fAddressIndex || fSpentIndex))
            {
                // Update spent inputs for insight
                for (size_t j = 0; j < tx.vin.size(); j++)
                {
                    const CTxIn input = tx.vin[j];
                    if (input.IsAnonInput())
                    {
                        nAnonIn++;
                        continue;
                    };

                    const Coin &coin = view.AccessCoin(input.prevout);
                    const CScript *pScript = &coin.out.scriptPubKey;

                    CAmount nValue = coin.nType == OUTPUT_CT ? 0 : coin.out.nValue;
                    std::vector<uint8_t> hashBytes;
                    int scriptType = 0;

                    if (!ExtractIndexInfo(pScript, scriptType, hashBytes)
                        || scriptType == 0)
                        continue;

                    uint256 hashAddress;
                    if (scriptType > 0)
                        hashAddress = uint256(hashBytes.data(), hashBytes.size());

                    if (fAddressIndex && scriptType > 0)
                    {
                        // record spending activity
                        view.addressIndex.push_back(std::make_pair(CAddressIndexKey(scriptType, hashAddress, pindex->nHeight, i, txhash, j, true), nValue * -1));
                        // remove address from unspent index
                        view.addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(scriptType, hashAddress, input.prevout.hash, input.prevout.n), CAddressUnspentValue()));
                    };

                    if (fSpentIndex)
                    {
                        CAmount nValue = coin.nType == OUTPUT_CT ? -1 : coin.out.nValue;
                        // add the spent index to determine the txid and input that spent an output
                        // and to find the amount and address from an input
                        view.spentIndex.push_back(std::make_pair(CSpentIndexKey(input.prevout.hash, input.prevout.n), CSpentIndexValue(txhash, j, pindex->nHeight, nValue, scriptType, hashAddress)));
                    };
                };
            };
        };

        // GetTransactionSigOpCost counts 3 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        // * witness (when witness enabled in flags and excludes coinbase)
        nSigOpsCost += GetTransactionSigOpCost(tx, view, flags);
        if (nSigOpsCost > MAX_BLOCK_SIGOPS_COST) {
            control.Wait();
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");
        }
        txdata.emplace_back(tx);

        if (!tx.IsCoinBase())
        {
            std::vector<CScriptCheck> vChecks;
            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, fCacheResults, fCacheResults, txdata[i], nScriptCheckThreads ? &vChecks : nullptr)) {
                control.Wait();
                return error("ConnectBlock(): CheckInputs on %s failed with %s",
                    txhash.ToString(), FormatStateMessage(state));
            };

            control.Add(vChecks);

            blockundo.vtxundo.push_back(CTxUndo());
            UpdateCoins(tx, view, blockundo.vtxundo.back(), pindex->nHeight);
        } else
        {
            // tx is coinbase
            CTxUndo undoDummy;
            UpdateCoins(tx, view, undoDummy, pindex->nHeight);
            nMoneyCreated += tx.GetValueOut();
        };

        if (view.nLastRCTOutput == 0)
            view.nLastRCTOutput = pindex->pprev ? pindex->pprev->nAnonOutputs : 0;
        // Index rct outputs and keyimages
        if (state.fHasAnonOutput || state.fHasAnonInput)
        {

            COutPoint op(txhash, 0);
            for (const auto &txin : tx.vin)
            {
                if (txin.IsAnonInput())
                {
                    uint32_t nInputs, nRingSize;
                    txin.GetAnonInfo(nInputs, nRingSize);
                    if (txin.scriptData.stack.size() != 1
                        || txin.scriptData.stack[0].size() != 33 * nInputs) {
                        control.Wait();
                        return error("%s: Bad scriptData stack, %s.", __func__, txhash.ToString());
                    };

                    const std::vector<uint8_t> &vKeyImages = txin.scriptData.stack[0];

                    for (size_t k = 0; k < nInputs; ++k)
                    {
                        const CCmpPubKey &ki = *((CCmpPubKey*)&vKeyImages[k*33]);

                        view.keyImages.push_back(std::make_pair(ki, txhash));
                    };
                };
            };

            for (unsigned int k = 0; k < tx.vpout.size(); k++)
            {
                if (!tx.vpout[k]->IsType(OUTPUT_RINGCT))
                    continue;

                CTxOutRingCT *txout = (CTxOutRingCT*)tx.vpout[k].get();

                int64_t nTestExists;
                if (!fVerifyingDB && pblocktree->ReadRCTOutputLink(txout->pk, nTestExists)) {
                    control.Wait();

                    if (nTestExists > pindex->pprev->nAnonOutputs)
                    {
                        // The anon index can diverge from the chain index if shutdown does not complete
                        LogPrintf("%s: Duplicate anon-output %s, index %d, above last index %d.\n", __func__, HexStr(txout->pk.begin(), txout->pk.end()), nTestExists, pindex->pprev->nAnonOutputs);
                        LogPrintf("Attempting to repair anon index.\n");
                        std::set<CCmpPubKey> setKi; // unused
                        RollBackRCTIndex(pindex->pprev->nAnonOutputs, nTestExists, setKi);
                        return false;
                    }

                    return error("%s: Duplicate anon-output (db) %s, index %d.", __func__, HexStr(txout->pk.begin(), txout->pk.end()), nTestExists);
                };
                if (!fVerifyingDB && view.ReadRCTOutputLink(txout->pk, nTestExists)) {
                    control.Wait();
                    return error("%s: Duplicate anon-output (view) %s, index %d.", __func__, HexStr(txout->pk.begin(), txout->pk.end()), nTestExists);
                };



                op.n = k;
                view.nLastRCTOutput++;
                CAnonOutput ao(txout->pk, txout->commitment, op, pindex->nHeight, 0);

                view.anonOutputLinks[txout->pk] = view.nLastRCTOutput;
                view.anonOutputs.push_back(std::make_pair(view.nLastRCTOutput, ao));
            };
        };

        if (fAddressIndex)
        {
            // Update outputs for insight
            for (unsigned int k = 0; k < tx.vpout.size(); k++)
            {
                const CTxOutBase *out = tx.vpout[k].get();

                if (!out->IsType(OUTPUT_STANDARD)
                    && !out->IsType(OUTPUT_CT))
                    continue;

                const CScript *pScript;
                std::vector<unsigned char> hashBytes;
                int scriptType = 0;
                CAmount nValue;
                if (!ExtractIndexInfo(out, scriptType, hashBytes, nValue, pScript)
                    || scriptType == 0)
                    continue;

                // record receiving activity
                view.addressIndex.push_back(std::make_pair(CAddressIndexKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), pindex->nHeight, i, txhash, k, false), nValue));
                // record unspent output
                view.addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(scriptType, uint256(hashBytes.data(), hashBytes.size()), txhash, k), CAddressUnspentValue(nValue, *pScript, pindex->nHeight)));
            };
        }; // if (fAddressIndex)

        vPos.push_back(std::make_pair(txhash, pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    };

    int64_t nTime3 = GetTimeMicros(); nTimeConnect += nTime3 - nTime2;
    LogPrint(BCLog::BENCH, "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs (%.2fms/blk)]\n", (unsigned)block.vtx.size(), MILLI * (nTime3 - nTime2), MILLI * (nTime3 - nTime2) / block.vtx.size(), nInputs <= 1 ? 0 : MILLI * (nTime3 - nTime2) / (nInputs-1), nTimeConnect * MICRO, nTimeConnect * MILLI / nBlocksTotal);

    if (!control.Wait())
        return state.DoS(100, error("%s: CheckQueue failed", __func__), REJECT_INVALID, "block-validation-failed");

    if (fParticlMode)
    {
        if (block.IsProofOfStake()) // only the genesis block isn't proof of stake
        {
            CTransactionRef txCoinstake = block.vtx[0];
            const DevFundSettings *pDevFundSettings = Params().GetDevFundSettings(block.nTime);

            CAmount nCalculatedStakeReward = Params().GetProofOfStakeReward(pindex->pprev, nFees);

            if (!pDevFundSettings || pDevFundSettings->nMinDevStakePercent <= 0)
            {
                if (nStakeReward < 0 || nStakeReward > nCalculatedStakeReward)
                    return state.DoS(100, error("ConnectBlock() : coinstake pays too much(actual=%d vs calculated=%d)", nStakeReward, nCalculatedStakeReward), REJECT_INVALID, "bad-cs-amount");
            } else
            {
                assert(pDevFundSettings->nMinDevStakePercent <= 100);

                CAmount nMinDevPart = (nCalculatedStakeReward * pDevFundSettings->nMinDevStakePercent) / 100;
                CAmount nMaxHolderPart = nCalculatedStakeReward - nMinDevPart;
                if (nMinDevPart < 0 || nMaxHolderPart < 0)
                    return state.DoS(100, error("%s: bad coinstake split amount (foundation=%d vs reward=%d)", __func__, nMinDevPart, nMaxHolderPart), REJECT_INVALID, "bad-cs-amount");


                CAmount nDevBfwd = 0, nDevCfwdCheck = 0;
                if (pindex->pprev->nHeight > 0) // genesis block is pow
                {
                    CTransactionRef txPrevCoinstake;
                    if (!coinStakeCache.GetCoinStake(pindex->pprev->GetBlockHash(), txPrevCoinstake))
                        return state.DoS(100, error("%s: Failed to get previous coinstake.", __func__), REJECT_INVALID, "bad-cs-amount");

                    assert(txPrevCoinstake->IsCoinStake()); // Sanity check

                    if (!txPrevCoinstake->GetDevFundCfwd(nDevBfwd))
                        nDevBfwd = 0;
                };

                if (pindex->nHeight % pDevFundSettings->nDevOutputPeriod == 0)
                {
                    // Fund output must exist and match cfwd, cfwd data output must be unset
                    // nStakeReward must == nDevBfwd + nCalculatedStakeReward

                    if (nStakeReward != nDevBfwd + nCalculatedStakeReward)
                        return state.DoS(100, error("%s: bad stake-reward (actual=%d vs expected=%d)", __func__, nStakeReward, nDevBfwd + nCalculatedStakeReward), REJECT_INVALID, "bad-cs-amount");

                    CTxDestination dfDest = CBitcoinAddress(pDevFundSettings->sDevFundAddresses).Get();
                    if (dfDest.type() == typeid(CNoDestination))
                        return error("%s: Failed to get foundation fund destination: %s.", __func__, pDevFundSettings->sDevFundAddresses);
                    CScript devFundScriptPubKey = GetScriptForDestination(dfDest);

                    // output 1 must be to the dev fund
                    const CTxOutStandard *outputDF = txCoinstake->vpout[1]->GetStandardOutput();
                    if (!outputDF)
                        return state.DoS(100, error("%s: Bad foundation fund output.", __func__), REJECT_INVALID, "bad-cs");

                    if (outputDF->scriptPubKey != devFundScriptPubKey)
                        return state.DoS(100, error("%s: Bad foundation fund output script.", __func__), REJECT_INVALID, "bad-cs");

                    if (outputDF->nValue < nDevBfwd + nMinDevPart) // max value is clamped already
                        return state.DoS(100, error("%s: Bad foundation-reward (actual=%d vs minfundpart=%d)", __func__, nStakeReward, nDevBfwd + nMinDevPart), REJECT_INVALID, "bad-cs-fund-amount");


                    if (txCoinstake->GetDevFundCfwd(nDevCfwdCheck))
                        return state.DoS(100, error("%s: Coinstake foundation cfwd must be unset.", __func__), REJECT_INVALID, "bad-cs-cfwd");
                } else
                {
                    // Ensure cfwd data output is correct and nStakeReward is <= nHolderPart
                    // cfwd must == nDevBfwd + (nCalculatedStakeReward - nStakeReward) // allowing users to set a higher split

                    if (nStakeReward < 0 || nStakeReward > nMaxHolderPart)
                        return state.DoS(100, error("%s: Bad stake-reward (actual=%d vs maxholderpart=%d)", __func__, nStakeReward, nMaxHolderPart), REJECT_INVALID, "bad-cs-amount");

                    CAmount nDevCfwd = nDevBfwd + nCalculatedStakeReward - nStakeReward;
                    if (!txCoinstake->GetDevFundCfwd(nDevCfwdCheck)
                        || nDevCfwdCheck != nDevCfwd)
                        return state.DoS(100, error("%s: Coinstake foundation fund carried forward mismatch (actual=%d vs expected=%d)", __func__, nDevCfwdCheck, nDevCfwd), REJECT_INVALID, "bad-cs-cfwd");
                };

                coinStakeCache.InsertCoinStake(blockHash, txCoinstake);
            };
        } else
        {
            if (block.GetHash() != Params().GenesisBlock().GetHash())
                return state.DoS(100, error("ConnectBlock() : Found block that isn't coinstake or genesis."), REJECT_INVALID, "bad-cs");
        }
    } else
    {
        CAmount blockReward = nFees + GetBlockSubsidy(pindex->nHeight, chainparams.GetConsensus());
        if (block.vtx[0]->GetValueOut() > blockReward) // particl coins are imported as coinbase txns
            return state.DoS(100,
                             error("ConnectBlock(): coinbase pays too much (actual=%d vs limit=%d)",
                                   block.vtx[0]->GetValueOut(), blockReward),
                                   REJECT_INVALID, "bad-cb-amount");
    };

    int64_t nTime4 = GetTimeMicros(); nTimeVerify += nTime4 - nTime2;
    LogPrint(BCLog::BENCH, "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs (%.2fms/blk)]\n", nInputs - 1, MILLI * (nTime4 - nTime2), nInputs <= 1 ? 0 : MILLI * (nTime4 - nTime2) / (nInputs-1), nTimeVerify * MICRO, nTimeVerify * MILLI / nBlocksTotal);

    if (fJustCheck)
        return true;

    pindex->nMoneySupply = (pindex->pprev ? pindex->pprev->nMoneySupply : 0) + nMoneyCreated;
    pindex->nAnonOutputs = view.nLastRCTOutput;
    setDirtyBlockIndex.insert(pindex); // pindex has changed, must save to disk

    if (!fIsGenesisBlock
     && !WriteUndoDataForBlock(blockundo, state, pindex, chainparams))
        return false;

    if (!pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }


    if (fTimestampIndex)
    {
        unsigned int logicalTS = pindex->nTime;
        unsigned int prevLogicalTS = 0;

        // retrieve logical timestamp of the previous block
        if (pindex->pprev)
            if (!pblocktree->ReadTimestampBlockIndex(pindex->pprev->GetBlockHash(), prevLogicalTS))
                LogPrintf("%s: Failed to read previous block's logical timestamp\n", __func__);

        if (logicalTS <= prevLogicalTS)
        {
            logicalTS = prevLogicalTS + 1;
            LogPrintf("%s: Previous logical timestamp is newer Actual[%d] prevLogical[%d] Logical[%d]\n", __func__, pindex->nTime, prevLogicalTS, logicalTS);
        }

        if (!pblocktree->WriteTimestampIndex(CTimestampIndexKey(logicalTS, pindex->GetBlockHash())))
            return AbortNode(state, "Failed to write timestamp index");

        if (!pblocktree->WriteTimestampBlockIndex(CTimestampBlockIndexKey(pindex->GetBlockHash()), CTimestampBlockIndexValue(logicalTS)))
            return AbortNode(state, "Failed to write blockhash index");
    };

    assert(pindex->phashBlock);
    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash(), pindex->nHeight);

    int64_t nTime5 = GetTimeMicros(); nTimeIndex += nTime5 - nTime4;
    LogPrint(BCLog::BENCH, "    - Index writing: %.2fms [%.2fs (%.2fms/blk)]\n", MILLI * (nTime5 - nTime4), nTimeIndex * MICRO, nTimeIndex * MILLI / nBlocksTotal);

    int64_t nTime6 = GetTimeMicros(); nTimeCallbacks += nTime6 - nTime5;
    LogPrint(BCLog::BENCH, "    - Callbacks: %.2fms [%.2fs (%.2fms/blk)]\n", MILLI * (nTime6 - nTime5), nTimeCallbacks * MICRO, nTimeCallbacks * MILLI / nBlocksTotal);

    return true;
}

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with
 * if they're too large, if it's been a while since the last write,
 * or always and in all cases if we're in prune mode and are deleting files.
 *
 * If FlushStateMode::NONE is used, then FlushStateToDisk(...) won't do anything
 * besides checking if we need to prune.
 */
bool FlushStateToDisk(const CChainParams& chainparams, CValidationState &state, FlushStateMode mode, int nManualPruneHeight) {
    int64_t nMempoolUsage = mempool.DynamicMemoryUsage();
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    std::set<int> setFilesToPrune;
    bool full_flush_completed = false;
    try {
    {
        bool fFlushForPrune = false;
        bool fDoFullFlush = false;
        LOCK(cs_LastBlockFile);
        if (fPruneMode && (fCheckForPruning || nManualPruneHeight > 0) && !fReindex) {
            if (nManualPruneHeight > 0) {
                FindFilesToPruneManual(setFilesToPrune, nManualPruneHeight);
            } else {
                FindFilesToPrune(setFilesToPrune, chainparams.PruneAfterHeight());
                fCheckForPruning = false;
            }
            if (!setFilesToPrune.empty()) {
                fFlushForPrune = true;
                if (!fHavePruned) {
                    pblocktree->WriteFlag("prunedblockfiles", true);
                    fHavePruned = true;
                }
            }
        }
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.
        if (nLastWrite == 0) {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0) {
            nLastFlush = nNow;
        }
        int64_t nMempoolSizeMax = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
        int64_t cacheSize = pcoinsTip->DynamicMemoryUsage();
        int64_t nTotalSpace = nCoinCacheUsage + std::max<int64_t>(nMempoolSizeMax - nMempoolUsage, 0);
        // The cache is large and we're within 10% and 10 MiB of the limit, but we have time now (not in the middle of a block processing).
        bool fCacheLarge = mode == FlushStateMode::PERIODIC && cacheSize > std::max((9 * nTotalSpace) / 10, nTotalSpace - MAX_BLOCK_COINSDB_USAGE * 1024 * 1024);
        // The cache is over the limit, we have to write now.
        bool fCacheCritical = mode == FlushStateMode::IF_NEEDED && cacheSize > nTotalSpace;
        // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload after a crash.
        bool fPeriodicWrite = mode == FlushStateMode::PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
        bool fPeriodicFlush = mode == FlushStateMode::PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        fDoFullFlush = (mode == FlushStateMode::ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush || fFlushForPrune;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite) {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(0, true))
                return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                for (std::set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                    vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex*> vBlocks;
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }
                if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                    return AbortNode(state, "Failed to write to block index database");
                }
            }
            // Finally remove any pruned files
            if (fFlushForPrune)
                UnlinkPrunedFiles(setFilesToPrune);
            nLastWrite = nNow;
        }
        // Flush best chain related state. This can only be done if the blocks / block index write was also done.
        if (fDoFullFlush && !pcoinsTip->GetBestBlock().IsNull()) {
            // Typical Coin structures on disk are around 48 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(48 * 2 * 2 * pcoinsTip->GetCacheSize()))
                return state.Error("out of disk space");
            // Flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush())
                return AbortNode(state, "Failed to write to coin database");
            nLastFlush = nNow;
            full_flush_completed = true;
        }
    }
    if (full_flush_completed) {
        // Update best block in wallet (so we can detect restored wallets).
        GetMainSignals().ChainStateFlushed(chainActive.GetLocator());
    }
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    const CChainParams& chainparams = Params();
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::ALWAYS)) {
        LogPrintf("%s: failed to flush state (%s)\n", __func__, FormatStateMessage(state));
    }
}

void PruneAndFlush() {
    CValidationState state;
    fCheckForPruning = true;
    const CChainParams& chainparams = Params();
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::NONE)) {
        LogPrintf("%s: failed to flush state (%s)\n", __func__, FormatStateMessage(state));
    }
}

static void DoWarning(const std::string& strWarning)
{
    static bool fWarned = false;
    SetMiscWarning(strWarning);
    if (!fWarned) {
        AlertNotify(strWarning);
        fWarned = true;
    }
}

bool FlushView(CCoinsViewCache *view, CValidationState& state, bool fDisconnecting)
{
    if (!view->Flush())
        return false;

    if (fAddressIndex)
    {
        if (fDisconnecting)
        {
            if (!pblocktree->EraseAddressIndex(view->addressIndex))
                return AbortNode(state, "Failed to delete address index");
        } else
        {
            if (!pblocktree->WriteAddressIndex(view->addressIndex))
                return AbortNode(state, "Failed to write address index");
        };

        if (!pblocktree->UpdateAddressUnspentIndex(view->addressUnspentIndex))
            return AbortNode(state, "Failed to write address unspent index");
    };

    if (fSpentIndex)
    {
        if (!pblocktree->UpdateSpentIndex(view->spentIndex))
            return AbortNode(state, "Failed to write transaction index");
    };

    view->addressIndex.clear();
    view->addressUnspentIndex.clear();
    view->spentIndex.clear();

    if (fDisconnecting)
    {
        for (auto &it : view->keyImages)
            if (!pblocktree->EraseRCTKeyImage(it.first))
                return error("%s: EraseRCTKeyImage failed, txn %s.", __func__, it.second.ToString());

        if (view->anonOutputLinks.size() > 0)
        {
            for (auto &it : view->anonOutputLinks)
            {
                if (!pblocktree->EraseRCTOutput(it.second))
                    return error("%s: EraseRCTOutput failed.", __func__);

                if (!pblocktree->EraseRCTOutputLink(it.first))
                    return error("%s: EraseRCTOutput failed.", __func__);
            };

        };
    } else
    {
        CDBBatch batch(*pblocktree);

        for (auto &it : view->keyImages)
            batch.Write(std::make_pair(DB_RCTKEYIMAGE, it.first), it.second);

        for (auto &it : view->anonOutputs)
            batch.Write(std::make_pair(DB_RCTOUTPUT, it.first), it.second);

        for (auto &it : view->anonOutputLinks)
            batch.Write(std::make_pair(DB_RCTOUTPUT_LINK, it.first), it.second);

        if (!pblocktree->WriteBatch(batch))
            return error("%s: Write RCT outputs failed.", __func__);
    };

    view->nLastRCTOutput = 0;
    view->anonOutputs.clear();
    view->anonOutputLinks.clear();
    view->keyImages.clear();

    return true;
};

/** Check warning conditions and do some notifications on new chain tip set. */
void UpdateTip(const CBlockIndex *pindexNew, const CChainParams& chainParams) {
    // New best block
    mempool.AddTransactionsUpdated(1);

    {
        WaitableLock lock(g_best_block_mutex);
        g_best_block = pindexNew->GetBlockHash();
        g_best_block_cv.notify_all();
    }

    std::vector<std::string> warningMessages;
    if (!IsInitialBlockDownload())
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexNew;
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(pindex, chainParams.GetConsensus(), warningcache[bit]);
            if (state == ThresholdState::ACTIVE || state == ThresholdState::LOCKED_IN) {
                const std::string strWarning = strprintf(_("Warning: unknown new rules activated (versionbit %i)"), bit);
                if (state == ThresholdState::ACTIVE) {
                    DoWarning(strWarning);
                } else {
                    warningMessages.push_back(strWarning);
                }
            }
        }
        // Check the version of the last 100 blocks to see if we need to upgrade:
        for (int i = 0; i < 100 && pindex != nullptr; i++)
        {
            if (fParticlMode)
            {
                if (pindex->nVersion > PARTICL_BLOCK_VERSION)
                    ++nUpgraded;
            } else
            {
                int32_t nExpectedVersion = ComputeBlockVersion(pindex->pprev, chainParams.GetConsensus());
                if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION && (pindex->nVersion & ~nExpectedVersion) != 0)
                    ++nUpgraded;
            }
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            warningMessages.push_back(strprintf(_("%d of last 100 blocks have unexpected version"), nUpgraded));
        if (nUpgraded > 100/2)
        {
            std::string strWarning = _("Warning: Unknown block versions being mined! It's possible unknown rules are in effect");
            // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            DoWarning(strWarning);
        }
    }
    LogPrintf("%s: new best=%s height=%d version=0x%08x log2_work=%.8g tx=%lu date='%s' progress=%f cache=%.1fMiB(%utxo)", __func__, /* Continued */
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight, pindexNew->nVersion,
      log(pindexNew->nChainWork.getdouble())/log(2.0), (unsigned long)pindexNew->nChainTx,
      FormatISO8601DateTime(pindexNew->GetBlockTime()),
      GuessVerificationProgress(chainParams.TxData(), pindexNew), pcoinsTip->DynamicMemoryUsage() * (1.0 / (1<<20)), pcoinsTip->GetCacheSize());
    if (!warningMessages.empty())
        LogPrintf(" warning='%s'", boost::algorithm::join(warningMessages, ", ")); /* Continued */
    LogPrintf("\n");

}

/** Disconnect chainActive's tip.
  * After calling, the mempool will be in an inconsistent state, with
  * transactions from disconnected blocks being added to disconnectpool.  You
  * should make the mempool consistent again by calling UpdateMempoolForReorg.
  * with cs_main held.
  *
  * If disconnectpool is nullptr, then no disconnected transactions are added to
  * disconnectpool (note that the caller is responsible for mempool consistency
  * in any case).
  */
bool CChainState::DisconnectTip(CValidationState& state, const CChainParams& chainparams, DisconnectedBlockTransactions *disconnectpool)
{
    CBlockIndex *pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    CBlock& block = *pblock;
    if (!ReadBlockFromDisk(block, pindexDelete, chainparams.GetConsensus()))
        return AbortNode(state, "Failed to read block");
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip.get());
        assert(view.GetBestBlock() == pindexDelete->GetBlockHash());
        if (DisconnectBlock(block, pindexDelete, view) != DISCONNECT_OK)
            return error("DisconnectTip(): DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        bool flushed = FlushView(&view, state, true);
        assert(flushed);
    }
    LogPrint(BCLog::BENCH, "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * MILLI);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::IF_NEEDED))
        return false;

    if (disconnectpool) {
        // Save transactions to re-add to mempool at end of reorg
        for (auto it = block.vtx.rbegin(); it != block.vtx.rend(); ++it) {
            disconnectpool->addTransaction(*it);
        }
        while (disconnectpool->DynamicMemoryUsage() > MAX_DISCONNECTED_TX_POOL_SIZE * 1000) {
            // Drop the earliest entry, and remove its children from the mempool.
            auto it = disconnectpool->queuedTx.get<insertion_order>().begin();
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
            disconnectpool->removeEntry(it);
        }
    }

    chainActive.SetTip(pindexDelete->pprev);

    UpdateTip(pindexDelete->pprev, chainparams);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    GetMainSignals().BlockDisconnected(pblock);
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

struct PerBlockConnectTrace {
    CBlockIndex* pindex = nullptr;
    std::shared_ptr<const CBlock> pblock;
    std::shared_ptr<std::vector<CTransactionRef>> conflictedTxs;
    PerBlockConnectTrace() : conflictedTxs(std::make_shared<std::vector<CTransactionRef>>()) {}
};
/**
 * Used to track blocks whose transactions were applied to the UTXO state as a
 * part of a single ActivateBestChainStep call.
 *
 * This class also tracks transactions that are removed from the mempool as
 * conflicts (per block) and can be used to pass all those transactions
 * through SyncTransaction.
 *
 * This class assumes (and asserts) that the conflicted transactions for a given
 * block are added via mempool callbacks prior to the BlockConnected() associated
 * with those transactions. If any transactions are marked conflicted, it is
 * assumed that an associated block will always be added.
 *
 * This class is single-use, once you call GetBlocksConnected() you have to throw
 * it away and make a new one.
 */
class ConnectTrace {
private:
    std::vector<PerBlockConnectTrace> blocksConnected;
    CTxMemPool &pool;

public:
    explicit ConnectTrace(CTxMemPool &_pool) : blocksConnected(1), pool(_pool) {
        pool.NotifyEntryRemoved.connect(boost::bind(&ConnectTrace::NotifyEntryRemoved, this, _1, _2));
    }

    ~ConnectTrace() {
        pool.NotifyEntryRemoved.disconnect(boost::bind(&ConnectTrace::NotifyEntryRemoved, this, _1, _2));
    }

    void BlockConnected(CBlockIndex* pindex, std::shared_ptr<const CBlock> pblock) {
        assert(!blocksConnected.back().pindex);
        assert(pindex);
        assert(pblock);
        blocksConnected.back().pindex = pindex;
        blocksConnected.back().pblock = std::move(pblock);
        blocksConnected.emplace_back();
    }

    std::vector<PerBlockConnectTrace>& GetBlocksConnected() {
        // We always keep one extra block at the end of our list because
        // blocks are added after all the conflicted transactions have
        // been filled in. Thus, the last entry should always be an empty
        // one waiting for the transactions from the next block. We pop
        // the last entry here to make sure the list we return is sane.
        assert(!blocksConnected.back().pindex);
        assert(blocksConnected.back().conflictedTxs->empty());
        blocksConnected.pop_back();
        return blocksConnected;
    }

    void NotifyEntryRemoved(CTransactionRef txRemoved, MemPoolRemovalReason reason) {
        assert(!blocksConnected.back().pindex);
        if (reason == MemPoolRemovalReason::CONFLICT) {
            blocksConnected.back().conflictedTxs->emplace_back(std::move(txRemoved));
        }
    }
};

/**
 * Connect a new block to chainActive. pblock is either nullptr or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is added to connectTrace if connection succeeds.
 */
bool CChainState::ConnectTip(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexNew, const std::shared_ptr<const CBlock>& pblock, ConnectTrace& connectTrace, DisconnectedBlockTransactions &disconnectpool)
{
    assert(pindexNew->pprev == chainActive.Tip());
    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    std::shared_ptr<const CBlock> pthisBlock;
    if (!pblock) {
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        if (!ReadBlockFromDisk(*pblockNew, pindexNew, chainparams.GetConsensus()))
            return AbortNode(state, "Failed to read block");
        pthisBlock = pblockNew;
    } else {
        pthisBlock = pblock;
    }
    const CBlock& blockConnecting = *pthisBlock;
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros(); nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;

    LogPrint(BCLog::BENCH, "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * MILLI, nTimeReadFromDisk * MICRO);
    setConnectKi.clear();
    {

        CCoinsViewCache view(pcoinsTip.get());
        bool rv = ConnectBlock(blockConnecting, state, pindexNew, view, chainparams);
        if (pindexNew->nFlags & BLOCK_FAILED_DUPLICATE_STAKE)
            state.nFlags |= BLOCK_FAILED_DUPLICATE_STAKE;
        GetMainSignals().BlockChecked(blockConnecting, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, blockConnecting, state);

            return error("ConnectTip(): ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        nTime3 = GetTimeMicros(); nTimeConnectTotal += nTime3 - nTime2;
        LogPrint(BCLog::BENCH, "  - Connect total: %.2fms [%.2fs (%.2fms/blk)]\n", (nTime3 - nTime2) * MILLI, nTimeConnectTotal * MICRO, nTimeConnectTotal * MILLI / nBlocksTotal);
        bool flushed = FlushView(&view, state, false);
        assert(flushed);
    }
    int64_t nTime4 = GetTimeMicros(); nTimeFlush += nTime4 - nTime3;
    LogPrint(BCLog::BENCH, "  - Flush: %.2fms [%.2fs (%.2fms/blk)]\n", (nTime4 - nTime3) * MILLI, nTimeFlush * MICRO, nTimeFlush * MILLI / nBlocksTotal);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::IF_NEEDED))
    {
        //RollBackRCTIndex(nLastValidRCTOutput, setConnectKi);
        return false;
    }
    int64_t nTime5 = GetTimeMicros(); nTimeChainState += nTime5 - nTime4;
    LogPrint(BCLog::BENCH, "  - Writing chainstate: %.2fms [%.2fs (%.2fms/blk)]\n", (nTime5 - nTime4) * MILLI, nTimeChainState * MICRO, nTimeChainState * MILLI / nBlocksTotal);
    // Remove conflicting transactions from the mempool.;
    mempool.removeForBlock(blockConnecting.vtx, pindexNew->nHeight);
    disconnectpool.removeForBlock(blockConnecting.vtx);
    // Update chainActive & related variables.
    chainActive.SetTip(pindexNew);
    UpdateTip(pindexNew, chainparams);

    int64_t nTime6 = GetTimeMicros(); nTimePostConnect += nTime6 - nTime5; nTimeTotal += nTime6 - nTime1;
    LogPrint(BCLog::BENCH, "  - Connect postprocess: %.2fms [%.2fs (%.2fms/blk)]\n", (nTime6 - nTime5) * MILLI, nTimePostConnect * MICRO, nTimePostConnect * MILLI / nBlocksTotal);
    LogPrint(BCLog::BENCH, "- Connect block: %.2fms [%.2fs (%.2fms/blk)]\n", (nTime6 - nTime1) * MILLI, nTimeTotal * MICRO, nTimeTotal * MILLI / nBlocksTotal);

    connectTrace.BlockConnected(pindexNew, std::move(pthisBlock));
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
CBlockIndex* CChainState::FindMostWorkChain() {
    do {
        CBlockIndex *pindexNew = nullptr;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return nullptr;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex *pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);

            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == nullptr || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex *pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {

                        if (pindexTest->nFlags & BLOCK_FAILED_DUPLICATE_STAKE)
                            pindexFailed->nFlags |= BLOCK_FAILED_DUPLICATE_STAKE;

                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while(true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
void CChainState::PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either nullptr or a pointer to a CBlock corresponding to pindexMostWork.
 */
bool CChainState::ActivateBestChainStep(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexMostWork, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, ConnectTrace& connectTrace)
{
    AssertLockHeld(cs_main);
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state, chainparams, &disconnectpool)) {
            // This is likely a fatal error, but keep the mempool consistent,
            // just in case. Only remove from the mempool in this case.
            UpdateMempoolForReorg(disconnectpool, false);
            return false;
        }
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex *pindexConnect : reverse_iterate(vpindexToConnect)) {
            if (!ConnectTip(state, chainparams, pindexConnect, pindexConnect == pindexMostWork ? pblock : std::shared_ptr<const CBlock>(), connectTrace, disconnectpool)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible()) {
                        InvalidChainFound(vpindexToConnect.front());
                    }
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    // Make the mempool consistent with the current tip, just in case
                    // any observers try to use it before shutdown.
                    UpdateMempoolForReorg(disconnectpool, false);
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        // If any blocks were disconnected, disconnectpool may be non empty.  Add
        // any disconnected transactions back to the mempool.
        UpdateMempoolForReorg(disconnectpool, true);
    }
    mempool.check(pcoinsTip.get());


    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

static void NotifyHeaderTip() {
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex* pindexHeaderOld = nullptr;
    CBlockIndex* pindexHeader = nullptr;
    {
        LOCK(cs_main);
        pindexHeader = pindexBestHeader;

        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            pindexHeaderOld = pindexHeader;
        }
    }
    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either nullptr or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 *
 * ActivateBestChain is split into steps (see ActivateBestChainStep) so that
 * we avoid holding cs_main for an extended period of time; the length of this
 * call may be quite long during reindexing or a substantial reorg.
 */
bool CChainState::ActivateBestChain(CValidationState &state, const CChainParams& chainparams, std::shared_ptr<const CBlock> pblock) {
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    AssertLockNotHeld(cs_main);

    CBlockIndex *pindexMostWork = nullptr;
    CBlockIndex *pindexNewTip = nullptr;
    int nStopAtHeight = gArgs.GetArg("-stopatheight", DEFAULT_STOPATHEIGHT);
    do {
        boost::this_thread::interruption_point();

        if (GetMainSignals().CallbacksPending() > 10) {
            // Block until the validation queue drains. This should largely
            // never happen in normal operation, however may happen during
            // reindex, causing memory blowup if we run too far ahead.
            SyncWithValidationInterfaceQueue();
        }

        const CBlockIndex *pindexFork;
        bool fInitialDownload;
        {
            LOCK(cs_main);
            ConnectTrace connectTrace(mempool); // Destructed before cs_main is unlocked

            CBlockIndex *pindexOldTip = chainActive.Tip();
            if (pindexMostWork == nullptr) {
                pindexMostWork = FindMostWorkChain();
            }

            // Whether we have anything to do at all.
            if (pindexMostWork == nullptr || pindexMostWork == chainActive.Tip())
                return true;
            bool fInvalidFound = false;
            std::shared_ptr<const CBlock> nullBlockPtr;
            if (!ActivateBestChainStep(state, chainparams, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : nullBlockPtr, fInvalidFound, connectTrace))
                return false;
            if (fInvalidFound) {
                // Wipe cache, we may need another branch now.
                pindexMostWork = nullptr;
            }
            pindexNewTip = chainActive.Tip();
            pindexFork = chainActive.FindFork(pindexOldTip);
            fInitialDownload = IsInitialBlockDownload();

            for (const PerBlockConnectTrace& trace : connectTrace.GetBlocksConnected()) {
                assert(trace.pblock && trace.pindex);
                GetMainSignals().BlockConnected(trace.pblock, trace.pindex, trace.conflictedTxs);
            }

            // Notify external listeners about the new tip.
            // Enqueue while holding cs_main to ensure that UpdatedBlockTip is called in the order in which blocks are connected
            GetMainSignals().UpdatedBlockTip(pindexNewTip, pindexFork, fInitialDownload);

            // Always notify the UI if a new block tip was connected
            if (pindexFork != pindexNewTip) {
                uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
            }
        }
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        if (nStopAtHeight && pindexNewTip && pindexNewTip->nHeight >= nStopAtHeight) StartShutdown();

        // We check shutdown only after giving ActivateBestChainStep a chance to run once so that we
        // never shutdown before connecting the genesis block during LoadChainTip(). Previously this
        // caused an assert() failure during shutdown in such cases as the UTXO DB flushing checks
        // that the best block hash is non-null.
        if (ShutdownRequested())
            break;
    } while (pindexNewTip != pindexMostWork);
    CheckBlockIndex(chainparams.GetConsensus());


    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::PERIODIC)) {
        return false;
    }
    return true;
}
bool ActivateBestChain(CValidationState &state, const CChainParams& chainparams, std::shared_ptr<const CBlock> pblock) {
    return g_chainstate.ActivateBestChain(state, chainparams, std::move(pblock));
}

bool CChainState::PreciousBlock(CValidationState& state, const CChainParams& params, CBlockIndex *pindex)
{
    {
        LOCK(cs_main);
        if (pindex->nChainWork < chainActive.Tip()->nChainWork) {
            // Nothing to do, this block is not at the tip.
            return true;
        }
        if (chainActive.Tip()->nChainWork > nLastPreciousChainwork) {
            // The chain has been extended since the last call, reset the counter.
            nBlockReverseSequenceId = -1;
        }
        nLastPreciousChainwork = chainActive.Tip()->nChainWork;
        setBlockIndexCandidates.erase(pindex);
        pindex->nSequenceId = nBlockReverseSequenceId;
        if (nBlockReverseSequenceId > std::numeric_limits<int32_t>::min()) {
            // We can't keep reducing the counter if somebody really wants to
            // call preciousblock 2**31-1 times on the same set of tips...
            nBlockReverseSequenceId--;
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && pindex->nChainTx) {
            setBlockIndexCandidates.insert(pindex);
            PruneBlockIndexCandidates();
        }
    }

    return ActivateBestChain(state, params, std::shared_ptr<const CBlock>());
}
bool PreciousBlock(CValidationState& state, const CChainParams& params, CBlockIndex *pindex) {
    return g_chainstate.PreciousBlock(state, params, pindex);
}

bool CChainState::InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex *pindex)
{
    AssertLockHeld(cs_main);

    // We first disconnect backwards and then mark the blocks as invalid.
    // This prevents a case where pruned nodes may fail to invalidateblock
    // and be left unable to start as they have no tip candidates (as there
    // are no blocks that meet the "have data and are not invalid per
    // nStatus" criteria for inclusion in setBlockIndexCandidates).

    bool pindex_was_in_chain = false;
    CBlockIndex *invalid_walk_tip = chainActive.Tip();

    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Contains(pindex)) {
        pindex_was_in_chain = true;
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, chainparams, &disconnectpool)) {
            // It's probably hopeless to try to make the mempool consistent
            // here if DisconnectTip failed, but we can try.
            UpdateMempoolForReorg(disconnectpool, false);
            return false;
        }
    }

    // Now mark the blocks we just disconnected as descendants invalid
    // (note this may not be all descendants).
    while (pindex_was_in_chain && invalid_walk_tip != pindex) {
        invalid_walk_tip->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(invalid_walk_tip);
        setBlockIndexCandidates.erase(invalid_walk_tip);
        invalid_walk_tip = invalid_walk_tip->pprev;
    }

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);
    m_failed_blocks.insert(pindex);

    // DisconnectTip will add transactions to disconnectpool; try to add these
    // back to the mempool.
    UpdateMempoolForReorg(disconnectpool, true);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);

    // Only notify about a new block tip if the active chain was modified.
    if (pindex_was_in_chain) {
        uiInterface.NotifyBlockTip(IsInitialBlockDownload(), pindex->pprev);
    }
    return true;
}
bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex *pindex) {
    return g_chainstate.InvalidateBlock(state, chainparams, pindex);
}

bool CChainState::ResetBlockFailureFlags(CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = nullptr;
            }
            m_failed_blocks.erase(it->second);
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != nullptr) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}
bool ResetBlockFailureFlags(CBlockIndex *pindex) {
    return g_chainstate.ResetBlockFailureFlags(pindex);
}

CBlockIndex* CChainState::AddToBlockIndex(const CBlockHeader& block)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);

    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == nullptr || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool CChainState::ReceivedBlockTransactions(const CBlock &block, CValidationState& state, CBlockIndex *pindexNew, const CDiskBlockPos& pos, const Consensus::Params& consensusParams)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    if (IsWitnessEnabled(pindexNew->pprev, consensusParams)) {
        pindexNew->nStatus |= BLOCK_OPT_WITNESS;
    }
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);
    if (pindexNew->pprev == nullptr || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == nullptr || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

static bool FindBlockPos(CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile, vinfoBlockFile[nLastBlockFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (fPruneMode)
                fCheckForPruning = true;
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos, true)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

static bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode)
            fCheckForPruning = true;
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos, true)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error("out of disk space");
    }

    return true;
}

static bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW = true)
{
    // Check timestamp
    if (fParticlMode
        && !block.hashPrevBlock.IsNull() // allow genesis block to be created in the future
        && block.GetBlockTime() > FutureDrift(GetAdjustedTime()))
        return state.DoS(50, false, REJECT_INVALID, "block-timestamp", false, "block timestamp too far in the future");

    // Check proof of work matches claimed amount
    if (!fParticlMode
        && fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
        return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed");

    return true;
}


bool CheckBlockSignature(const CBlock &block)
{
    if (!block.IsProofOfStake())
        return block.vchBlockSig.empty();
    if (block.vchBlockSig.empty())
        return false;
    if (block.vtx[0]->vin.size() < 1)
        return false;

    const auto &txin = block.vtx[0]->vin[0];
    if (txin.scriptWitness.stack.size() != 2)
        return false;

    if (txin.scriptWitness.stack[1].size() != 33)
        return false;

    CPubKey pubKey(txin.scriptWitness.stack[1]);
    return pubKey.Verify(block.GetHash(), block.vchBlockSig);
};

bool AddToMapStakeSeen(const COutPoint &kernel, const uint256 &blockHash)
{
    // Overwrites existing values

    std::pair<std::map<COutPoint, uint256>::iterator,bool> ret;
    ret = mapStakeSeen.insert(std::pair<COutPoint, uint256>(kernel, blockHash));
    if (ret.second == false) // existing element
    {
        ret.first->second = blockHash;
    } else
    {
        listStakeSeen.push_back(kernel);
    };
    //mapStakeSeen[kernel] = blockHash;
    //listStakeSeen.push_back(kernel);
    return true;
};

bool CheckStakeUnused(const COutPoint &kernel)
{
    std::map<COutPoint, uint256>::const_iterator mi = mapStakeSeen.find(kernel);
    if (mi != mapStakeSeen.end())
        return false;
    return true;
}

bool CheckStakeUnique(const CBlock &block, bool fUpdate)
{
    uint256 blockHash = block.GetHash();
    const COutPoint &kernel = block.vtx[0]->vin[0].prevout;

    std::map<COutPoint, uint256>::const_iterator mi = mapStakeSeen.find(kernel);
    if (mi != mapStakeSeen.end())
    {
        if (mi->second == blockHash)
            return true;

        return error("%s: Stake kernel for %s first seen on %s.", __func__, blockHash.ToString(), mi->second.ToString());
    };

    if (!fUpdate)
        return true;



    if (listStakeSeen.size() > MAX_STAKE_SEEN_SIZE)
    {
        const COutPoint &oldest = listStakeSeen.front();
        if (1 != mapStakeSeen.erase(oldest))
            LogPrintf("%s: Warning: mapStakeSeen did not erase %s %n\n", __func__, oldest.hash.ToString(), oldest.n);
        listStakeSeen.pop_front();
    };

    return AddToMapStakeSeen(kernel, blockHash);
};

bool CheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW, bool fCheckMerkleRoot)
{
    // These are checks that are independent of context.

    if (block.fChecked)
        return true;

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, consensusParams, fCheckPOW))
        return false;

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;

        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);

        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot", true, "hashMerkleRoot mismatch");

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate", true, "duplicate transaction");
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.
    // Note that witness malleability is checked in ContextualCheckBlock, so no
    // checks that use witness data may be performed here.

    // Size limits
    if (block.vtx.empty() || block.vtx.size() * WITNESS_SCALE_FACTOR > MAX_BLOCK_WEIGHT || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * WITNESS_SCALE_FACTOR > MAX_BLOCK_WEIGHT)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false, "size limits failed");

    if (fParticlMode)
    {
        if (!IsInitialBlockDownload()
            && block.vtx[0]->IsCoinStake()
            && !CheckStakeUnique(block))
        {
            //state.DoS(10, false, REJECT_INVALID, "bad-cs-duplicate", false, "duplicate coinstake");

            state.nFlags |= BLOCK_FAILED_DUPLICATE_STAKE;

            /*
            // TODO: ask peers which stake kernel they have
            if (chainActive.Tip()->nHeight < GetNumBlocksOfPeers() - 8) // peers have significantly longer chain, this node must've got the wrong stake 1st
            {
                LogPrint(BCLog::POS, "%s: Ignoring CheckStakeUnique for block %s, chain height behind peers.\n", __func__, block.GetHash().ToString());
                const COutPoint &kernel = block.vtx[0]->vin[0].prevout;
                mapStakeSeen[kernel] = block.GetHash();
            } else
                return state.DoS(20, false, REJECT_INVALID, "bad-cs-duplicate", false, "duplicate coinstake");
            */
        };

        // First transaction must be coinbase (genesis only) or coinstake
        // 2nd txn may be coinbase in early blocks: check further in ContextualCheckBlock
        if (!(block.vtx[0]->IsCoinBase() || block.vtx[0]->IsCoinStake())) // only genesis can be coinbase, check in ContextualCheckBlock
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false, "first tx is not coinbase");

        // 2nd txn may never be coinstake, remaining txns must not be coinbase/stake
        for (size_t i = 1; i < block.vtx.size(); i++)
            if ((i > 1 && block.vtx[i]->IsCoinBase()) || block.vtx[i]->IsCoinStake())
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "more than one coinbase or coinstake");

        if (!CheckBlockSignature(block))
            return state.DoS(100, false, REJECT_INVALID, "bad-block-signature", false, "bad block signature");
    } else
    {
        // First transaction must be coinbase, the rest must not be
        if (block.vtx.empty() || !block.vtx[0]->IsCoinBase())
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false, "first tx is not coinbase");

        // Remaining txns must not be coinbase/stake
        for (size_t i = 1; i < block.vtx.size(); i++)
            if (block.vtx[i]->IsCoinBase() || block.vtx[i]->IsCoinStake())
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "more than one coinbase");
    };

    // Check transactions
    for (const auto& tx : block.vtx)
    {
        //if (!CheckTransaction(*tx, state, false))
        if (!CheckTransaction(*tx, state)) // Check for duplicate inputs, TODO: UpdateCoins should return a bool, db/coinsview txn should be undone
            return state.Invalid(false, state.GetRejectCode(), state.GetRejectReason(),
                                 strprintf("Transaction check failed (tx hash %s) %s", tx->GetHash().ToString(), state.GetDebugMessage()));
    };

    unsigned int nSigOps = 0;
    for (const auto& tx : block.vtx)
    {
        nSigOps += GetLegacySigOpCount(*tx);
    }
    if (nSigOps * WITNESS_SCALE_FACTOR > MAX_BLOCK_SIGOPS_COST)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-sigops", false, "out-of-bounds SigOpCount");

    if (fCheckPOW && fCheckMerkleRoot)
        block.fChecked = true;

    return true;
}

bool IsWitnessEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    if (fParticlMode) return true;

    LOCK(cs_main);
    return (VersionBitsState(pindexPrev, params, Consensus::DEPLOYMENT_SEGWIT, versionbitscache) == ThresholdState::ACTIVE);
}

bool IsNullDummyEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    LOCK(cs_main);
    return (VersionBitsState(pindexPrev, params, Consensus::DEPLOYMENT_SEGWIT, versionbitscache) == ThresholdState::ACTIVE);
}

// Compute at which vout of the block's coinbase transaction the witness
// commitment occurs, or -1 if not found.
static int GetWitnessCommitmentIndex(const CBlock& block)
{
    int commitpos = -1;
    if (!block.vtx.empty()) {
        for (size_t o = 0; o < block.vtx[0]->vout.size(); o++) {
            if (block.vtx[0]->vout[o].scriptPubKey.size() >= 38 && block.vtx[0]->vout[o].scriptPubKey[0] == OP_RETURN && block.vtx[0]->vout[o].scriptPubKey[1] == 0x24 && block.vtx[0]->vout[o].scriptPubKey[2] == 0xaa && block.vtx[0]->vout[o].scriptPubKey[3] == 0x21 && block.vtx[0]->vout[o].scriptPubKey[4] == 0xa9 && block.vtx[0]->vout[o].scriptPubKey[5] == 0xed) {
                commitpos = o;
            }
        }
    }
    return commitpos;
}

void UpdateUncommittedBlockStructures(CBlock& block, const CBlockIndex* pindexPrev, const Consensus::Params& consensusParams)
{
    int commitpos = GetWitnessCommitmentIndex(block);
    static const std::vector<unsigned char> nonce(32, 0x00);
    if (commitpos != -1 && IsWitnessEnabled(pindexPrev, consensusParams) && !block.vtx[0]->HasWitness()) {
        CMutableTransaction tx(*block.vtx[0]);
        tx.vin[0].scriptWitness.stack.resize(1);
        tx.vin[0].scriptWitness.stack[0] = nonce;
        block.vtx[0] = MakeTransactionRef(std::move(tx));
    }
}

std::vector<unsigned char> GenerateCoinbaseCommitment(CBlock& block, const CBlockIndex* pindexPrev, const Consensus::Params& consensusParams)
{
    std::vector<unsigned char> commitment;
    if (fParticlMode)
        return commitment;

    int commitpos = GetWitnessCommitmentIndex(block);
    std::vector<unsigned char> ret(32, 0x00);
    if (consensusParams.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout != 0) {
        if (commitpos == -1) {
            uint256 witnessroot = BlockWitnessMerkleRoot(block, nullptr);
            CHash256().Write(witnessroot.begin(), 32).Write(ret.data(), 32).Finalize(witnessroot.begin());
            CTxOut out;
            out.nValue = 0;
            out.scriptPubKey.resize(38);
            out.scriptPubKey[0] = OP_RETURN;
            out.scriptPubKey[1] = 0x24;
            out.scriptPubKey[2] = 0xaa;
            out.scriptPubKey[3] = 0x21;
            out.scriptPubKey[4] = 0xa9;
            out.scriptPubKey[5] = 0xed;
            memcpy(&out.scriptPubKey[6], witnessroot.begin(), 32);
            commitment = std::vector<unsigned char>(out.scriptPubKey.begin(), out.scriptPubKey.end());
            CMutableTransaction tx(*block.vtx[0]);
            tx.vout.push_back(out);
            block.vtx[0] = MakeTransactionRef(std::move(tx));
        }
    }
    UpdateUncommittedBlockStructures(block, pindexPrev, consensusParams);
    return commitment;
}

unsigned int GetNextTargetRequired(const CBlockIndex *pindexLast)
{
    const Consensus::Params &consensus = Params().GetConsensus();

    arith_uint256 bnProofOfWorkLimit;
    unsigned int nProofOfWorkLimit;
    int nHeight = pindexLast ? pindexLast->nHeight+1 : 0;

    if (nHeight < (int)Params().GetLastImportHeight())
    {
        if (nHeight == 0)
            return arith_uint256("00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff").GetCompact();
        int nLastImportHeight = (int) Params().GetLastImportHeight();
        arith_uint256 nMaxProofOfWorkLimit = arith_uint256("000000000008ffffffffffffffffffffffffffffffffffffffffffffffffffff");
        arith_uint256 nMinProofOfWorkLimit = UintToArith256(consensus.powLimit);
        arith_uint256 nStep = (nMaxProofOfWorkLimit - nMinProofOfWorkLimit) / nLastImportHeight;


        bnProofOfWorkLimit = nMaxProofOfWorkLimit - (nStep * nHeight);
        nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();
    } else
    {
        bnProofOfWorkLimit = UintToArith256(consensus.powLimit);
        nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();
    };


    if (pindexLast == nullptr)
        return nProofOfWorkLimit; // Genesis block

    const CBlockIndex* pindexPrev = pindexLast;
    if (pindexPrev->pprev == nullptr)
        return nProofOfWorkLimit; // first block
    const CBlockIndex *pindexPrevPrev = pindexPrev->pprev;
    if (pindexPrevPrev->pprev == nullptr)
        return nProofOfWorkLimit; // second block

    int64_t nTargetSpacing = Params().GetTargetSpacing();
    int64_t nTargetTimespan = Params().GetTargetTimespan();
    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    if (nActualSpacing > nTargetSpacing * 10)
        nActualSpacing = nTargetSpacing * 10;

    // pos: target change every block
    // pos: retarget with exponential moving toward target spacing
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);

    int64_t nInterval = nTargetTimespan / nTargetSpacing;
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);


    if (bnNew <= 0 || bnNew > bnProofOfWorkLimit)
        return nProofOfWorkLimit;

    return bnNew.GetCompact();
}

/** Context-dependent validity checks.
 *  By "context", we mean only the previous block headers, but not the UTXO
 *  set; UTXO-related validity checks are done in ConnectBlock().
 *  NOTE: This function is not currently invoked by ConnectBlock(), so we
 *  should consider upgrade issues if we change which consensus rules are
 *  enforced in this function (eg by adding a new consensus rule). See comment
 *  in ConnectBlock().
 *  Note that -reindex-chainstate skips the validation that happens here!
 */
static bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& params, const CBlockIndex* pindexPrev, int64_t nAdjustedTime)
{
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
    const Consensus::Params& consensusParams = params.GetConsensus();

    if (fParticlMode && pindexPrev)
    {
        // Check proof-of-stake
        if (block.nBits != GetNextTargetRequired(pindexPrev))
            return state.DoS(100, false, REJECT_INVALID, "bad-proof-of-stake", true, strprintf("%s: Bad proof-of-stake target", __func__));
    } else
    {
        // Check proof of work
        if (block.nBits != GetNextWorkRequired(pindexPrev, &block, consensusParams))
            return state.DoS(100, false, REJECT_INVALID, "bad-diffbits", false, "incorrect proof of work");
    };

    // Check against checkpoints
    if (fCheckpointsEnabled) {
        // Don't accept any forks from the main chain prior to last checkpoint.
        // GetLastCheckpoint finds the last checkpoint in MapCheckpoints that's in our
        // MapBlockIndex.
        CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(params.Checkpoints());
        if (pcheckpoint && nHeight < pcheckpoint->nHeight)
            return state.DoS(100, error("%s: forked chain older than last checkpoint (height %d)", __func__, nHeight), REJECT_CHECKPOINT, "bad-fork-prior-to-checkpoint");
    }

    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return state.Invalid(false, REJECT_INVALID, "time-too-old", "block's timestamp is too early");

    // Check timestamp
    if (nHeight > 0
        && block.GetBlockTime() > nAdjustedTime + MAX_FUTURE_BLOCK_TIME)
        return state.Invalid(false, REJECT_INVALID, "time-too-new", "block timestamp too far in the future");

    // Reject outdated version blocks when 95% (75% on testnet) of the network has upgraded:
    // check for version 2, 3 and 4 upgrades
    if((block.nVersion < 2 && nHeight >= consensusParams.BIP34Height) ||
       (block.nVersion < 3 && nHeight >= consensusParams.BIP66Height) ||
       (block.nVersion < 4 && nHeight >= consensusParams.BIP65Height))
            return state.Invalid(false, REJECT_OBSOLETE, strprintf("bad-version(0x%08x)", block.nVersion),
                                 strprintf("rejected nVersion=0x%08x block", block.nVersion));

    return true;
}

/** NOTE: This function is not currently invoked by ConnectBlock(), so we
 *  should consider upgrade issues if we change which consensus rules are
 *  enforced in this function (eg by adding a new consensus rule). See comment
 *  in ConnectBlock().
 *  Note that -reindex-chainstate skips the validation that happens here!
 */
static bool ContextualCheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
    const int64_t nPrevTime = pindexPrev ? pindexPrev->nTime : 0;

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    if (fParticlMode)
    {
        nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
    } else
    {
        if (VersionBitsState(pindexPrev, consensusParams, Consensus::DEPLOYMENT_CSV, versionbitscache) == ThresholdState::ACTIVE) {
            nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
        }
    };

    int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                              ? (pindexPrev ? pindexPrev->GetMedianTimePast() : block.GetBlockTime())
                              : block.GetBlockTime();

    // Check that all transactions are finalized
    for (const auto& tx : block.vtx) {
        if (!IsFinalTx(*tx, nHeight, nLockTimeCutoff)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false, "non-final transaction");
        }
    }

    if (fParticlMode)
    {
        // Enforce rule that the coinbase/coinstake ends with serialized block height
        // genesis block scriptSig size will be different

        if (block.IsProofOfStake())
        {
            // Limit the number of outputs in a coinstake txn to 6: 1 data + 1 foundation + 4 user
            if (nPrevTime >= consensusParams.OpIsCoinstakeTime)
            {
                if (block.vtx[0]->vpout.size() > 6)
                    return state.DoS(100, false, REJECT_INVALID, "bad-cs-outputs", false, "Too many outputs in coinstake");
            };

            // coinstake output 0 must be data output of blockheight
            int i;
            if (!block.vtx[0]->GetCoinStakeHeight(i))
                return state.DoS(100, false, REJECT_INVALID, "bad-cs-malformed", false, "coinstake txn is malformed");

            if (i != nHeight)
                return state.DoS(100, false, REJECT_INVALID, "bad-cs-height", false, "block height mismatch in coinstake");

            std::vector<uint8_t> &vData = ((CTxOutData*)block.vtx[0]->vpout[0].get())->vData;
            if (vData.size() > 8 && vData[4] == DO_VOTE)
            {
                uint32_t voteToken;
                memcpy(&voteToken, &vData[5], 4);

                LogPrint(BCLog::HDWALLET, _("Block %d casts vote for option %u of proposal %u.\n").c_str(),
                    nHeight, voteToken >> 16, voteToken & 0xFFFF);
            };

            // check witness merkleroot, TODO: should witnessmerkleroot be hashed?
            bool malleated = false;
            uint256 hashWitness = BlockWitnessMerkleRoot(block, &malleated);

            if (hashWitness != block.hashWitnessMerkleRoot)
                return state.DoS(100, false, REJECT_INVALID, "bad-witness-merkle-match", true, strprintf("%s : witness merkle commitment mismatch", __func__));

            if (!CheckCoinStakeTimestamp(nHeight, block.GetBlockTime()))
                return state.DoS(50, false, REJECT_INVALID, "bad-coinstake-time", true, strprintf("%s: coinstake timestamp violation nTimeBlock=%d", __func__, block.GetBlockTime()));

            // Check timestamp against prev
            if (block.GetBlockTime() <= pindexPrev->GetPastTimeLimit() || FutureDrift(block.GetBlockTime()) < pindexPrev->GetBlockTime())
                return state.DoS(50, false, REJECT_INVALID, "bad-block-time", true, strprintf("%s: block's timestamp is too early", __func__));

            uint256 hashProof, targetProofOfStake;

            // Blocks are connected at end of import / reindex
            // CheckProofOfStake is run again during connectblock
            if (!IsInitialBlockDownload() // checks (!fImporting && !fReindex)
                && !CheckProofOfStake(pindexPrev, *block.vtx[0], block.nTime, block.nBits, hashProof, targetProofOfStake))
            {
                LogPrintf("WARNING: ContextualCheckBlock(): check proof-of-stake failed for block %s\n", block.GetHash().ToString());
                //return false; // do not error here as we expect this during initial block download
                if (pindexPrev->bnStakeModifier.IsNull())
                    // Can happen if the block is received out of order - CheckProofOfStake will run again on connectblock.
                    LogPrint(BCLog::POS, "%s: Accepting failed CheckProofOfStake block, missing stake-modifier.\n", __func__);
                else
                    return state.DoS(50, false, REJECT_INVALID, "bad-proof-of-stake", true, strprintf("%s: CheckProofOfStake failed.", __func__));
            };
        } else
        {
            bool fCheckPOW = true; // TODO: pass properly
            if (fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits, consensusParams, nHeight, Params().GetLastImportHeight()))
                return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed");

            // Enforce rule that the coinbase/ ends with serialized block height
            // genesis block scriptSig size will be different
            CScript expect = CScript() << OP_RETURN << nHeight;
            const CScript &scriptSig = block.vtx[0]->vin[0].scriptSig;
            if (scriptSig.size() < expect.size() ||
                !std::equal(expect.begin()
                    , expect.end(), scriptSig.begin() + scriptSig.size()-expect.size())) {
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false, "block height mismatch in coinbase");
            }
        };

        if (nHeight > 0 && !block.vtx[0]->IsCoinStake()) // only genesis block can start with coinbase
            return state.DoS(100, false, REJECT_INVALID, "bad-cs-missing", false, "first tx is not coinstake");

        if (nHeight > 0 // skip genesis
            && Params().GetLastImportHeight() >= (uint32_t)nHeight)
        {
            // 2nd txn must be coinbase
            if (block.vtx.size() < 2 || !block.vtx[1]->IsCoinBase())
                return state.DoS(100, false, REJECT_INVALID, "bad-cb", false, "Second txn of import block must be coinbase");

            // Check hash of genesis import txn matches expected hash.
            uint256 txnHash = block.vtx[1]->GetHash();
            if (!Params().CheckImportCoinbase(nHeight, txnHash))
                return state.DoS(100, false, REJECT_INVALID, "bad-cb", false, "Incorrect outputs hash.");
        } else
        {
            // 2nd txn can't be coinbase if block height > GetLastImportHeight
            if (block.vtx.size() > 1 && block.vtx[1]->IsCoinBase())
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "unexpected coinbase");
        };
    } else
    {
        if (nHeight >= consensusParams.BIP34Height)
        {
            CScript expect = CScript() << nHeight;
            if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
                !std::equal(expect.begin(), expect.end(), block.vtx[0]->vin[0].scriptSig.begin())) {
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false, "block height mismatch in coinbase");
            }
        }

        // Validation for witness commitments.
        // * We compute the witness hash (which is the hash including witnesses) of all the block's transactions, except the
        //   coinbase (where 0x0000....0000 is used instead).
        // * The coinbase scriptWitness is a stack of a single 32-byte vector, containing a witness reserved value (unconstrained).
        // * We build a merkle tree with all those witness hashes as leaves (similar to the hashMerkleRoot in the block header).
        // * There must be at least one output whose scriptPubKey is a single 36-byte push, the first 4 bytes of which are
        //   {0xaa, 0x21, 0xa9, 0xed}, and the following 32 bytes are SHA256^2(witness root, witness reserved value). In case there are
        //   multiple, the last one is used.
        bool fHaveWitness = false;
        if (VersionBitsState(pindexPrev, consensusParams, Consensus::DEPLOYMENT_SEGWIT, versionbitscache) == ThresholdState::ACTIVE) {
            int commitpos = GetWitnessCommitmentIndex(block);
            if (commitpos != -1) {
                bool malleated = false;
                uint256 hashWitness = BlockWitnessMerkleRoot(block, &malleated);
                // The malleation check is ignored; as the transaction tree itself
                // already does not permit it, it is impossible to trigger in the
                // witness tree.
                if (block.vtx[0]->vin[0].scriptWitness.stack.size() != 1 || block.vtx[0]->vin[0].scriptWitness.stack[0].size() != 32) {
                    return state.DoS(100, false, REJECT_INVALID, "bad-witness-nonce-size", true, strprintf("%s : invalid witness reserved value size", __func__));
                }
                CHash256().Write(hashWitness.begin(), 32).Write(&block.vtx[0]->vin[0].scriptWitness.stack[0][0], 32).Finalize(hashWitness.begin());
                if (memcmp(hashWitness.begin(), &block.vtx[0]->vout[commitpos].scriptPubKey[6], 32)) {
                    return state.DoS(100, false, REJECT_INVALID, "bad-witness-merkle-match", true, strprintf("%s : witness merkle commitment mismatch", __func__));
                }
                fHaveWitness = true;
            }
        }

        // No witness data is allowed in blocks that don't commit to witness data, as this would otherwise leave room for spam
        if (!fHaveWitness) {
          for (const auto& tx : block.vtx) {
                if (tx->HasWitness()) {
                    return state.DoS(100, false, REJECT_INVALID, "unexpected-witness", true, strprintf("%s : unexpected witness data found", __func__));
                }
            }
        }
    };

    // After the coinbase witness reserved value and commitment are verified,
    // we can check if the block weight passes (before we've checked the
    // coinbase witness, it would be possible for the weight to be too
    // large by filling up the coinbase witness, which doesn't change
    // the block hash, so we couldn't mark the block as permanently
    // failed).
    if (GetBlockWeight(block) > MAX_BLOCK_WEIGHT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-weight", false, strprintf("%s : weight limit failed", __func__));
    }

    return true;
}

bool ProcessDuplicateStakeHeader(CBlockIndex *pindex, NodeId nodeId)
{
    if (!pindex)
        return false;

    uint256 hash = pindex->GetBlockHash();

    bool fMakeValid = false;
    if (nodeId == -1)
    {
        LogPrintf("%s: Duplicate stake block %s was received in a group, marking valid.\n",
            __func__, hash.ToString());

        fMakeValid = true;
    };

    if (nodeId > -1)
    {
        std::pair<std::map<uint256, StakeConflict>::iterator,bool> ret;
        ret = mapStakeConflict.insert(std::pair<uint256, StakeConflict>(hash, StakeConflict()));
        StakeConflict &sc = ret.first->second;
        sc.Add(nodeId);

        if ((int)sc.peerCount.size() > std::min(GetNumPeers() / 2, 4))
        {
            LogPrintf("%s: More than half the connected peers are building on block %s," /* Continued */
                "  marked as duplicate stake, assuming this node has the duplicate.\n", __func__, hash.ToString());

            fMakeValid = true;
        };
    };

    if (fMakeValid)
    {
        pindex->nFlags &= (~BLOCK_FAILED_DUPLICATE_STAKE);
        pindex->nStatus |= BLOCK_VALID_HEADER;
        pindex->nStatus &= (~BLOCK_FAILED_VALID);
        setDirtyBlockIndex.insert(pindex);

        //if (pindex->nStatus & BLOCK_FAILED_CHILD)
        //{
            CBlockIndex *pindexPrev = pindex->pprev;
            while (pindexPrev)
            {
                if (pindexPrev->nStatus & BLOCK_VALID_MASK)
                    break;

                if (pindexPrev->nFlags & BLOCK_FAILED_DUPLICATE_STAKE)
                {
                    pindexPrev->nFlags &= (~BLOCK_FAILED_DUPLICATE_STAKE);
                    pindexPrev->nStatus |= BLOCK_VALID_HEADER;
                    pindexPrev->nStatus &= (~BLOCK_FAILED_VALID);
                    setDirtyBlockIndex.insert(pindexPrev);

                    if (!pindexPrev->prevoutStake.IsNull())
                    {
                        uint256 prevhash = pindexPrev->GetBlockHash();
                        AddToMapStakeSeen(pindexPrev->prevoutStake, prevhash);
                    };

                    pindexPrev->nStatus &= (~BLOCK_FAILED_CHILD);
                };

                pindexPrev = pindexPrev->pprev;
            };

            pindex->nStatus &= (~BLOCK_FAILED_CHILD);
        //};

        if (!pindex->prevoutStake.IsNull())
        {
            AddToMapStakeSeen(pindex->prevoutStake, hash);
        };
        return true;
    };

    return false;
}

bool CChainState::AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex *pindex = nullptr;
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {
        if (miSelf != mapBlockIndex.end()) {
            // Block header is already known.
            pindex = miSelf->second;
            if (ppindex)
                *ppindex = pindex;

            if (pindex->nStatus & BLOCK_FAILED_MASK)
            {
                /*
                if (pindex->nFlags & BLOCK_FAILED_DUPLICATE_STAKE
                    && ProcessDuplicateStakeHeader(pindex, state.nodeId))
                {
                    // pass
                } else */
                {
                    return state.Invalid(error("%s: block %s is marked invalid", __func__, hash.ToString()), 0, "duplicate");
                };
            };
            return true;
        }

        if (!CheckBlockHeader(block, state, chainparams.GetConsensus()))
            return error("%s: Consensus::CheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

        // Get prev block index
        CBlockIndex* pindexPrev = nullptr;
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("%s: prev block not found", __func__), 0, "prev-blk-not-found");
        pindexPrev = (*mi).second;

        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
        if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev, GetAdjustedTime()))
            return error("%s: Consensus::ContextualCheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

        // If the previous block index isn't valid, determine if it descends from any block which
        // has been found invalid (g_failed_blocks), then mark pindexPrev and any blocks
        // between them as failed.
        if (!pindexPrev->IsValid(BLOCK_VALID_SCRIPTS)) {
            for (const CBlockIndex* failedit : m_failed_blocks) {
                if (pindexPrev->GetAncestor(failedit->nHeight) == failedit) {
                    assert(failedit->nStatus & BLOCK_FAILED_VALID);
                    CBlockIndex* invalid_walk = pindexPrev;
                    while (invalid_walk != failedit) {
                        invalid_walk->nStatus |= BLOCK_FAILED_CHILD;
                        setDirtyBlockIndex.insert(invalid_walk);
                        invalid_walk = invalid_walk->pprev;
                    }
                    return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
                }
            }
        }
    }
    if (pindex == nullptr)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    CheckBlockIndex(chainparams.GetConsensus());

    return true;
}

// Exposed wrapper for AcceptBlockHeader
bool ProcessNewBlockHeaders(const std::vector<CBlockHeader>& headers, CValidationState& state, const CChainParams& chainparams, const CBlockIndex** ppindex, CBlockHeader *first_invalid)
{
    if (first_invalid != nullptr) first_invalid->SetNull();
    {
        LOCK(cs_main);
        for (const CBlockHeader& header : headers) {
            CBlockIndex *pindex = nullptr; // Use a temp pindex instead of ppindex to avoid a const_cast
            if (!g_chainstate.AcceptBlockHeader(header, state, chainparams, &pindex)) {
                if (first_invalid) *first_invalid = header;
                return false;
            }
            if (ppindex) {
                *ppindex = pindex;
            }
        }
    }
    NotifyHeaderTip();
    return true;
}

/** Store block on disk. If dbp is non-nullptr, the file is known to already reside on disk */
static CDiskBlockPos SaveBlockToDisk(const CBlock& block, int nHeight, const CChainParams& chainparams, const CDiskBlockPos* dbp) {
    unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
    CDiskBlockPos blockPos;
    if (dbp != nullptr)
        blockPos = *dbp;
    if (!FindBlockPos(blockPos, nBlockSize+8, nHeight, block.GetBlockTime(), dbp != nullptr)) {
        error("%s: FindBlockPos failed", __func__);
        return CDiskBlockPos();
    }
    if (dbp == nullptr) {
        if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart())) {
            AbortNode("Failed to write block");
            return CDiskBlockPos();
        }
    }
    return blockPos;
}

/** Store block on disk. If dbp is non-nullptr, the file is known to already reside on disk */
bool CChainState::AcceptBlock(const std::shared_ptr<const CBlock>& pblock, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex, bool fRequested, const CDiskBlockPos* dbp, bool* fNewBlock)
{
    const CBlock& block = *pblock;

    if (fNewBlock) *fNewBlock = false;
    AssertLockHeld(cs_main);

    CBlockIndex *pindexDummy = nullptr;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;

    if (!AcceptBlockHeader(block, state, chainparams, &pindex))
        return false;

    if (block.IsProofOfStake())
    {
        pindex->SetProofOfStake();
        pindex->prevoutStake = pblock->vtx[0]->vin[0].prevout;
        if (pindex->pprev && pindex->pprev->bnStakeModifier.IsNull()) // block received out of order
        {
            if (!IsInitialBlockDownload())
                LogPrintf("Warning: %s - Previous stake modifier is null.\n", __func__);
        } else
        {
            pindex->bnStakeModifier = ComputeStakeModifierV2(pindex->pprev, pindex->prevoutStake.hash);
        };
        setDirtyBlockIndex.insert(pindex);
    };

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreOrSameWork = (chainActive.Tip() ? pindex->nChainWork >= chainActive.Tip()->nChainWork : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead = (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: Decouple this function from the block download logic by removing fRequested
    // This requires some new chain data structure to efficiently look up if a
    // block is in a chain leading to a candidate for best tip, despite not
    // being such a candidate itself.

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) return true;
    if (!fRequested) {  // If we didn't ask for it:
        if (pindex->nTx != 0) return true;    // This is a previously-processed block that was pruned
        if (!fHasMoreOrSameWork) return true; // Don't process less-work chains
        if (fTooFarAhead) return true;        // Block height is too high

        // Protect against DoS attacks from low-work chains.
        // If our tip is behind, a peer could try to send us
        // low-work blocks on a fake chain that we would never
        // request; don't process these.
        if (pindex->nChainWork < nMinimumChainWork) return true;
    }
    if (fNewBlock) *fNewBlock = true;

    if (!CheckBlock(block, state, chainparams.GetConsensus()) ||
        !ContextualCheckBlock(block, state, chainparams.GetConsensus(), pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s", __func__, FormatStateMessage(state));
    }

    // Header is valid/has work, merkle tree and segwit merkle tree are good...RELAY NOW
    // (but if it does not build on our best tip, let the SendMessages loop relay it)
    if (!IsInitialBlockDownload() && chainActive.Tip() == pindex->pprev)
    {
        if (!(state.nFlags & BLOCK_FAILED_DUPLICATE_STAKE))
            GetMainSignals().NewPoWValidBlock(pindex, pblock);
    };

    // Write block to history file
    try {
        CDiskBlockPos blockPos = SaveBlockToDisk(block, pindex->nHeight, chainparams, dbp);
        if (blockPos.IsNull()) {
            state.Error(strprintf("%s: Failed to find position to write new block to disk", __func__));
            return false;
        }
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos, chainparams.GetConsensus()))
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    FlushStateToDisk(chainparams, state, FlushStateMode::NONE);

    CheckBlockIndex(chainparams.GetConsensus());

    return true;
}

bool ProcessNewBlock(const CChainParams& chainparams, const std::shared_ptr<const CBlock> pblock, bool fForceProcessing, bool *fNewBlock)
{
    AssertLockNotHeld(cs_main);

    {
        /*
        uint256 hash = pblock->GetHash();
        // Limited duplicity on stake: prevents block flood attack
        // Duplicate stake allowed only when there is orphan child block
        if (!fReindex && !fImporting && pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash))
            return error("%s: Duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());
        */

        CBlockIndex *pindex = nullptr;
        if (fNewBlock) *fNewBlock = false;
        CValidationState state;
        // Ensure that CheckBlock() passes before calling AcceptBlock, as
        // belt-and-suspenders.
        bool ret = CheckBlock(*pblock, state, chainparams.GetConsensus());

        LOCK(cs_main);

        if (ret) {
            // Store to disk
            ret = g_chainstate.AcceptBlock(pblock, state, chainparams, &pindex, fForceProcessing, nullptr, fNewBlock);
        }
        if (!ret) {
            if (fParticlMode)
            {
                // Mark block as invalid to prevent re-requesting from peer.
                // Block will have been added to the block index in AcceptBlockHeader
                CBlockIndex *pindex = g_chainstate.AddToBlockIndex(*pblock);
                g_chainstate.InvalidBlockFound(pindex, *pblock, state);

                if (IncomingBlockChecked(*pblock, state)) // returns true if it did nothing
                    GetMainSignals().BlockChecked(*pblock, state);
            } else
            {
                GetMainSignals().BlockChecked(*pblock, state);
            }
            return error("%s: AcceptBlock FAILED (%s)", __func__, FormatStateMessage(state));
        }

        if (pindex && state.nFlags & BLOCK_FAILED_DUPLICATE_STAKE)
        {
            pindex->nFlags |= BLOCK_FAILED_DUPLICATE_STAKE;
            setDirtyBlockIndex.insert(pindex);
            LogPrint(BCLog::POS, "%s Marking duplicate stake: %s.\n", __func__, pindex->GetBlockHash().ToString());
            //GetMainSignals().BlockChecked(*pblock, state);
            IncomingBlockChecked(*pblock, state);
        };
    }

    NotifyHeaderTip();

    CValidationState state; // Only used to report errors, not invalidity - ignore it
    if (!g_chainstate.ActivateBestChain(state, chainparams, pblock))
        return error("%s: ActivateBestChain failed (%s)", __func__, FormatStateMessage(state));

    if (smsg::fSecMsgEnabled && gArgs.GetBoolArg("-smsgscanincoming", false))
        smsgModule.ScanBlock(*pblock);
    return true;
}

bool TestBlockValidity(CValidationState& state, const CChainParams& chainparams, const CBlock& block, CBlockIndex* pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev && pindexPrev == chainActive.Tip());
    CCoinsViewCache viewNew(pcoinsTip.get());
    uint256 block_hash(block.GetHash());
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;
    indexDummy.phashBlock = &block_hash;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev, GetAdjustedTime()))
        return error("%s: Consensus::ContextualCheckBlockHeader: %s", __func__, FormatStateMessage(state));
    if (!CheckBlock(block, state, chainparams.GetConsensus(), fCheckPOW, fCheckMerkleRoot))
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));
    if (!ContextualCheckBlock(block, state, chainparams.GetConsensus(), pindexPrev))
        return error("%s: Consensus::ContextualCheckBlock: %s", __func__, FormatStateMessage(state));
    if (!g_chainstate.ConnectBlock(block, state, &indexDummy, viewNew, chainparams, true))
        return false;
    assert(state.IsValid());

    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage()
{
    LOCK(cs_LastBlockFile);

    uint64_t retval = 0;
    for (const CBlockFileInfo &file : vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

/* Prune a block file (modify associated database entries)*/
void PruneOneBlockFile(const int fileNumber)
{
    LOCK(cs_LastBlockFile);

    for (const auto& entry : mapBlockIndex) {
        CBlockIndex* pindex = entry.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    mapBlocksUnlinked.erase(_it);
                }
            }
        }
    }

    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}


void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune)
{
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        CDiskBlockPos pos(*it, 0);
        fs::remove(GetBlockPosFilename(pos, "blk"));
        fs::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

/* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
static void FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight)
{
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr)
        return;

    // last block to prune is the lesser of (user-specified height, MIN_BLOCKS_TO_KEEP from the tip)
    unsigned int nLastBlockWeCanPrune = std::min((unsigned)nManualPruneHeight, chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP);
    int count=0;
    for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
        if (vinfoBlockFile[fileNumber].nSize == 0 || vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
            continue;
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n", nLastBlockWeCanPrune, count);
}

/* This function is called from the RPC code for pruneblockchain */
void PruneBlockFilesManual(int nManualPruneHeight)
{
    CValidationState state;
    const CChainParams& chainparams = Params();
    if (!FlushStateToDisk(chainparams, state, FlushStateMode::NONE, nManualPruneHeight)) {
        LogPrintf("%s: failed to flush state (%s)\n", __func__, FormatStateMessage(state));
    }
}

/**
 * Prune block and undo files (blk???.dat and undo???.dat) so that the disk space used is less than a user-defined target.
 * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
 * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
 * (which in this case means the blockchain must be re-downloaded.)
 *
 * Pruning functions are called from FlushStateToDisk when the global fCheckForPruning flag has been set.
 * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
 * Pruning cannot take place until the longest chain is at least a certain length (100000 on mainnet, 1000 on testnet, 1000 on regtest).
 * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
 * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
 * A db flag records the fact that at least some block files have been pruned.
 *
 * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
 */
static void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr || nPruneTarget == 0) {
        return;
    }
    if ((uint64_t)chainActive.Tip()->nHeight <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune = chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count=0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize + vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0)
                continue;

            if (nCurrentUsage + nBuffer < nPruneTarget)  // are we below our target?
                break;

            // don't prune files that could have a block within MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
                continue;

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "Prune: target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
           nPruneTarget/1024/1024, nCurrentUsage/1024/1024,
           ((int64_t)nPruneTarget - (int64_t)nCurrentUsage)/1024/1024,
           nLastBlockWeCanPrune, count);
}

bool CheckDiskSpace(uint64_t nAdditionalBytes, bool blocks_dir)
{
    uint64_t nFreeBytesAvailable = fs::space(blocks_dir ? GetBlocksDir() : GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

static FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return nullptr;
    fs::path path = GetBlockPosFilename(pos, prefix);
    fs::create_directories(path.parent_path());
    FILE* file = fsbridge::fopen(path, fReadOnly ? "rb": "rb+");
    if (!file && !fReadOnly)
        file = fsbridge::fopen(path, "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return nullptr;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return nullptr;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

/** Open an undo file (rev?????.dat) */
static FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

fs::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix)
{
    return GetBlocksDir() / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex * CChainState::InsertBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (hash.IsNull())
        return nullptr;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool CChainState::LoadBlockIndex(const Consensus::Params& consensus_params, CBlockTreeDB& blocktree)
{
    if (!blocktree.LoadBlockIndexGuts(consensus_params, [this](const uint256& hash){ return this->InsertBlockIndex(hash); }))
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex*>& item : vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);
        // We can link the chain of blocks for which we've received transactions at some point.
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            setDirtyBlockIndex.insert(pindex);
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == nullptr))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == nullptr || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    return true;
}

bool static LoadBlockIndexDB(const CChainParams& chainparams)
{
    if (!g_chainstate.LoadBlockIndex(chainparams.GetConsensus(), *pblocktree))
        return false;

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++)
    {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned)
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    if(fReindexing) fReindex = true;

    // Check whether we have a transaction index
    //pblocktree->ReadFlag("txindex", fTxIndex);
    //LogPrintf("%s: transaction index %s\n", __func__, fTxIndex ? "enabled" : "disabled");

    // Check whether we have an address index
    pblocktree->ReadFlag("addressindex", fAddressIndex);
    LogPrintf("%s: address index %s\n", __func__, fAddressIndex ? "enabled" : "disabled");

    // Check whether we have a timestamp index
    pblocktree->ReadFlag("timestampindex", fTimestampIndex);
    LogPrintf("%s: timestamp index %s\n", __func__, fTimestampIndex ? "enabled" : "disabled");

    // Check whether we have a spent index
    pblocktree->ReadFlag("spentindex", fSpentIndex);
    LogPrintf("%s: spent index %s\n", __func__, fSpentIndex ? "enabled" : "disabled");

    return true;
}

bool LoadChainTip(const CChainParams& chainparams)
{
    AssertLockHeld(cs_main);

    if (chainActive.Tip() && chainActive.Tip()->GetBlockHash() == pcoinsTip->GetBestBlock()) return true;

    if (pcoinsTip->GetBestBlock().IsNull() && mapBlockIndex.size() == 1) {
        // In case we just added the genesis block, connect it now, so
        // that we always have a chainActive.Tip() when we return.
        LogPrintf("%s: Connecting genesis block...\n", __func__);
        CValidationState state;
        if (!ActivateBestChain(state, chainparams)) {
            LogPrintf("%s: failed to activate chain (%s)\n", __func__, FormatStateMessage(state));
            return false;
        }
    }

    // Load pointer to end of best chain
    CBlockIndex* pindex = LookupBlockIndex(pcoinsTip->GetBestBlock());
    if (!pindex) {
        return false;
    }
    chainActive.SetTip(pindex);

    g_chainstate.PruneBlockIndexCandidates();

    LogPrintf("Loaded best chain: hashBestChain=%s height=%d date=%s progress=%f\n",
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        FormatISO8601DateTime(chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainparams.TxData(), chainActive.Tip()));
    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0, false);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100, false);
}

bool CVerifyDB::VerifyDB(const CChainParams& chainparams, CCoinsView *coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == nullptr || chainActive.Tip()->pprev == nullptr)
        return true;

    fVerifyingDB = true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0 || nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = nullptr;
    int nGoodTransactions = 0;
    CValidationState state;
    int reportDone = 0;
    LogPrintf("[0%%]..."); /* Continued */
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev)
    {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));
        if (reportDone < percentageDone/10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone); /* Continued */
            reportDone = percentageDone/10;
        }
        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone, false);
        if (pindex->nHeight < chainActive.Height()-nCheckDepth)
            break;
        if (fPruneMode && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, only go back as far as we have data.
            LogPrintf("VerifyDB(): block verification stopping at height %d (pruning, no data)\n", pindex->nHeight);
            break;
        }
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
            return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state, chainparams.GetConsensus()))
            return error("%s: *** found bad block at %d, hash=%s (%s)\n", __func__,
                         pindex->nHeight, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            if (!pindex->GetUndoPos().IsNull()) {
                if (!UndoReadFromDisk(undo, pindex)) {
                    return error("VerifyDB(): *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                }
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            assert(coins.GetBestBlock() == pindex->GetBlockHash());
            DisconnectResult res = g_chainstate.DisconnectBlock(block, pindex, coins);
            if (res == DISCONNECT_FAILED) {
                return error("VerifyDB(): *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB(): *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))), false);
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
                return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!g_chainstate.ConnectBlock(block, state, pindex, coins, chainparams))
                return error("VerifyDB(): *** found unconnectable block at %d, hash=%s (%s)", pindex->nHeight, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
        }
    }

    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);
    fVerifyingDB = false;

    return true;
}

/** Apply the effects of a block on the utxo cache, ignoring that it may already have been applied. */
bool CChainState::RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs, const CChainParams& params)
{
    // TODO: merge with ConnectBlock
    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, params.GetConsensus())) {
        return error("ReplayBlock(): ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
    }

    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const CTxIn &txin : tx->vin) {
                inputs.SpendCoin(txin.prevout);
            }
        }
        // Pass check = true as every addition may be an overwrite.
        AddCoins(inputs, *tx, pindex->nHeight, true);
    }
    return true;
}

bool CChainState::ReplayBlocks(const CChainParams& params, CCoinsView* view)
{
    LOCK(cs_main);

    CCoinsViewCache cache(view);

    std::vector<uint256> hashHeads = view->GetHeadBlocks();
    if (hashHeads.empty()) return true; // We're already in a consistent state.
    if (hashHeads.size() != 2) return error("ReplayBlocks(): unknown inconsistent state");

    uiInterface.ShowProgress(_("Replaying blocks..."), 0, false);
    LogPrintf("Replaying blocks\n");

    const CBlockIndex* pindexOld = nullptr;  // Old tip during the interrupted flush.
    const CBlockIndex* pindexNew;            // New tip during the interrupted flush.
    const CBlockIndex* pindexFork = nullptr; // Latest block common to both the old and the new tip.

    if (mapBlockIndex.count(hashHeads[0]) == 0) {
        return error("ReplayBlocks(): reorganization to unknown block requested");
    }
    pindexNew = mapBlockIndex[hashHeads[0]];

    if (!hashHeads[1].IsNull()) { // The old tip is allowed to be 0, indicating it's the first flush.
        if (mapBlockIndex.count(hashHeads[1]) == 0) {
            return error("ReplayBlocks(): reorganization from unknown block requested");
        }
        pindexOld = mapBlockIndex[hashHeads[1]];
        pindexFork = LastCommonAncestor(pindexOld, pindexNew);
        assert(pindexFork != nullptr);
    }

    // Rollback along the old branch.
    while (pindexOld != pindexFork) {
        if (pindexOld->nHeight > 0) { // Never disconnect the genesis block.
            CBlock block;
            if (!ReadBlockFromDisk(block, pindexOld, params.GetConsensus())) {
                return error("RollbackBlock(): ReadBlockFromDisk() failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            LogPrintf("Rolling back %s (%i)\n", pindexOld->GetBlockHash().ToString(), pindexOld->nHeight);
            DisconnectResult res = DisconnectBlock(block, pindexOld, cache);
            if (res == DISCONNECT_FAILED) {
                return error("RollbackBlock(): DisconnectBlock failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            // If DISCONNECT_UNCLEAN is returned, it means a non-existing UTXO was deleted, or an existing UTXO was
            // overwritten. It corresponds to cases where the block-to-be-disconnect never had all its operations
            // applied to the UTXO set. However, as both writing a UTXO and deleting a UTXO are idempotent operations,
            // the result is still a version of the UTXO set with the effects of that block undone.
        }
        pindexOld = pindexOld->pprev;
    }

    // Roll forward from the forking point to the new tip.
    int nForkHeight = pindexFork ? pindexFork->nHeight : 0;
    for (int nHeight = nForkHeight + 1; nHeight <= pindexNew->nHeight; ++nHeight) {
        const CBlockIndex* pindex = pindexNew->GetAncestor(nHeight);
        LogPrintf("Rolling forward %s (%i)\n", pindex->GetBlockHash().ToString(), nHeight);
        if (!RollforwardBlock(pindex, cache, params)) return false;
    }

    cache.SetBestBlock(pindexNew->GetBlockHash(), pindexNew->nHeight);
    cache.Flush();
    uiInterface.ShowProgress("", 100, false);
    return true;
}

bool ReplayBlocks(const CChainParams& params, CCoinsView* view) {
    return g_chainstate.ReplayBlocks(params, view);
}

bool CChainState::RewindBlockIndex(const CChainParams& params)
{
    LOCK(cs_main);

    // Note that during -reindex-chainstate we are called with an empty chainActive!

    int nHeight = 1;
    while (nHeight <= chainActive.Height()) {
        // Although SCRIPT_VERIFY_WITNESS is now generally enforced on all
        // blocks in ConnectBlock, we don't need to go back and
        // re-download/re-verify blocks from before segwit actually activated.
        if (IsWitnessEnabled(chainActive[nHeight - 1], params.GetConsensus()) && !(chainActive[nHeight]->nStatus & BLOCK_OPT_WITNESS)) {
            break;
        }
        nHeight++;
    }

    // nHeight is now the height of the first insufficiently-validated block, or tipheight + 1
    CValidationState state;
    CBlockIndex* pindex = chainActive.Tip();
    while (chainActive.Height() >= nHeight) {
        if (fPruneMode && !(chainActive.Tip()->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, don't try rewinding past the HAVE_DATA point;
            // since older blocks can't be served anyway, there's
            // no need to walk further, and trying to DisconnectTip()
            // will fail (and require a needless reindex/redownload
            // of the blockchain).
            break;
        }
        if (!DisconnectTip(state, params, nullptr)) {
            return error("RewindBlockIndex: unable to disconnect block at height %i (%s)", pindex->nHeight, FormatStateMessage(state));
        }
        // Occasionally flush state to disk.
        if (!FlushStateToDisk(params, state, FlushStateMode::PERIODIC)) {
            LogPrintf("RewindBlockIndex: unable to flush state to disk (%s)\n", FormatStateMessage(state));
            return false;
        }
    }

    // Reduce validity flag and have-data flags.
    // We do this after actual disconnecting, otherwise we'll end up writing the lack of data
    // to disk before writing the chainstate, resulting in a failure to continue if interrupted.
    for (const auto& entry : mapBlockIndex) {
        CBlockIndex* pindexIter = entry.second;

        // Note: If we encounter an insufficiently validated block that
        // is on chainActive, it must be because we are a pruning node, and
        // this block or some successor doesn't HAVE_DATA, so we were unable to
        // rewind all the way.  Blocks remaining on chainActive at this point
        // must not have their validity reduced.
        if (IsWitnessEnabled(pindexIter->pprev, params.GetConsensus()) && !(pindexIter->nStatus & BLOCK_OPT_WITNESS) && !chainActive.Contains(pindexIter)) {
            // Reduce validity
            pindexIter->nStatus = std::min<unsigned int>(pindexIter->nStatus & BLOCK_VALID_MASK, BLOCK_VALID_TREE) | (pindexIter->nStatus & ~BLOCK_VALID_MASK);
            // Remove have-data flags.
            pindexIter->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
            // Remove storage location.
            pindexIter->nFile = 0;
            pindexIter->nDataPos = 0;
            pindexIter->nUndoPos = 0;
            // Remove various other things
            pindexIter->nTx = 0;
            pindexIter->nChainTx = 0;
            pindexIter->nSequenceId = 0;
            // Make sure it gets written.
            setDirtyBlockIndex.insert(pindexIter);
            // Update indexes
            setBlockIndexCandidates.erase(pindexIter);
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> ret = mapBlocksUnlinked.equal_range(pindexIter->pprev);
            while (ret.first != ret.second) {
                if (ret.first->second == pindexIter) {
                    mapBlocksUnlinked.erase(ret.first++);
                } else {
                    ++ret.first;
                }
            }
        } else if (pindexIter->IsValid(BLOCK_VALID_TRANSACTIONS) && pindexIter->nChainTx) {
            setBlockIndexCandidates.insert(pindexIter);
        }
    }

    if (chainActive.Tip() != nullptr) {
        // We can't prune block index candidates based on our tip if we have
        // no tip due to chainActive being empty!
        PruneBlockIndexCandidates();

        CheckBlockIndex(params.GetConsensus());
    }

    return true;
}

bool RewindBlockIndex(const CChainParams& params) {
    if (!g_chainstate.RewindBlockIndex(params)) {
        return false;
    }

    if (chainActive.Tip() != nullptr) {
        // FlushStateToDisk can possibly read chainActive. Be conservative
        // and skip it here, we're about to -reindex-chainstate anyway, so
        // it'll get called a bunch real soon.
        CValidationState state;
        if (!FlushStateToDisk(params, state, FlushStateMode::ALWAYS)) {
            LogPrintf("RewindBlockIndex: unable to flush state to disk (%s)\n", FormatStateMessage(state));
            return false;
        }
    }

    return true;
}

void CChainState::UnloadBlockIndex() {
    nBlockSequenceId = 1;
    m_failed_blocks.clear();
    setBlockIndexCandidates.clear();
}

// May NOT be used after any connections are up as much
// of the peer-processing logic assumes a consistent
// block index state
void UnloadBlockIndex()
{
    LOCK(cs_main);
    chainActive.SetTip(nullptr);
    pindexBestInvalid = nullptr;
    pindexBestHeader = nullptr;
    mempool.clear();
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    versionbitscache.Clear();
    for (int b = 0; b < VERSIONBITS_NUM_BITS; b++) {
        warningcache[b].clear();
    }

    for (BlockMap::value_type& entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
    fHavePruned = false;

    g_chainstate.UnloadBlockIndex();
}

bool TryAutoReindex()
{
    // Force reindex to update version
    bool nV1 = false;
    if (!pblocktree->ReadFlag("v1", nV1) || !nV1)
        return true;
    LogPrintf("%s: v1 Marker not detected, attempting reindex.\n", __func__);
    return false;
};

bool LoadBlockIndex(const CChainParams& chainparams)
{
    // Load block index from databases
    bool needs_init = fReindex;

    if (!fReindex) {
        bool ret = LoadBlockIndexDB(chainparams);
        if (!ret) return false;
        needs_init = mapBlockIndex.empty();
    }

    if (needs_init) {
        // Everything here is for *new* reindex/DBs. Thus, though
        // LoadBlockIndexDB may have set fReindex if we shut down
        // mid-reindex previously, we don't check fReindex and
        // instead only check it prior to LoadBlockIndexDB to set
        // needs_init.

        LogPrintf("Initializing databases...\n");
        pblocktree->WriteFlag("v1", true);

        // Use the provided setting for -addressindex in the new database
        fAddressIndex = gArgs.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX);
        pblocktree->WriteFlag("addressindex", fAddressIndex);
        LogPrintf("%s: address index %s\n", __func__, fAddressIndex ? "enabled" : "disabled");

        // Use the provided setting for -timestampindex in the new database
        fTimestampIndex = gArgs.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX);
        pblocktree->WriteFlag("timestampindex", fTimestampIndex);
        LogPrintf("%s: timestamp index %s\n", __func__, fTimestampIndex ? "enabled" : "disabled");

        // Use the provided setting for -spentindex in the new database
        fSpentIndex = gArgs.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX);
        pblocktree->WriteFlag("spentindex", fSpentIndex);
        LogPrintf("%s: spent index %s\n", __func__, fSpentIndex ? "enabled" : "disabled");
    }
    return true;
}

bool CChainState::LoadGenesisBlock(const CChainParams& chainparams)
{
    LOCK(cs_main);

    // Check whether we're already initialized by checking for genesis in
    // mapBlockIndex. Note that we can't use chainActive here, since it is
    // set based on the coins db, not the block index db, which is the only
    // thing loaded at this point.
    if (mapBlockIndex.count(chainparams.GenesisBlock().GetHash()))
        return true;

    try {
        CBlock &block = const_cast<CBlock&>(chainparams.GenesisBlock());
        CDiskBlockPos blockPos = SaveBlockToDisk(block, 0, chainparams, nullptr);
        if (blockPos.IsNull())
            return error("%s: writing genesis block to disk failed", __func__);
        CBlockIndex *pindex = AddToBlockIndex(block);
        CValidationState state;
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos, chainparams.GetConsensus()))
            return error("%s: genesis block not accepted (%s)", __func__, FormatStateMessage(state));
    } catch (const std::runtime_error& e) {
        return error("%s: failed to write genesis block: %s", __func__, e.what());
    }

    return true;
}

bool LoadGenesisBlock(const CChainParams& chainparams)
{
    return g_chainstate.LoadGenesisBlock(chainparams);
}

bool LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, CDiskBlockPos *dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    fAddressIndex = gArgs.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX);
    fTimestampIndex = gArgs.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX);
    fSpentIndex = gArgs.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX);

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SERIALIZED_SIZE, MAX_BLOCK_SERIALIZED_SIZE+8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[CMessageHeader::MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> buf;
                if (memcmp(buf, chainparams.MessageStart(), CMessageHeader::MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SERIALIZED_SIZE)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
                CBlock& block = *pblock;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                uint256 hash = block.GetHash();
                {
                    LOCK(cs_main);
                    // detect out of order blocks, and store them for later
                    if (hash != chainparams.GetConsensus().hashGenesisBlock && !LookupBlockIndex(block.hashPrevBlock)) {
                        LogPrint(BCLog::REINDEX, "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                                block.hashPrevBlock.ToString());
                        if (dbp)
                            mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                        continue;
                    }

                    // process in case the block isn't known yet
                    CBlockIndex* pindex = LookupBlockIndex(hash);
                    if (!pindex || (pindex->nStatus & BLOCK_HAVE_DATA) == 0) {
                      CValidationState state;
                      if (g_chainstate.AcceptBlock(pblock, state, chainparams, nullptr, true, dbp, nullptr)) {
                          nLoaded++;
                      }
                      if (state.IsError()) {
                          break;
                      }
                    } else if (hash != chainparams.GetConsensus().hashGenesisBlock && pindex->nHeight % 1000 == 0) {
                      LogPrint(BCLog::REINDEX, "Block Import: already had block %s at height %d\n", hash.ToString(), pindex->nHeight);
                    }
                }

                // Activate the genesis block so normal node progress can continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(state, chainparams)) {
                        break;
                    }
                }

                NotifyHeaderTip();

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        std::shared_ptr<CBlock> pblockrecursive = std::make_shared<CBlock>();
                        if (ReadBlockFromDisk(*pblockrecursive, it->second, chainparams.GetConsensus()))
                        {
                            LogPrint(BCLog::REINDEX, "%s: Processing out of order child %s of %s\n", __func__, pblockrecursive->GetHash().ToString(),
                                    head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (g_chainstate.AcceptBlock(pblockrecursive, dummy, chainparams, nullptr, true, &it->second, nullptr))
                            {
                                nLoaded++;
                                queue.push_back(pblockrecursive->GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void CChainState::CheckBlockIndex(const Consensus::Params& consensusParams)
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*,CBlockIndex*> forward;
    for (auto& entry : mapBlockIndex) {
        forward.insert(std::make_pair(entry.second->pprev, entry.second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(nullptr);
    CBlockIndex *pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent nullptr.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = nullptr; // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = nullptr; // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNeverProcessed = nullptr; // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex* pindexFirstNotTreeValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotTransactionsValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != nullptr) {
        nNodes++;
        if (pindexFirstInvalid == nullptr && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == nullptr && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindexFirstNeverProcessed == nullptr && pindex->nTx == 0) pindexFirstNeverProcessed = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotTreeValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotTransactionsValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) pindexFirstNotTransactionsValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotChainValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotScriptsValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == nullptr) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis()); // The current active chain's genesis block must be this block.
        }
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId <= 0);  // nSequenceId can't be set positive for blocks that aren't linked (negative is used for preciousblock)
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or not pruning has occurred).
        // HAVE_DATA is only equivalent to nTx > 0 (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) assert(pindex->nStatus & BLOCK_HAVE_DATA);
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0)); // This is pruning-independent.
        // All parents having had data (at some point) is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstNeverProcessed != nullptr) == (pindex->nChainTx == 0)); // nChainTx != 0 is used to signal that all parent blocks have been processed (but may have been pruned).
        assert((pindexFirstNotTransactionsValid != nullptr) == (pindex->nChainTx == 0));
        assert(pindex->nHeight == nHeight); // nHeight must be consistent.
        assert(pindex->pprev == nullptr || pindex->nChainWork >= pindex->pprev->nChainWork); // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight))); // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == nullptr); // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == nullptr); // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == nullptr); // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == nullptr); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == nullptr) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstNeverProcessed == nullptr) {
            if (pindexFirstInvalid == nullptr) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates.  chainActive.Tip() must also be there
                // even if some data has been pruned.
                if (pindexFirstMissing == nullptr || pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block was in
                // setBlockIndexCandidates but had to be removed because of the missing data.
                // In this case it must be in mapBlocksUnlinked -- see test below.
            }
        } else { // If this block sorts worse than the current tip or some ancestor's block has never been seen, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed != nullptr && pindexFirstInvalid == nullptr) {
            // If this block has block data available, some parent was never received, and has no invalid parents, it must be in mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) assert(!foundInUnlinked); // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
        if (pindexFirstMissing == nullptr) assert(!foundInUnlinked); // We aren't missing data for any parent -- cannot be in mapBlocksUnlinked.
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed == nullptr && pindexFirstMissing != nullptr) {
            // We HAVE_DATA for this block, have received data for all parents at some point, but we're currently missing data for some parent.
            assert(fHavePruned); // We must have pruned.
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == nullptr) {
                    assert(foundInUnlinked);
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = nullptr;
            if (pindex == pindexFirstMissing) pindexFirstMissing = nullptr;
            if (pindex == pindexFirstNeverProcessed) pindexFirstNeverProcessed = nullptr;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = nullptr;
            if (pindex == pindexFirstNotTransactionsValid) pindexFirstNotTransactionsValid = nullptr;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = nullptr;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = nullptr;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, FormatISO8601Date(nTimeFirst), FormatISO8601Date(nTimeLast));
}

CBlockFileInfo* GetBlockFileInfo(size_t n)
{
    LOCK(cs_LastBlockFile);

    return &vinfoBlockFile.at(n);
}

ThresholdState VersionBitsTipState(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

BIP9Stats VersionBitsTipStatistics(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsStatistics(chainActive.Tip(), params, pos);
}

int VersionBitsTipStateSinceHeight(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsStateSinceHeight(chainActive.Tip(), params, pos, versionbitscache);
}

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

bool LoadMempool(void)
{
    const CChainParams& chainparams = Params();
    int64_t nExpiryTimeout = gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60;
    FILE* filestr = fsbridge::fopen(GetDataDir() / "mempool.dat", "rb");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t expired = 0;
    int64_t failed = 0;
    int64_t already_there = 0;
    int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }
        uint64_t num;
        file >> num;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            CAmount amountdelta = nFeeDelta;
            if (amountdelta) {
                mempool.PrioritiseTransaction(tx->GetHash(), amountdelta);
            }
            CValidationState state;
            if (nTime + nExpiryTimeout > nNow) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(chainparams, mempool, state, tx, nullptr /* pfMissingInputs */, nTime,
                                           nullptr /* plTxnReplaced */, false /* bypass_limits */, 0 /* nAbsurdFee */,
                                           false /* test_accept */, false /* ignore_locks */);
                if (state.IsValid()) {
                    ++count;
                } else {
                    // mempool may contain the transaction already, e.g. from
                    // wallet(s) having loaded it while we were processing
                    // mempool transactions; consider these as valid, instead of
                    // failed, but mark them as 'already there'
                    if (mempool.exists(tx->GetHash())) {
                        ++already_there;
                    } else {
                        ++failed;
                    }
                }
            } else {
                ++expired;
            }
            if (ShutdownRequested())
                return false;
        }
        std::map<uint256, CAmount> mapDeltas;
        file >> mapDeltas;

        for (const auto& i : mapDeltas) {
            mempool.PrioritiseTransaction(i.first, i.second);
        }
    } catch (const std::exception& e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i succeeded, %i failed, %i expired, %i already there\n", count, failed, expired, already_there);
    return true;
}

bool DumpMempool(void)
{
    int64_t start = GetTimeMicros();

    std::map<uint256, CAmount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    {
        LOCK(mempool.cs);
        for (const auto &i : mempool.mapDeltas) {
            mapDeltas[i.first] = i.second;
        }
        vinfo = mempool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE* filestr = fsbridge::fopen(GetDataDir() / "mempool.dat.new", "wb");
        if (!filestr) {
            return false;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)vinfo.size();
        for (const auto& i : vinfo) {
            file << *(i.tx);
            file << (int64_t)i.nTime;
            file << (int64_t)i.nFeeDelta;
            mapDeltas.erase(i.tx->GetHash());
        }
        file << mapDeltas;
        if (!FileCommit(file.Get()))
            throw std::runtime_error("FileCommit failed");
        file.fclose();
        RenameOver(GetDataDir() / "mempool.dat.new", GetDataDir() / "mempool.dat");
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n", (mid-start)*MICRO, (last-mid)*MICRO);
    } catch (const std::exception& e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
        return false;
    }
    return true;
}

//! Guess how far we are in the verification process at the given block index
//! require cs_main if pindex has not been validated yet (because nChainTx might be unset)
double GuessVerificationProgress(const ChainTxData& data, const CBlockIndex *pindex) {
    if (pindex == nullptr)
        return 0.0;

    int64_t nNow = time(nullptr);

    double fTxTotal;

    if (pindex->nChainTx <= data.nTxCount) {
        fTxTotal = data.nTxCount + (nNow - data.nTime) * data.dTxRate;
    } else {
        fTxTotal = pindex->nChainTx + (nNow - pindex->GetBlockTime()) * data.dTxRate;
    }

    return pindex->nChainTx / fTxTotal;
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
    }
} instance_of_cmaincleanup;


bool CoinStakeCache::GetCoinStake(const uint256 &blockHash, CTransactionRef &tx)
{

    for (const auto &i : lData)
    {
        if (blockHash != i.first)
            continue;
        tx = i.second;
        return true;
    };

    BlockMap::iterator mi = mapBlockIndex.find(blockHash);
    if (mi == mapBlockIndex.end())
        return false;

    CBlockIndex *pindex = mi->second;
    if (ReadTransactionFromDiskBlock(pindex, 0, tx))
        return InsertCoinStake(blockHash, tx);

    return false;
};

bool CoinStakeCache::InsertCoinStake(const uint256 &blockHash, const CTransactionRef &tx)
{
    lData.emplace_front(blockHash, tx);

    while (lData.size() > nMaxSize)
        lData.pop_back();

    return true;
};

