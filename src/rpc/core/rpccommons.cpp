// Copyright (c) 2017-2019 The WaykiChain Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php

#include "rpccommons.h"

#include "entities/key.h"
#include "main.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#include "init.h"
#include "rpcserver.h"

Object SubmitTx(CUserID &userId, CBaseTx &tx) {
    uint64_t minFee = GetTxMinFee(tx.nTxType, chainActive.Height());
    if (tx.llFees == 0) {
        tx.llFees = minFee;
    } else if (tx.llFees < minFee) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Tx fee given is too small: %d < %d",
                            tx.llFees, minFee));
    }

    CAccount account;
    if (pCdMan->pAccountCache->GetAccount(userId, account) && account.IsRegistered()) {
        uint64_t balance = account.GetFreeBcoins();
        if (balance < tx.llFees) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Account balance is insufficient");
        }
    } else {
        throw JSONRPCError(RPC_WALLET_ERROR, "Account is unregistered");
    }

    CKeyID keyId;
    if (!pCdMan->pAccountCache->GetKeyId(userId, keyId)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to acquire key id");
    }

    CRegID regId;
    pCdMan->pAccountCache->GetRegId(userId, regId);
    tx.txUid = regId;

    assert(pWalletMain != nullptr);
    {
        EnsureWalletIsUnlocked();
        if (!pWalletMain->HaveKey(keyId)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Sender address not found in wallet");
        }
        if (!pWalletMain->Sign(keyId, tx.ComputeSignatureHash(), tx.signature)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Sign failed");
        }

        std::tuple<bool, string> ret = pWalletMain->CommitTx((CBaseTx*)&tx);
        if (!std::get<0>(ret)) {
            throw JSONRPCError(RPC_WALLET_ERROR, std::get<1>(ret));
        }

        Object obj;
        obj.push_back(Pair("txid", std::get<1>(ret)));
        return obj;
    }
}

string RegIDToAddress(CUserID &userId) {
    CKeyID keyId;
    if (pCdMan->pAccountCache->GetKeyId(userId, keyId))
        return keyId.ToAddress();

    return "cannot get address from given RegId";
}

bool GetKeyId(const string &addr, CKeyID &keyId) {
    if (!CRegID::GetKeyId(addr, keyId)) {
        keyId = CKeyID(addr);
        if (keyId.IsEmpty())
            return false;
    }

    return true;
}

Object GetTxDetailJSON(const uint256& txid) {
    Object obj;
    std::shared_ptr<CBaseTx> pBaseTx;
    {
        LOCK(cs_main);
        CBlock genesisblock;
        CBlockIndex* pGenesisBlockIndex = mapBlockIndex[SysCfg().GetGenesisBlockHash()];
        ReadBlockFromDisk(pGenesisBlockIndex, genesisblock);
        assert(genesisblock.GetMerkleRootHash() == genesisblock.BuildMerkleTree());
        for (unsigned int i = 0; i < genesisblock.vptx.size(); ++i) {
            if (txid == genesisblock.GetTxid(i)) {
                obj = genesisblock.vptx[i]->ToJson(*pCdMan->pAccountCache);
                obj.push_back(Pair("block_hash", SysCfg().GetGenesisBlockHash().GetHex()));
                obj.push_back(Pair("confirmed_height", (int) 0));
                obj.push_back(Pair("confirmed_time", (int) genesisblock.GetTime()));
                CDataStream ds(SER_DISK, CLIENT_VERSION);
                ds << genesisblock.vptx[i];
                obj.push_back(Pair("rawtx", HexStr(ds.begin(), ds.end())));
                return obj;
            }
        }

        if (SysCfg().IsTxIndex()) {
            CDiskTxPos postx;
            if (pCdMan->pContractCache->ReadTxIndex(txid, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file, postx.nTxOffset, SEEK_CUR);
                    file >> pBaseTx;
                    obj = pBaseTx->ToJson(*pCdMan->pAccountCache);
                    obj.push_back(Pair("confirmed_height", (int) header.GetHeight()));
                    obj.push_back(Pair("confirmed_time", (int) header.GetTime()));
                    obj.push_back(Pair("block_hash", header.GetHash().GetHex()));

                    if (pBaseTx->nTxType == CONTRACT_INVOKE_TX) {
                        vector<CVmOperate> vOutput;
                        pCdMan->pContractCache->GetTxOutput(pBaseTx->GetHash(), vOutput);
                        Array outputArray;
                        for (auto& item : vOutput) {
                            outputArray.push_back(item.ToJson());
                        }
                        obj.push_back(Pair("listOutput", outputArray));
                    }
                    CDataStream ds(SER_DISK, CLIENT_VERSION);
                    ds << pBaseTx;
                    obj.push_back(Pair("rawtx", HexStr(ds.begin(), ds.end())));
                } catch (std::exception &e) {
                    throw runtime_error(tfm::format("%s : Deserialize or I/O error - %s", __func__, e.what()).c_str());
                }
                return obj;
            }
        }
        {
            pBaseTx = mempool.Lookup(txid);
            if (pBaseTx.get()) {
                obj = pBaseTx->ToJson(*pCdMan->pAccountCache);
                CDataStream ds(SER_DISK, CLIENT_VERSION);
                ds << pBaseTx;
                obj.push_back(Pair("rawtx", HexStr(ds.begin(), ds.end())));
                return obj;
            }
        }
    }
    return obj;
}

Array GetTxAddressDetail(std::shared_ptr<CBaseTx> pBaseTx) {
    Array arrayDetail;
    Object obj;
    std::set<CKeyID> vKeyIdSet;
    auto spCW = std::make_shared<CCacheWrapper>(pCdMan);

    double dAmount = static_cast<double>(pBaseTx->GetValues()[CoinType::WICC]) / COIN;
    switch (pBaseTx->nTxType) {
        case BLOCK_REWARD_TX: {
            if (!pBaseTx->GetInvolvedKeyIds(*spCW, vKeyIdSet))
                return arrayDetail;

            obj.push_back(Pair("address", vKeyIdSet.begin()->ToAddress()));
            obj.push_back(Pair("category", "receive"));
            obj.push_back(Pair("amount", dAmount));
            obj.push_back(Pair("tx_type", "BLOCK_REWARD_TX"));
            arrayDetail.push_back(obj);

            break;
        }
        case ACCOUNT_REGISTER_TX: {
            if (!pBaseTx->GetInvolvedKeyIds(*spCW, vKeyIdSet))
                return arrayDetail;

            obj.push_back(Pair("address", vKeyIdSet.begin()->ToAddress()));
            obj.push_back(Pair("category", "send"));
            obj.push_back(Pair("amount", dAmount));
            obj.push_back(Pair("tx_type", "ACCOUNT_REGISTER_TX"));
            arrayDetail.push_back(obj);

            break;
        }
        case BCOIN_TRANSFER_TX: {
            CBaseCoinTransferTx* ptx = (CBaseCoinTransferTx*)pBaseTx.get();
            CKeyID sendKeyID;
            if (ptx->txUid.type() == typeid(CPubKey)) {
                sendKeyID = ptx->txUid.get<CPubKey>().GetKeyId();
            } else if (ptx->txUid.type() == typeid(CRegID)) {
                sendKeyID = ptx->txUid.get<CRegID>().GetKeyId(*pCdMan->pAccountCache);
            }

            CKeyID recvKeyId;
            if (ptx->toUid.type() == typeid(CKeyID)) {
                recvKeyId = ptx->toUid.get<CKeyID>();
            } else if (ptx->toUid.type() == typeid(CRegID)) {
                CRegID desRegID = ptx->toUid.get<CRegID>();
                recvKeyId       = desRegID.GetKeyId(*pCdMan->pAccountCache);
            }

            obj.push_back(Pair("tx_type", "BCOIN_TRANSFER_TX"));
            obj.push_back(Pair("from_address", sendKeyID.ToAddress()));
            obj.push_back(Pair("to_address", recvKeyId.ToAddress()));
            obj.push_back(Pair("transfer_amount", dAmount));
            obj.push_back(Pair("memo", HexStr(ptx->memo)));
            arrayDetail.push_back(obj);

            break;
        }
        case CONTRACT_INVOKE_TX: {
            CContractInvokeTx* ptx = (CContractInvokeTx*)pBaseTx.get();
            CKeyID sendKeyID;
            if (ptx->txUid.type() == typeid(CPubKey)) {
                sendKeyID = ptx->txUid.get<CPubKey>().GetKeyId();
            } else if (ptx->txUid.type() == typeid(CRegID)) {
                sendKeyID = ptx->txUid.get<CRegID>().GetKeyId(*pCdMan->pAccountCache);
            }

            CKeyID recvKeyId;
            if (ptx->appUid.type() == typeid(CRegID)) {
                CRegID appUid = ptx->appUid.get<CRegID>();
                recvKeyId     = appUid.GetKeyId(*pCdMan->pAccountCache);
            }

            obj.push_back(Pair("tx_type", "CONTRACT_INVOKE_TX"));
            obj.push_back(Pair("from_address", sendKeyID.ToAddress()));
            obj.push_back(Pair("to_address", recvKeyId.ToAddress()));
            obj.push_back(Pair("arguments", HexStr(ptx->arguments)));
            obj.push_back(Pair("transfer_amount", dAmount));
            arrayDetail.push_back(obj);

            vector<CVmOperate> vOutput;
            pCdMan->pContractCache->GetTxOutput(pBaseTx->GetHash(), vOutput);
            Array outputArray;
            for (auto& item : vOutput) {
                Object objOutPut;
                string address;
                if (item.accountType == ACCOUNT_TYPE::regid) {
                    vector<unsigned char> vRegId(item.accountId, item.accountId + 6);
                    CRegID regId(vRegId);
                    CUserID userId(regId);
                    address = RegIDToAddress(userId);
                } else if (item.accountType == base58addr) {
                    address.assign(item.accountId[0], sizeof(item.accountId));
                }

                objOutPut.push_back(Pair("address", address));

                uint64_t amount;
                memcpy(&amount, item.money, sizeof(item.money));
                double dAmount = amount / COIN;

                if (item.opType == ADD_BCOIN) {
                    objOutPut.push_back(Pair("category", "receive"));
                    objOutPut.push_back(Pair("amount", dAmount));
                } else if (item.opType == MINUS_BCOIN) {
                    objOutPut.push_back(Pair("category", "send"));
                    objOutPut.push_back(Pair("amount", -dAmount));
                }

                if (item.timeoutHeight > 0)
                    objOutPut.push_back(Pair("freeze_height", (int)item.timeoutHeight));

                arrayDetail.push_back(objOutPut);
            }

            break;
        }
        case CONTRACT_DEPLOY_TX:
        case DELEGATE_VOTE_TX: {

            if (!pBaseTx->GetInvolvedKeyIds(*spCW, vKeyIdSet))
                return arrayDetail;

            double dAmount = static_cast<double>(pBaseTx->GetValues()[CoinType::WICC]) / COIN;

            obj.push_back(Pair("from_address", vKeyIdSet.begin()->ToAddress()));
            obj.push_back(Pair("category", "send"));
            obj.push_back(Pair("transfer_amount", dAmount));

            if (pBaseTx->nTxType == CONTRACT_DEPLOY_TX)
                obj.push_back(Pair("tx_type", "CONTRACT_DEPLOY_TX"));
            else if (pBaseTx->nTxType == DELEGATE_VOTE_TX)
                obj.push_back(Pair("tx_type", "DELEGATE_VOTE_TX"));

            arrayDetail.push_back(obj);

            break;
        }
        case COMMON_MTX: {
            CMulsigTx* ptx = (CMulsigTx*)pBaseTx.get();

            CAccount account;
            set<CPubKey> pubKeys;
            for (const auto& item : ptx->signaturePairs) {
                if (!pCdMan->pAccountCache->GetAccount(item.regId, account))
                    return arrayDetail;

                pubKeys.insert(account.pubKey);
            }

            CMulsigScript script;
            script.SetMultisig(ptx->required, pubKeys);
            CKeyID sendKeyId = script.GetID();

            CKeyID recvKeyId;
            if (ptx->desUserId.type() == typeid(CKeyID)) {
                recvKeyId = ptx->desUserId.get<CKeyID>();
            } else if (ptx->desUserId.type() == typeid(CRegID)) {
                CRegID desRegID = ptx->desUserId.get<CRegID>();
                recvKeyId       = desRegID.GetKeyId(*pCdMan->pAccountCache);
            }

            obj.push_back(Pair("tx_type", "COMMON_MTX"));
            obj.push_back(Pair("from_address", sendKeyId.ToAddress()));
            obj.push_back(Pair("to_address", recvKeyId.ToAddress()));
            obj.push_back(Pair("transfer_amount", dAmount));
            obj.push_back(Pair("memo", HexStr(ptx->memo)));

            arrayDetail.push_back(obj);
            break;
        }
        //TODO: other Tx types
        case CDP_STAKE_TX:
        case CDP_REDEEMP_TX:
        case CDP_LIQUIDATE_TX:
        case PRICE_FEED_TX:
        case FCOIN_STAKE_TX:
        case DEX_SETTLE_TX:
        case DEX_CANCEL_ORDER_TX:
        case DEX_BUY_LIMIT_ORDER_TX:
        case DEX_SELL_LIMIT_ORDER_TX:
        case DEX_BUY_MARKET_ORDER_TX:
        case DEX_SELL_MARKET_ORDER_TX:
        default:
            break;
    }
    return arrayDetail;
}
