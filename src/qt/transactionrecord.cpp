// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionrecord.h>

#include <chain.h>
#include <chainparams.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <names/applications.h>
#include <script/names.h>
#include <wallet/types.h>

#include <stdint.h>

#include <QDateTime>

using wallet::ISMINE_NO;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    bool involvesWatchAddress = false;
    isminetype fAllFromMe = ISMINE_SPENDABLE;
    bool any_from_me = false;
    if (wtx.is_coinbase) {
        fAllFromMe = ISMINE_NO;
    } else {
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
            if (mine) any_from_me = true;
        }
    }

    if (fAllFromMe || !any_from_me) {
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
        }

        std::optional<CNameScript> nNameCredit = wtx.name_credit;
        std::optional<CNameScript> nNameDebit = wtx.name_debit;

        TransactionRecord nameSub(hash, nTime, TransactionRecord::NameOp, "", 0, 0);

        if(nNameCredit)
        {
            // TODO: Use friendly names based on namespaces
            if(nNameCredit.value().isAnyUpdate())
            {
                if(nNameDebit)
                {
                    if(nNameCredit.value().getNameOp() == OP_NAME_FIRSTUPDATE)
                    {
                        nameSub.nameOpType = TransactionRecord::NameOpType::FirstUpdate;
                    }
                    else
                    {
                        // OP_NAME_UPDATE

                        // Check if renewal (previous value is unchanged)
                        if(nNameDebit.value().isAnyUpdate() && nNameDebit.value().getOpValue() == nNameCredit.value().getOpValue())
                        {
                            nameSub.nameOpType = TransactionRecord::NameOpType::Renew;
                        }
                        else
                        {
                            nameSub.nameOpType = TransactionRecord::NameOpType::Update;
                        }
                    }
                }
                else
                {
                    nameSub.nameOpType = TransactionRecord::NameOpType::Recv;
                }

                nameSub.nameNamespace = NamespaceFromName(nNameCredit.value().getOpName());
                nameSub.address = DescFromName(nNameCredit.value().getOpName(), nameSub.nameNamespace);
            }
            else
            {
                nameSub.nameOpType = TransactionRecord::NameOpType::New;
            }
        }
        else if(nNameDebit)
        {
            nameSub.nameOpType = TransactionRecord::NameOpType::Send;

            if(nNameDebit.value().isAnyUpdate())
            {
                nameSub.nameNamespace = NamespaceFromName(nNameDebit.value().getOpName());
                nameSub.address = DescFromName(nNameDebit.value().getOpName(), nameSub.nameNamespace);
            }
        }

        CAmount nTxFee = nDebit - wtx.tx->GetValueOut();

        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CTxOut& txout = wtx.tx->vout[i];

            if (fAllFromMe) {
                // Change is only really possible if we're the sender
                // Otherwise, someone just sent bitcoins to a change address, which should be shown
                if (wtx.txout_is_change[i]) {
                    // Name credits sent to change addresses should not be skipped
                    if (nNameCredit && CNameScript::isNameScript(txout.scriptPubKey)) {
                        nameSub.debit = nNet;
                        nameSub.idx = i;
                        nameSub.involvesWatchAddress = involvesWatchAddress;
                        parts.append(nameSub);
                    }

                    continue;
                }

                //
                // Debit
                //

                TransactionRecord sub(hash, nTime);
                sub.idx = i;
                sub.involvesWatchAddress = involvesWatchAddress;

                if(nNameDebit && CNameScript::isNameScript(txout.scriptPubKey))
                {
                    nameSub.idx = sub.idx;
                    nameSub.involvesWatchAddress = sub.involvesWatchAddress;
                    sub = nameSub;
                }

                if (!std::get_if<CNoDestination>(&wtx.txout_address[i]))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }

                CAmount nValue = txout.nValue;
                if (sub.type == TransactionRecord::NameOp)
                {
                    // 300k is just a "sufficiently high" height
                    nValue -= Params().GetConsensus().rules->MinNameCoinAmount(300000);
                }
                /* Add fee to first output */
                if (nTxFee > 0)
                {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                parts.append(sub);
            }

            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                //
                // Credit
                //

                TransactionRecord sub(hash, nTime);
                sub.idx = i; // vout index
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (wtx.txout_address_is_mine[i])
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                if (wtx.is_coinbase)
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                }

                parts.append(sub);
            }

            if (nNameCredit) {
                nameSub.debit = nNet;
                parts.append(nameSub);
            }
        }
    } else {
        //
        // Mixed debit transaction, can't break down payees
        //
        parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
        parts.last().involvesWatchAddress = involvesWatchAddress;
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    int typesort;
    switch (type) {
    case SendToAddress: case SendToOther:
        typesort = 2; break;
    case RecvWithAddress: case RecvFromOther:
        typesort = 3; break;
    default:
        typesort = 9;
    }
    status.sortKey = strprintf("%010d-%01d-%010u-%03d-%d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx,
        typesort);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.m_cur_block_hash = block_hash;

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated) {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(const uint256& block_hash) const
{
    assert(!block_hash.IsNull());
    return status.m_cur_block_hash != block_hash || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
