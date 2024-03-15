#include <memory>
#include <unordered_map>
#include <future>
#include <utils/contract_utils.h>

#include "transaction_cache.h"
#include "transaction.h"
#include "transaction_entity_V33_1.h"
#include "utils/json.hpp"
#include "db/db_api.h"

#include "ca/txhelper.h"
#include "utils/magic_singleton.h"
#include "algorithm.h"
#include "../utils/time_util.h"
#include "block_helper.h"
#include "utils/account_manager.h"
#include "checker.h"
#include "utils/tfs_bench_mark.h"
#include "common/time_report.h"
#include "utils/console.h"
#include "utils/tmp_log.h"
#include "evmone.h"
#include "block_helper.h"
#include "utils/time_util.h"
#include "common/global_data.h"
#include "../../net/unregister_node.h"
#include <chrono>


class ContractDataCache;

const int TransactionCache::_kBuildInterval = 3 * 1000;
const time_t TransactionCache::_kTxExpireInterval  = 10;
const int TransactionCache::_kBuildThreshold = 1000000;


int CreateBlock(const std::list<CTransaction>& txs, const uint64_t& blockHeight, CBlock& cblock)
{
	cblock.Clear();

	// Fill version
    cblock.set_version(global::ca::kCurrentBlockVersion);

	// Fill time
	uint64_t time = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	cblock.set_time(time);
    DEBUGLOG("block set time ======");

	// Fill height
	uint64_t prevBlockHeight = blockHeight - 1;
	cblock.set_height(blockHeight);

    nlohmann::json storage;
    bool isContractBlock = false;
    
    packDispatch packdis;
	// Fill tx
	for(auto& tx : txs)
	{
		// Add major transaction
		CTransaction * majorTx = cblock.add_txs();
		*majorTx = tx;
		auto& txHash = tx.hash();
        auto txType = (global::ca::TxType)tx.txtype();
        if (txType == global::ca::TxType::kTxTypeCallContract || txType == global::ca::TxType::kTxTypeDeployContract)
        {
            isContractBlock = true;
            nlohmann::json txStorage;
            if (MagicSingleton<TransactionCache>::GetInstance()->GetContractInfoCache(txHash, txStorage) != 0)
            {
                ERRORLOG("can't find storage of tx {}", txHash);
                return -1;
            }
            
            std::set<std::string> dirtyContractList;
            if(!MagicSingleton<TransactionCache>::GetInstance()->GetDirtyContractMap(tx.hash(), dirtyContractList))
            {
                ERRORLOG("GetDirtyContractMap fail!!! txHash:{}", tx.hash());
                return -2;
            }
            txStorage["dependentCTx"] = dirtyContractList;

            storage[txHash] = txStorage;
        }
	}

    cblock.set_data(storage.dump());
    // Fill preblockhash
    uint64_t seekPrehashTime = 0;
    std::future_status status;
    auto futurePrehash = MagicSingleton<BlockStroage>::GetInstance()->GetPrehash(prevBlockHeight);
    if(!futurePrehash.valid())
    {
        ERRORLOG("futurePrehash invalid,hight:{}",prevBlockHeight);
        return -3;
    }
    status = futurePrehash.wait_for(std::chrono::seconds(6));
    if (status == std::future_status::timeout) 
    {
        ERRORLOG("seek prehash timeout, hight:{}",prevBlockHeight);
        return -4;
    }
    else if(status == std::future_status::ready) 
    {
        std::string preBlockHash = futurePrehash.get().first;
        if(preBlockHash.empty())
        {
            ERRORLOG("seek prehash <fail>!!!,hight:{},prehash:{}",prevBlockHeight, preBlockHash);
            return -5;
        }
        DEBUGLOG("seek prehash <success>!!!,hight:{},prehash:{},blockHeight:{}",prevBlockHeight, preBlockHash, blockHeight);
        seekPrehashTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        cblock.set_prevhash(preBlockHash);
    }
    
	// Fill merkleroot
	cblock.set_merkleroot(ca_algorithm::CalcBlockMerkle(cblock));
	// Fill hash
	cblock.set_hash(Getsha256hash(cblock.SerializeAsString()));
    DEBUGLOG("blockHash:{}, \n storage:{}", cblock.hash().substr(0,6), storage.dump(4));
    DEBUGLOG("block hash = {} set time ",cblock.hash());
	return 0;
}

int BuildBlock(const std::list<CTransaction>& txs, const uint64_t& blockHeight, bool build_first, bool isConTractTx = false)
{
	if(txs.empty())
	{
		ERRORLOG("Txs is empty!");
		return -1;
	}

	CBlock cblock;
    auto S = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	int ret = CreateBlock(txs, blockHeight, cblock);
    if(ret != 0)
    {
        if(ret == -3 || ret == -4 || ret == -5)
        {
            MagicSingleton<BlockStroage>::GetInstance()->ForceCommitSeekTask(cblock.height() - 1);
        }
        ERRORLOG("Create block failed!");
		return ret - 100;
    }
	auto S1 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	std::string serBlock = cblock.SerializeAsString();

	ca_algorithm::PrintBlock(cblock);

    BlockMsg blockmsg;
    blockmsg.set_version(global::kVersion);
    blockmsg.set_time(MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp());
    blockmsg.set_block(serBlock);

    for(auto &tx : cblock.txs())
    {
        if(GetTransactionType(tx) != kTransactionType_Tx)
        {
            continue;
        }

        CTransaction copyTx = tx;
        copyTx.clear_hash();
        copyTx.clear_verifysign();
        std::string txHash = Getsha256hash(copyTx.SerializeAsString());
        MagicSingleton<TFSbenchmark>::GetInstance()->SetTxHashByBlockHash(cblock.hash(), txHash);

        uint64_t handleTxHeight =  cblock.height() - 1;
        TxHelper::vrfAgentType type = TxHelper::GetVrfAgentType(tx, handleTxHeight);
        if(type == TxHelper::vrfAgentType_defalut || type == TxHelper::vrfAgentType_local)
        {
            continue;
        }

        std::pair<std::string,Vrf> vrfPair;
        if(!MagicSingleton<VRF>::GetInstance()->getVrfInfo(txHash, vrfPair))
        {
            ERRORLOG("getVrfInfo failed! tx hash {}", txHash);
            return -3000;
        }
        Vrf *vrfinfo  = blockmsg.add_vrfinfo();
        vrfinfo ->CopyFrom(vrfPair.second);

        if(!MagicSingleton<VRF>::GetInstance()->getTxVrfInfo(txHash, vrfPair))
        {
            ERRORLOG("getTxVrfInfo failed! tx hash {}", txHash);
            return -4000;
        }

        Vrf *txvrfinfo  = blockmsg.add_txvrfinfo();
        vrfPair.second.mutable_vrfdata()->set_txvrfinfohash(txHash);
        txvrfinfo ->CopyFrom(vrfPair.second);

        // auto vrfJson = nlohmann::json::parse(vrf.second.data());
		// vrfJson["txhash"] = txHash;
        // vrf.second.set_data(vrfJson.dump());
        
        //blockmsg.mutable_txvrfinfomap()->insert({txHash, vrfPair.second});
    }
    
    BlockMsg _cpMsg = blockmsg;
    _cpMsg.clear_block();

    std::string defaultBase58Addr = MagicSingleton<AccountManager>::GetInstance()->GetDefaultBase58Addr();
    std::string _cpMsgHash = Getsha256hash(_cpMsg.SerializeAsString());
	std::string signature;
	std::string pub;
	if (TxHelper::Sign(defaultBase58Addr, _cpMsgHash, signature, pub) != 0)
	{
		return -8;
	}

	CSign * sign = blockmsg.mutable_sign();
	sign->set_sign(signature);
	sign->set_pub(pub);

    auto msg = make_shared<BlockMsg>(blockmsg);
    DEBUGLOG("DoHandleBlock start");
	ret = DoHandleBlock(msg);
    if(ret != 0)
    {
        ERRORLOG("DoHandleBlock failed The error code is {} ,blockHash:{}",ret, cblock.hash().substr(0,6));
        CBlock cblock;
	    if (!cblock.ParseFromString(msg->block()))
	    {
		    ERRORLOG("fail to serialization!!");
		    return -3090;
	    }
        ClearVRF(cblock);
        return ret -4000;
    }

	return 0;
}

TransactionCache::TransactionCache()
{
    _buildTimer.AsyncLoop(
        _kBuildInterval, 
        [=](){ _blockBuilder.notify_one(); }
        );
}

int TransactionCache::AddCache(const CTransaction& transaction, const uint64_t& height, const std::vector<std::string> dirtyContract)
{
    auto txType = (global::ca::TxType)transaction.txtype();
    bool isContractTransaction = txType == global::ca::TxType::kTxTypeCallContract || txType == global::ca::TxType::kTxTypeDeployContract;
    if (isContractTransaction)
    {
//        if(!_HasContractPackingPermission(height, transaction.time()))
//        {
//            DEBUGLOG("_HasContractPackingPermission fail!!!, txHash:{}", transaction.hash().substr(0,6));
//            return -1;
//        }
        std::unique_lock<mutex> locker(_contractCacheMutex);
        if(Checker::CheckConflict(transaction, _contractCache))
        {
            DEBUGLOG("DoubleSpentTransactions, txHash:{}", transaction.hash());
            return -2;
        }
        DEBUGLOG("transaction hash:{}", transaction.hash());
        _contractCache.push_back({transaction, height, false});
    }
    else
    {
        std::unique_lock<mutex> locker(_transactionCacheMutex);
        if(Checker::CheckConflict(transaction, _transactionCache))
        {
            DEBUGLOG("DoubleSpentTransactions, txHash:{}", transaction.hash());
            return -3;
        }
        
        _transactionCache[height].push_back(transaction);
        for(auto txEntity: _transactionCache)
        {
            if (txEntity.second.size() >= _kBuildThreshold)
            {
                _blockBuilder.notify_one();
            }
        }
    }
    return 0;
}

bool TransactionCache::Process()
{
    _transactionCacheBuildThread = std::thread(std::bind(&TransactionCache::_TransactionCacheProcessingFunc, this));
    _transactionCacheBuildThread.detach();
//    _contractCacheBuildThread = std::thread(std::bind(&TransactionCache::_ContractCacheProcessingFunc, this));
//    _contractCacheBuildThread.detach();
    return true;
}

//bool TransactionCache::CheckConflict(const CTransaction& transaction)
//{
//    return Checker::CheckConflict(transaction, _transactionCache);
//}
void TransactionCache::Stop(){
    _threadRun=false;
}

void TransactionCache::_TransactionCacheProcessingFunc()
{
    while (_threadRun)
    {
        std::unique_lock<mutex> locker(_transactionCacheMutex);
        _blockBuilder.wait(locker);
        std::vector<cacheIter> emptyHeightCache;

        std::list<CTransaction> buildTxs;
        if(_transactionCache.empty())
        {
            continue;
        }
        uint64_t blockHeight = _transactionCache.rbegin()->first + 1;
        for(auto& txs : _transactionCache)
        {
            buildTxs.insert(buildTxs.end(), txs.second.begin(), txs.second.end());
        }

        auto ret = BuildBlock(buildTxs, blockHeight, false);
        if(ret != 0)
        {
            ERRORLOG("{} build block fail", ret);
            _transactionCache.clear();
            continue;
            std::cout << "block packaging fail" << std::endl;
        }
        std::cout << "block successfully packaged" << std::endl;
        _transactionCache.clear();
        locker.unlock();
    }
}

//void TransactionCache::_ContractCacheProcessingFunc()
//{
//    uint64_t timeBaseline = 0;
//    while (true)
//    {
//        sleep(1);
//        {
//            std::unique_lock<std::mutex> locker(_contractCacheMutex);
//            if (_contractCache.empty())
//            {
//                continue;
//            }
//        }
//        //FormatUTCTimestamp
//        // 10:06:09
//        // 10:06:00
//        uint64_t currentTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
//        if (timeBaseline == 0)
//        {
//            timeBaseline = (currentTime / 1000000 / _precedingContractBlockLookupInterval) * 1000000 * _precedingContractBlockLookupInterval;
//            DEBUGLOG("current baseline {}", timeBaseline);
//        }
//        auto s1 = MagicSingleton<TimeUtil>::GetInstance()->FormatUTCTimestamp(currentTime);
//        auto s2 = MagicSingleton<TimeUtil>::GetInstance()->FormatUTCTimestamp(timeBaseline);
//        DEBUGLOG("FFF OOOOOOOO, currentTime:{}, timeBaseline:{}", s1, s2);
//        if (currentTime < timeBaseline + _precedingContractBlockLookupStartTime)
//        {
//            continue;
//        }
//        else if (currentTime >= timeBaseline + _precedingContractBlockLookupStartTime && currentTime < timeBaseline + _precedingContractBlockLookupEndtime
//            && _preContractBlockHash.empty())
//        {
//            std::string contractPreHash = _SeekContractPreHash();
//            if (contractPreHash.empty())
//            {
//                DEBUGLOG("FFF 0000000000");
//                continue;
//            }
//            DEBUGLOG("FFF 1111111111");
//            DBReader dbReader;
//            std::string blockRaw;
//            if (DBStatus::DB_SUCCESS != dbReader.GetBlockByBlockHash(contractPreHash, blockRaw))
//            {
//                DEBUGLOG("FFF 22222222");
//                if (SeekBlockByContractPreHashReq(contractPreHash, blockRaw) != 0)
//                {
//                    continue;
//                }
//                CBlock block;
//                if (!block.ParseFromString(blockRaw))
//                {
//                    continue;
//                }
//                CTimer _contractPreBlockWaitTime;
//
//                _contractPreBlockWaitTime.AsyncLoop(1,
//                                                    [this, contractPreHash]()
//                                                    {
//                                                        DBReader dbReader;
//                                                        std::string blockRaw;
//                                                        if (_preContractBlockHash != contractPreHash)
//                                                        {
//                                                            return;
//                                                        }
//                                                        if (DBStatus::DB_SUCCESS == dbReader.GetBlockByBlockHash(contractPreHash, blockRaw))
//                                                        {
//                                                            _contractPreBlockWaiter.notify_one();
//                                                        }
//                                                    }
//                                                    );
//
//                MagicSingleton<BlockHelper>::GetInstance()->AddMissingBlock(block);
//                std::unique_lock<std::mutex> locker(_contractPreHashMutex);
//                _preContractBlockHash = contractPreHash;
//                using namespace std::literals::chrono_literals;
//                _contractPreBlockWaiter.wait_for(locker, 5s);
//            }
//            std::unique_lock<std::mutex> locker(_contractPreHashMutex);
//            _preContractBlockHash = contractPreHash;
//        }
//
//        _ExecuteContracts();
//        currentTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
//
//        auto s3 = MagicSingleton<TimeUtil>::GetInstance()->FormatUTCTimestamp(currentTime);
//        auto s4 = MagicSingleton<TimeUtil>::GetInstance()->FormatUTCTimestamp(timeBaseline + _contractBlockPackingTime);
//        DEBUGLOG("FFF 22222,currentTime:{}, timeBaseline + _contractBlockPackingTime:{}", s3, s4);
//
//        if (currentTime >= timeBaseline + _contractBlockPackingTime)
//        {
//            DEBUGLOG("FFF 333333333");
//            std::list<CTransaction> buildTxs;
//            uint64_t topTransactionHeight = 0;
//            for(const auto& txEntity : _contractCache)
//            {
//                buildTxs.push_back(txEntity.GetTransaction());
//                if (txEntity.GetHeight() > topTransactionHeight)
//                {
//                    topTransactionHeight = txEntity.GetHeight();
//                }
//            }
//            DEBUGLOG("FFF 444444444");
//            auto ret = BuildBlock(buildTxs, topTransactionHeight + 1, false);
//            if(ret != 0)
//            {
//                ERRORLOG("{} build block fail", ret);
//                std::cout << "block packaging fail" << std::endl;
//            }
//            DEBUGLOG("FFF 555555555");
//            MagicSingleton<ContractDataCache>::GetInstance()->clear();
//            std::cout << "block successfully packaged" << std::endl;
//            timeBaseline = 0;
//            std::scoped_lock locker(_contractCacheMutex, _contractInfoCacheMutex, _contractPreHashMutex);
//            _preContractBlockHash.clear();
//            _contractCache.clear();
//            _contractInfoCache.clear();
//            _contractPreHashCache.clear();
//        }
//    }
//}
int TransactionCache::HandleContractPackagerMsg(const std::shared_ptr<ContractPackagerMsg> &msg, const MsgData &msgdata)
{
    std::unique_lock<mutex> locker(_threadMutex);
    DEBUGLOG("111111111111 HHHHHHHHHH");

    auto cSign = msg->sign();
    auto pub = cSign.pub();
    auto signature = cSign.sign();

    ContractPackagerMsg cp_msg = *msg;
    cp_msg.clear_sign();
	std::string message = Getsha256hash(cp_msg.SerializeAsString());
    Account account;
    if(MagicSingleton<AccountManager>::GetInstance()->GetAccountPubByBytes(pub, account) == false){
        ERRORLOG(RED " HandleContractPackagerMsg Get public key from bytes failed!" RESET);
        return -1;
    }
    if(account.Verify(message, signature) == false)
    {
        ERRORLOG(RED "HandleBuildBlockBroadcastMsg Public key verify sign failed!" RESET);
        return -2;
    }

    // todo verify vrf
    Vrf vrfInfo = msg->vrfinfo();
    std::string hash;
    int range;
    uint64_t verifyHeight;

    const VrfData& vrfData = vrfInfo.vrfdata();
	hash = vrfData.hash();
	range = vrfData.range();
    verifyHeight = vrfData.height();

	EVP_PKEY *pkey = nullptr;
	if (!GetEDPubKeyByBytes(vrfInfo.vrfsign().pub(), pkey))
	{
		ERRORLOG(RED "HandleBuildBlockBroadcastMsg Get public key from bytes failed!" RESET);
		return -4;
	}

    std::string contractHash;
    for (const TxMsgReq& txMsg : msg->txmsgreq())
    {
        const TxMsgInfo& txMsgInfo = txMsg.txmsginfo();
        CTransaction transaction;
        if (!transaction.ParseFromString(txMsgInfo.tx()))
        {
            ERRORLOG("Failed to deserialize transaction body!");
            continue;
        }
        contractHash += transaction.hash();
    }
    std::string input = Getsha256hash(contractHash);
    DEBUGLOG("HandleContractPackagerMsg input : {} , hash : {}", input,hash);
	std::string result = hash;
	std::string proof = vrfInfo.vrfsign().sign();
	if (MagicSingleton<VRF>::GetInstance()->VerifyVRF(pkey, input, result, proof) != 0)
	{
		ERRORLOG(RED "HandleBuildBlockBroadcastMsg Verify VRF Info fail" RESET);
		return -5;
	}

    std::vector<Node> _vrfNodelist;
    for(auto & item : msg->vrfdatasource().vrfnodelist())
    {
        Node x;
        x.base58Address = item;
        _vrfNodelist.push_back(x);
    }

    DEBUGLOG("2222222222222 HHHHHHHHHH");

    auto ret = verifyVrfDataSource(_vrfNodelist,verifyHeight);
    if(ret != 0)
    {
        ERRORLOG("verifyVrfDataSource fail ! ,ret:{}", ret);
        return -6;
    }

    Node node;
	if (!MagicSingleton<PeerNode>::GetInstance()->FindNodeByFd(msgdata.fd, node))
	{
		return -7;
	}
    DEBUGLOG("dispatchNodeAddr:{}", node.base58Address);
    double randNum = MagicSingleton<VRF>::GetInstance()->GetRandNum(result);
    std::string defaultAddr = MagicSingleton<AccountManager>::GetInstance()->GetDefaultBase58Addr();
    ret = VerifyContractPackNode(node.base58Address, randNum, defaultAddr,_vrfNodelist);
    if(ret != 0)
    {
        ERRORLOG("VerifyContractPackNode ret  {}", ret);
        return -8;
    }
    DEBUGLOG("333333333333333 HHHHHHHHHH,txSize:{}", msg->txmsgreq().size());
    std::set<CTransaction> ContractTxs;

    packDispatch packdis;
    std::map<std::string, future<int>> txTaskResults;
    for (const TxMsgReq& txMsg : msg->txmsgreq())
    {
        const TxMsgInfo& txMsgInfo = txMsg.txmsginfo();
        CTransaction transaction;
        if (!transaction.ParseFromString(txMsgInfo.tx()))
        {
            ERRORLOG("Failed to deserialize transaction body!");
            continue;
        }

        DEBUGLOG("SetDirtyContractMap, txHash:{}", transaction.hash());
        SetDirtyContractMap(transaction.hash(), {txMsgInfo.contractstoragelist().begin(), txMsgInfo.contractstoragelist().end()});


        std::vector<std::string> dependentAddress(txMsgInfo.contractstoragelist().begin(), txMsgInfo.contractstoragelist().end());
        packdis.Add(transaction.hash(),dependentAddress,transaction);

        auto task = std::make_shared<std::packaged_task<int()>>(
                [txMsg, txMsgInfo, transaction] {
//                    const CSign dispatcherSign = txMsgInfo ->sign();
//                    bool isMultiSign = IsMultiSign(transaction);
//                    Base58Ver ver = isMultiSign ? Base58Ver::kBase58Ver_MultiSign : Base58Ver::kBase58Ver_Normal;
                    std::string dispatcherAddr = transaction.identity();
                    if(!HasContractPackingPermission(dispatcherAddr, txMsgInfo.height(), transaction.time()))
                    {
                        ERRORLOG("HasContractPackingPermission fail!!!, txHash:{}", transaction.hash().substr(0,6));
                        return -1;
                    }

                    int ret = DoHandleTx(std::make_shared<TxMsgReq>(txMsg), *std::make_unique<CTransaction>());
                    if (ret != 0)
                    {
                        ERRORLOG("DoHandleTx fail ret: {}, tx hash : {}", ret, transaction.hash());
                        return ret;
                    }
                    return 0;
                });
        try
        {
            txTaskResults[transaction.hash()] = task->get_future();
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        MagicSingleton<TaskPool>::GetInstance()->CommitWorkTask([task](){(*task)();});
    }

    DEBUGLOG("3333333333333444 HHHHHHHHHH");

    std::map<uint32, future<std::pair<int, std::string>>> dependentContractTxTaskResults;
    std::vector<future<std::pair<int, std::string>>> nondependentContractTxTaskResults;

    std::map<uint32_t, std::map<std::string, CTransaction>> dependentContractTxMap;
    std::map<std::string, CTransaction> nondependentContractTxMap;
    packdis.GetDependentData(dependentContractTxMap, nondependentContractTxMap);
    DEBUGLOG("44444444444 HHHHHHHHHH");
    auto processDependentContract = [&dependentContractTxTaskResults](const uint32_t N, std::map<std::string, CTransaction> ContractTxs) {
        auto task = std::make_shared<std::packaged_task<std::pair<int, std::string>()>>(
            [ContractTxs] {
                return MagicSingleton<TransactionCache>::GetInstance()->_ExecuteContracts(ContractTxs);
            });

        if (task)
            dependentContractTxTaskResults[N] =task->get_future();
        else
            return -10;

        MagicSingleton<TaskPool>::GetInstance()->CommitTxTask([task]() { (*task)(); });

        return 0; 
    };

    for(const auto& iter : dependentContractTxMap)
    {
        DEBUGLOG("dependentContractTxMap HHHHHHHHHH first:{}, second size:{}", iter.first, iter.second.size());
        processDependentContract(iter.first, iter.second);
    }
    DEBUGLOG("555555555555 HHHHHHHHHH");

    DEBUGLOG("nondependentContractTxMap HHHHHHHHHH size:{}", nondependentContractTxMap.size());
    for(const auto& iter : nondependentContractTxMap)
    {
        std::map<std::string, CTransaction> ContractTxs;
        ContractTxs.emplace(iter.first, iter.second);
        
        auto task = std::make_shared<std::packaged_task<std::pair<int, std::string>()>>(
        [ContractTxs]
        { 
            return MagicSingleton<TransactionCache>::GetInstance()->_ExecuteContracts(ContractTxs);
        });

        if(task)    nondependentContractTxTaskResults.push_back(task->get_future());
        else    return -10;

        MagicSingleton<TaskPool>::GetInstance()->CommitBlockTask([task](){(*task)();});
    }
    DEBUGLOG("66666666666 HHHHHHHHHH");
    std::map<std::string, int> txRes;
    for (auto& res : txTaskResults)
    {
        if (res.second.valid()) 
        {
            auto retPair = res.second.get();
            txRes[res.first] = retPair;
        }
    }
    DEBUGLOG("77777777777 HHHHHHHHHH");

    std::set<uint32_t> failDependent;
    for (auto& res : txRes)
    {
        if(res.second != 0)
        {
            ERRORLOG("failDependent txHash:{}, ret:{}", res.first, res.second);
            for (auto iter = dependentContractTxMap.begin(); iter != dependentContractTxMap.end();) 
            {
                if (iter->second.find(res.first) != iter->second.end()) 
                {
                    RemoveContractInfoCacheTransaction(iter->second);
                    failDependent.insert(iter->first);
                    ERRORLOG("verify tx fail !!! delete contract tx:{}", res.first);
                    iter->second.erase(res.first);
                }

                if (iter->second.empty()) 
                {
                    iter = dependentContractTxMap.erase(iter);
                } 
                else 
                {
                    ++iter;
                }
            }
            if(nondependentContractTxMap.find(res.first) != nondependentContractTxMap.end())
            {
                std::map<std::string, CTransaction> contractTxs;
                contractTxs[res.first] = {};
                RemoveContractInfoCacheTransaction(contractTxs);
            }
        }
    }

    for(const auto& iter : failDependent)
    {
        dependentContractTxTaskResults.erase(iter);
        processDependentContract(iter, dependentContractTxMap[iter]);
    }

    std::map<std::string, int> depenDentCTxRes;
    for (auto& res : dependentContractTxTaskResults)
    {
        if (res.second.valid()) 
        {
            auto retPair = res.second.get();
            depenDentCTxRes[retPair.second] = retPair.first;
        }
    }
    DEBUGLOG("888888888888 HHHHHHHHHH");

    std::map<std::string, int> nondepenDentCTxRes;
    for (auto& res : nondependentContractTxTaskResults)
    {
        if(res.valid())
        {
            auto retPair = res.get();
            nondepenDentCTxRes[retPair.second] = retPair.first;
        }
        
    }
    DEBUGLOG("999999999999 HHHHHHHHHH");

    for (auto& res : depenDentCTxRes)
    {
        if(res.second != 0)
        {
            ERRORLOG("faildepenDentCTxRes txHash:{}, ret:{}", res.first, res.second);
            for(const auto& iter : dependentContractTxMap)
            {
                if(iter.second.find(res.first) != iter.second.end())
                {
                    RemoveContractsCacheTransaction(iter.second);
                }
            }
            ERRORLOG("_ExecuteDependentContractTx fail!!!, txHash:{}", res.first);
        }
    }

    for (auto& res : nondepenDentCTxRes)
    {
        if(res.second != 0)
        {
            ERRORLOG("nondepenDentCTxRes txHash:{}, ret:{}", res.first, res.second);
            std::map<std::string, CTransaction> contractTxs;
            contractTxs[res.first] = {};
            RemoveContractsCacheTransaction(contractTxs);
            ERRORLOG("_ExecuteNonDependentContractTx fail!!!, txHash:{}", res.first);
        }
    }
    
    DEBUGLOG("1010101010101 HHHHHHHHHH");
    ProcessContract();
    DEBUGLOG("Packager HandleContractPackagerMsg 005 ");
    return 0;
}

void TransactionCache::ProcessContract()
{
    std::scoped_lock locker(_contractCacheMutex, _contractInfoCacheMutex, _dirtyContractMapMutex);
    //_ExecuteContracts();
    std::list<CTransaction> buildTxs;
    uint64_t topTransactionHeight = 0;
    for(const auto& txEntity : _contractCache)
    {
        buildTxs.push_back(txEntity.GetTransaction());
        if (txEntity.GetHeight() > topTransactionHeight)
        {
            topTransactionHeight = txEntity.GetHeight();
        }
    }
    DBReader dbReader;
    uint64_t top = 0;
    if (DBStatus::DB_SUCCESS != dbReader.GetBlockTop(top))
    {
        ERRORLOG("GetBlockTop error!");
        std::cout << "block packaging fail" << std::endl;
        return;
    }
    if (top > topTransactionHeight)
    {
        MagicSingleton<BlockStroage>::GetInstance()->CommitSeekTask(top);
        topTransactionHeight = top;
        DEBUGLOG("top:{} > topTransactionHeight:{}", top, topTransactionHeight);
    }
    if (buildTxs.empty())
    {
        DEBUGLOG("buildTxs.empty()");
        return;
    }

    ON_SCOPE_EXIT{
        removeExpiredEntriesFromDirtyContractMap();
        _contractCache.clear();
        _contractInfoCache.clear();
    };

    DEBUGLOG("_GetContractTxPreHash start");
    std::list<std::pair<std::string, std::string>> contractTxPreHashList;
    if(_GetContractTxPreHash(buildTxs,contractTxPreHashList) != 0)
    {
        ERRORLOG("_GetContractTxPreHash fail");
        return;
    }   
    DEBUGLOG("_GetContractTxPreHash end");

    if(contractTxPreHashList.empty())
    {
        DEBUGLOG("contractTxPreHashList empty");
    }
    else
    {
        auto ret = _newSeekContractPreHash(contractTxPreHashList);
        if ( ret != 0)
        {
            ERRORLOG("{} _newSeekContractPreHash fail", ret);
            return;
        }
    }
    DEBUGLOG("BuildBlock start");
    auto ret = BuildBlock(buildTxs, topTransactionHeight + 1, false,true);
    if(ret != 0)
    {
        ERRORLOG("{} build block fail", ret);
        std::cout << "block packaging fail" << std::endl;
    }
    else
    {
        std::cout << "block successfully packaged" << std::endl;
    }
    DEBUGLOG("FFF 555555555");
    return;
}

std::pair<int, std::string> TransactionCache::_ExecuteContracts(const std::map<std::string, CTransaction>& dependentContractTxMap)
{
    uint64_t StartTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    ContractDataCache contractDataCache;
    std::map<std::string, std::string> contractPreHashCache;
    for (auto iterPair : dependentContractTxMap)
    {
        uint64_t StartTime1 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        auto& tx = iterPair.second;
        auto txType = (global::ca::TxType)tx.txtype();
        if ( (txType != global::ca::TxType::kTxTypeCallContract && txType != global::ca::TxType::kTxTypeDeployContract)
            || _AddContractInfoCache(tx, contractPreHashCache, &contractDataCache) != 0)
        {
            //_contractCache.erase(iter)
            return {-1,tx.hash()};
        }
        uint64_t StartTime2 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        DEBUGLOG("FFF _ExecuteContracts HHHHHHHHHH txHash:{}, time:{}", tx.hash().substr(0,6), (StartTime2 - StartTime1) / 1000000.0);
    }
    uint64_t EndTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    DEBUGLOG("FFF _ExecuteContracts HHHHHHHHHH txSize:{}, Time:{}",dependentContractTxMap.size(), (EndTime - StartTime) / 1000000.0);
    return {0,""};
}

int TransactionCache::_GetContractTxPreHash(const std::list<CTransaction>& txs, std::list<std::pair<std::string, std::string>>& contractTxPreHashList)
{
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> contractTxPreHashMap;
    for(auto& tx : txs)
	{
        if (global::ca::TxType::kTxTypeDeployContract == (global::ca::TxType)tx.txtype())
        {
            continue;
        }
        auto txHash = tx.hash();
        nlohmann::json txStorage;
        if (MagicSingleton<TransactionCache>::GetInstance()->GetContractInfoCache(txHash, txStorage) != 0)
        {
            ERRORLOG("can't find storage of tx {}", txHash);
            return -1;
        }

        for(auto &it : txStorage["PrevHash"].items())
        {
            contractTxPreHashMap[txHash].push_back({it.key(), it.value()});
        }
	}
    DBReader dbReader;
    for(auto& iter : contractTxPreHashMap)
    {
        for(auto& preHashPair : iter.second)
        {
            if(contractTxPreHashMap.find(preHashPair.second) != contractTxPreHashMap.end())
            {
                continue;
            }
            std::string DBContractPreHash;
            if (DBStatus::DB_SUCCESS != dbReader.GetLatestUtxoByContractAddr(preHashPair.first, DBContractPreHash))
            {
                ERRORLOG("GetLatestUtxoByContractAddr fail !!! ContractAddr:{}", preHashPair.first);
                return -2;
            }
            if(DBContractPreHash != preHashPair.second)
            {
                ERRORLOG("DBContractPreHash:({}) != preHashPair.second:({})", DBContractPreHash, preHashPair.second);
                return -3;
            }
            contractTxPreHashList.push_back(preHashPair);
        }
    }
    return 0;
}

bool TransactionCache::_VerifyDirtyContract(const std::string &transactionHash, const vector<string> &calledContract)
{
    auto found = _dirtyContractMap.find(transactionHash);
    if (found == _dirtyContractMap.end())
    {
        ERRORLOG("dirty contract not found hash: {}", transactionHash);
        return false;
    }
    std::set<std::string> calledContractSet(calledContract.begin(), calledContract.end());
    std::vector<std::string> result;
    std::set_difference(calledContractSet.begin(), calledContractSet.end(),
                        found->second.second.begin(), found->second.second.end(),
                        std::back_inserter(result));
    if (!result.empty())
    {
        for (const auto& addr : calledContract)
        {
            ERRORLOG("executed {}", addr);
        }
        for (const auto& addr : found->second.second)
        {
            ERRORLOG("found {}", addr);
        }
        for (const auto& addr : result)
        {
            ERRORLOG("result {}", addr);
        }
        ERRORLOG("dirty contract doesn't match execute result, tx hash: {}", transactionHash);
        return false;
    }
    return true;
}

int TransactionCache::_AddContractInfoCache(const CTransaction &transaction,
                                            std::map<std::string, std::string> &contractPreHashCache, ContractDataCache* contractDataCache)
{
    auto txType = (global::ca::TxType)transaction.txtype();
    if (txType != global::ca::TxType::kTxTypeCallContract && txType != global::ca::TxType::kTxTypeDeployContract)
    {
        return 0;
    }

    bool isMultiSign = IsMultiSign(transaction);
    std::string fromAddr;
    int ret = ca_algorithm::GetCallContractFromAddr(transaction, isMultiSign, fromAddr);
    if (ret != 0)
    {
        ERRORLOG("GetCallContractFromAddr fail ret: {}", ret);
        return -1;
    }

    std::string OwnerEvmAddr;
    global::ca::VmType vmType;

    std::string code;
    std::string transientContractAddress;
    std::string input;
    std::string deployerAddr;
    std::string deployHash;
    uint64_t contractTransfer;
    try
    {
        nlohmann::json dataJson = nlohmann::json::parse(transaction.data());
        nlohmann::json txInfo = dataJson["TxInfo"].get<nlohmann::json>();

        if(txInfo.find("OwnerEvmAddr") != txInfo.end())
        {
            OwnerEvmAddr = txInfo["OwnerEvmAddr"].get<std::string>();
        }
        std::string detAddr =  evm_utils::EvmAddrToBase58(OwnerEvmAddr);
        if(fromAddr != detAddr)
        {
            return -2;
        }
        vmType = txInfo["VmType"].get<global::ca::VmType>(); // todo for wasm

        if (txType == global::ca::TxType::kTxTypeCallContract)
        {
            deployerAddr = txInfo["DeployerAddr"].get<std::string>();
            deployHash = txInfo["DeployHash"].get<std::string>();
            input = txInfo["Input"].get<std::string>();
            contractTransfer = txInfo["contractTransfer"].get<uint64_t>();
        }
        else if (txType == global::ca::TxType::kTxTypeDeployContract)
        {
            code = txInfo["Code"].get<std::string>();
            transientContractAddress = txInfo["transientAddress"].get<std::string>();
        }

    }
    catch (...)
    {
        ERRORLOG("json parse fail");
        return -4;
    }

    std::string expectedOutput;
    TfsHost host(contractDataCache);
    int64_t gasCost = 0;
    if(txType == global::ca::TxType::kTxTypeDeployContract)
    {
        ret = Evmone::DeployContract(fromAddr, OwnerEvmAddr, code, expectedOutput,
                                     host, gasCost, transientContractAddress);
        if(ret != 0)
        {
            ERRORLOG("VM failed to deploy contract!, ret {}", ret);
            return ret - 600;
        }
    }
    else if(txType == global::ca::TxType::kTxTypeCallContract)
    {
        ret = Evmone::CallContract(fromAddr, OwnerEvmAddr, deployerAddr, deployHash, input, expectedOutput, host,
                                   gasCost, contractTransfer);
        if(ret != 0)
        {
            ERRORLOG("VM failed to call contract!, ret {}", ret);
            return ret - 700;
        }
    }

    nlohmann::json jTxInfo;

    ret = Evmone::ContractInfoAdd(host, transaction.hash(), txType, transaction.version(), jTxInfo,
                                  contractPreHashCache);
    if(ret != 0)
    {
        ERRORLOG("ContractInfoAdd fail ret: {}", ret);
        return -5;
    }
    std::vector<std::string> calledContract;
    Evmone::GetCalledContract(host, calledContract);
    if (!_VerifyDirtyContract(transaction.hash(), calledContract))
    {
        ERRORLOG("_VerifyDirtyContract fail");
        return -6;
    }
    if(host.contractDataCache != nullptr) host.contractDataCache->set(jTxInfo["Storage"]);
    else return -7;

    AddContractInfoCache(transaction.hash(), jTxInfo, transaction.time());
    return 0;
}

void TransactionCache::AddContractInfoCache(const std::string& transactionHash, const nlohmann::json& jTxInfo, const uint64_t& txtime)
{
    std::unique_lock<std::shared_mutex> locker(_contractInfoCacheMutex);
    _contractInfoCache[transactionHash] = {jTxInfo, txtime};
    return;
}

int TransactionCache::GetContractInfoCache(const std::string& transactionHash, nlohmann::json& jTxInfo)
{
    auto found = _contractInfoCache.find(transactionHash);
    if (found == _contractInfoCache.end())
    {
        return -1;
    }

    jTxInfo = found->second.first;
    return 0;
}

std::string TransactionCache::GetContractPrevBlockHash()
{
    return _preContractBlockHash;
}

bool TransactionCache::HasContractPackingPermission(const std::string& addr, uint64_t transactionHeight, uint64_t time)
{
    std::string packingAddr;
    if (CalculateThePackerByTime(time, transactionHeight, packingAddr, *std::make_unique<std::string>(), *std::make_unique<std::string>()) != 0)
    {
        return false;
    }
    DEBUGLOG("time: {}, height: {}, packer {}", time, transactionHeight, packingAddr);
    return packingAddr == addr;
}

bool TransactionCache::_HasContractPackingPermission(uint64_t transactionHeight, uint64_t time)
{
    return HasContractPackingPermission(MagicSingleton<PeerNode>::GetInstance()->GetSelfId(), transactionHeight, time);
}

void TransactionCache::ContractBlockNotify(const std::string& blockHash)
{
    if (_preContractBlockHash.empty())
    {
        return;
    }
    if (blockHash == _preContractBlockHash)
    {
        _contractPreBlockWaiter.notify_one();
    }
}

std::string
TransactionCache::GetAndUpdateContractPreHash(const std::string &contractAddress, const std::string &transactionHash,
                                              std::map<std::string, std::string> &contractPreHashCache)
{
//    std::unique_lock locker(_contractPreHashCacheMutex);
    std::string strPrevTxHash;
    auto found = contractPreHashCache.find(contractAddress);
    if (found == contractPreHashCache.end())
    {
        DBReader dbReader;
        if (dbReader.GetLatestUtxoByContractAddr(contractAddress, strPrevTxHash) != DBStatus::DB_SUCCESS)
        {
            ERRORLOG("GetLatestUtxo of ContractAddr {} fail", contractAddress);
            return "";
        }
        if (strPrevTxHash.empty())
        {
            return "";
        }
        DEBUGLOG("transactionHash:{}, contractAddress:{}, strPrevTxHash:{}",transactionHash, contractAddress, strPrevTxHash);
    }
    else
    {
        strPrevTxHash = found->second;
    }
    contractPreHashCache[contractAddress] = transactionHash;

    return strPrevTxHash;
}

void TransactionCache::SetDirtyContractMap(const std::string& transactionHash, const std::set<std::string>& dirtyContract)
{
    std::unique_lock locker(_dirtyContractMapMutex);
    uint64_t currentTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    _dirtyContractMap[transactionHash]= {currentTime, dirtyContract};
}

bool TransactionCache::GetDirtyContractMap(const std::string& transactionHash, std::set<std::string>& dirtyContract)
{
    auto found = _dirtyContractMap.find(transactionHash);
    if(found != _dirtyContractMap.end())
    {
        dirtyContract = found->second.second;
        return true;
    }
    
    return false;
}

void TransactionCache::removeExpiredEntriesFromDirtyContractMap()
{
    uint64_t nowTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    for(auto iter = _dirtyContractMap.begin(); iter != _dirtyContractMap.end();)
    {
        if(nowTime >= iter->second.first + 60 * 1000000ull)
        {
            DEBUGLOG("remove txHash:{}", iter->first);
            _dirtyContractMap.erase(iter++);
        }
        else
        {
            ++iter;
        }
    }
}

std::string TransactionCache::_SeekContractPreHash()
{
    DEBUGLOG("_SeekContractPreHash Start");
    uint64_t chainHeight = 0;
    if(!MagicSingleton<BlockHelper>::GetInstance()->ObtainChainHeight(chainHeight))
    {
        DEBUGLOG("ObtainChainHeight fail!!!");
        return "";
    }
    uint64_t selfNodeHeight = 0;
    DBReader dbReader;
    std::vector<std::string> pledgeAddr; // stake and invested addr
    {
        auto status = dbReader.GetBlockTop(selfNodeHeight);
        if (DBStatus::DB_SUCCESS != status)
        {
            DEBUGLOG("GetBlockTop fail!!!");
            return "";
        }
        std::vector<std::string> stakeAddr;
        status = dbReader.GetStakeAddress(stakeAddr);
        if (DBStatus::DB_SUCCESS != status && DBStatus::DB_NOT_FOUND != status)
        {
            DEBUGLOG("GetStakeAddress fail!!!");
            return "";
        }

        for(const auto& addr : stakeAddr)
        {
            if(VerifyBonusAddr(addr) != 0)
            {
                DEBUGLOG("{} doesn't get invested, skip", addr);
                continue;
            }
            pledgeAddr.push_back(addr);
        }
    }
    std::vector<std::string> sendNodeIds;
    if (GetContractPrehashFindNode(pledgeAddr.size() / 5 * 3, chainHeight, pledgeAddr, sendNodeIds) != 0)
    {
        ERRORLOG("get sync node fail");
        return "";
    }

    return _SeekContractPreHashByNode(sendNodeIds);
}

std::string TransactionCache::_SeekContractPreHashByNode(const std::vector<std::string> &sendNodeIds)
{
    std::string msgId;
    if (!GLOBALDATAMGRPTR.CreateWait(2, sendNodeIds.size() * 0.8, msgId))
    {
        ERRORLOG("CreateWait fail!!!");
        return "";
    }
    for (auto &nodeId : sendNodeIds)
    {
        DEBUGLOG("new seek get block hash from {}", nodeId);
        SendSeekContractPreHashReq(nodeId, msgId);
    }
    std::vector<std::string> retDatas;
    if (!GLOBALDATAMGRPTR.WaitData(msgId, retDatas))
    {
        if(retDatas.size() < sendNodeIds.size() * 0.5)
        {
            ERRORLOG("wait seek block hash time out send:{} recv:{}", sendNodeIds.size(), retDatas.size());
            return "";
        }
    }

    SeekContractPreHashAck ack;
    std::unordered_map<std::string, std::vector<std::string>> seekContractPreHashes;
    uint64_t succentCount = 0;
    for (auto &retData : retDatas)
    {
        ack.Clear();
        if (!ack.ParseFromString(retData))
        {
            continue;
        }
        succentCount++;
        seekContractPreHashes[ack.contractprehash()].push_back(ack.self_node_id());
    }
    size_t verifyNum = succentCount / 5 * 4;

    uint64_t maxContractPreHasheCount = 0;
    std::string maxContractPreHash;
    for(auto &iter : seekContractPreHashes)
    {
        //CESHI
        if(maxContractPreHasheCount < iter.second.size())
        {
            maxContractPreHasheCount = iter.second.size();
            maxContractPreHash = iter.first;
        }
        if(iter.second.size() >= verifyNum)
        {
            DEBUGLOG("seekContractPreHash success!!!, ContractPreHash:{}, sendNodeIds:{}, succentCount:{}, succentContractPreHashCount:{}",iter.first, sendNodeIds.size(), succentCount, iter.second.size());
            return iter.first;
        }
    }
    ERRORLOG("seekContractPreHash fail!!!, maxContractPreHash:{}, sendNodeIds:{}, succentCount:{}, maxContractPreHasheCount:{}",maxContractPreHash, sendNodeIds.size(), succentCount, maxContractPreHasheCount);
    return "";
}

int GetContractPrehashFindNode(uint32_t num, uint64_t selfNodeHeight, const std::vector<std::string> &pledgeAddr,
                            std::vector<std::string> &sendNodeIds)
{
    int ret = 0;
    if ((ret = SyncBlock::GetSyncNodeSimplify(num, selfNodeHeight, pledgeAddr, sendNodeIds)) != 0)
    {
        ERRORLOG("get seek node fail, ret:{}", ret);
        return -1;
    }
    return 0;
}
void SendSeekContractPreHashReq(const std::string &nodeId, const std::string &msgId)
{
    SeekContractPreHashReq req;
    req.set_self_node_id(NetGetSelfNodeId());
    req.set_msg_id(msgId);
    NetSendMessage<SeekContractPreHashReq>(nodeId, req, net_com::Compress::kCompress_False, net_com::Encrypt::kEncrypt_False, net_com::Priority::kPriority_High_1);
}

void SendSeekContractPreHashAck(SeekContractPreHashAck& ack,const std::string &nodeId, const std::string &msgId)
{
    DEBUGLOG("SendSeekContractPreHashAck, id:{}",  nodeId);
    ack.set_self_node_id(NetGetSelfNodeId());
    ack.set_msg_id(msgId);

    DBReader dbReader;
    uint64_t currentTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    uint64_t selfNodeHeight = 0;
    if (0 != dbReader.GetBlockTop(selfNodeHeight))
    {
        ERRORLOG("GetBlockTop(txn, top)");
        return;
    }

    std::string packager;
    std::string proof;
    std::string txHash;
    CalculateThePackerByTime(currentTime, selfNodeHeight, packager, proof, txHash);

    if(packager != nodeId)
    {
        ERRORLOG("Packer error in current time cycle, currentTime:{}, packager:{}, SeekPackager:{}",currentTime, packager, nodeId);
        return;
    }
    
    std::vector<std::string> contractBlockHashs;
//    if(dbReader.GetLatestContractBlockHash(contractBlockHashs) != DBStatus::DB_SUCCESS)
//    {
//        ERRORLOG("GetLatestContractBlockHash error");
//        return;
//    }
    ack.set_contractprehash(contractBlockHashs.at(0));
    NetSendMessage<SeekContractPreHashAck>(nodeId, ack, net_com::Compress::kCompress_True, net_com::Encrypt::kEncrypt_False, net_com::Priority::kPriority_High_1);
    return;
}

int HandleSeekContractPreHashReq(const std::shared_ptr<SeekContractPreHashReq> &msg, const MsgData &msgdata)
{
    SeekContractPreHashAck ack;
    SendSeekContractPreHashAck(ack, msg->self_node_id(), msg->msg_id());
    return 0;
}
int HandleSeekContractPreHashAck(const std::shared_ptr<SeekContractPreHashAck> &msg, const MsgData &msgdata)
{
    GLOBALDATAMGRPTR.AddWaitData(msg->msg_id(), msg->SerializeAsString());
    return 0;
}

int HandleContractPackagerMsg(const std::shared_ptr<ContractPackagerMsg> &msg, const MsgData &msgdata)
{
    MagicSingleton<TransactionCache>::GetInstance()->HandleContractPackagerMsg(msg, msgdata);
    return 0;
}

bool TransactionCache::RemoveContractInfoCacheTransaction(const std::map<std::string, CTransaction>& contractTxs)
{
    std::shared_lock<std::shared_mutex> locker(_contractInfoCacheMutex);
    auto it = _contractInfoCache.begin();
    while (it != _contractInfoCache.end()) 
    {
        if (contractTxs.find(it->first) != contractTxs.end()) 
        {
            DEBUGLOG("txHash:{}", it->first);
            it = _contractInfoCache.erase(it); 
        } 
        else 
        {
            ++it;
        }
    }
    return true;
}

bool TransactionCache::RemoveContractsCacheTransaction(const std::map<std::string, CTransaction>& contractTxs) {
    std::unique_lock<mutex> locker(_contractCacheMutex);
    auto it = _contractCache.begin();
    while (it != _contractCache.end()) {
        if (contractTxs.find(it->_transaction.hash()) != contractTxs.end()) {
            it = _contractCache.erase(it); 
        } else {
            ++it;
        }
    }
    return true;
}





//===============================================================================

int CreateBlock_V33_1(std::vector<TransactionEntity_V33_1>& txs, CBlock& cblock)
{
	cblock.Clear();

	// Fill version
	cblock.set_version(0);

	// Fill time
	uint64_t time = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	cblock.set_time(time);
    DEBUGLOG("block set time ======");

	// Fill height
	uint64_t prevBlockHeight = txs.front().GetTxmsg().txmsginfo().height();
	uint64_t cblockHeight = prevBlockHeight + 1;
	cblock.set_height(cblockHeight);

	// Fill tx
	for(auto& tx : txs)
	{
		// Add major transaction
		CTransaction * majorTx = cblock.add_txs();
		*majorTx = tx.GetTransaction();
		
		
		auto txHash = majorTx->hash();
	}

    // Fill preblockhash
    uint64_t seekPrehashTime = 0;
    std::future_status status;
    auto futurePrehash = MagicSingleton<BlockStroage>::GetInstance()->GetPrehash(prevBlockHeight);
    if(!futurePrehash.valid())
    {
        ERRORLOG("futurePrehash invalid,hight:{}",prevBlockHeight);
        return -3;
    }
    status = futurePrehash.wait_for(std::chrono::seconds(6));
    if (status == std::future_status::timeout) 
    {
        ERRORLOG("seek prehash timeout, hight:{}",prevBlockHeight);
        return -4;
    }
    else if(status == std::future_status::ready) 
    {
        std::string preBlockHash = futurePrehash.get().first;
        if(preBlockHash.empty())
        {
            ERRORLOG("seek prehash <fail>!!!,hight:{},prehash:{}",prevBlockHeight, preBlockHash);
            return -5;
        }
        DEBUGLOG("seek prehash <success>!!!,hight:{},prehash:{},blockHeight:{}",prevBlockHeight, preBlockHash,cblockHeight);
        seekPrehashTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        cblock.set_prevhash(preBlockHash);
    }
    
	// Fill merkleroot
	cblock.set_merkleroot(ca_algorithm::CalcBlockMerkle(cblock));
	// Fill hash
	cblock.set_hash(Getsha256hash(cblock.SerializeAsString()));
    DEBUGLOG("block hash = {} set time ",cblock.hash());
    MagicSingleton<TFSbenchmark>::GetInstance()->SetByBlockHash(cblock.hash(), &seekPrehashTime, 8);
    MagicSingleton<TFSbenchmark>::GetInstance()->AddBlockContainsTransactionAmountMap(cblock.hash(), txs.size());
	return 0;
}

int BuildBlock_V33_1(std::vector<TransactionEntity_V33_1>& txs, bool build_first)
{
	// if(txs.empty())
	// {
	// 	ERRORLOG("Txs is empty!");
	// 	return -1;
	// }

	// CBlock cblock;
    // auto S = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	// int ret = CreateBlock_V33_1(txs,cblock);
    // if(ret != 0)
    // {
    //     if(ret == -3 || ret == -4 || ret == -5)
    //     {
    //         MagicSingleton<BlockStroage>::GetInstance()->ForceCommitSeekTask(cblock.height() - 1);
    //     }
    //     ERRORLOG("Create block failed!");
	// 	return ret - 100;
    // }
	// auto S1 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
	// std::string serBlock = cblock.SerializeAsString();

	// ca_algorithm::PrintBlock(cblock);
    // auto startT4 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    // auto endT4 = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    // auto blockTime = cblock.time();
    // auto t4 = endT4 - startT4;
    // auto txSize = txs.size();
    // auto BlockHight = cblock.height();
    // MagicSingleton<TFSbenchmark>::GetInstance()->SetByBlockHash(cblock.hash(), &blockTime, 1 , &t4, &txSize, &BlockHight);

    // BlockMsg blockmsg;
    // blockmsg.set_version(global::kVersion);
    // blockmsg.set_time(MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp());
    // blockmsg.set_block(serBlock);


    // for(auto &tx : cblock.txs())
    // {
    //     if(GetTransactionType(tx) != kTransactionType_Tx)
    //     {
    //         continue;
    //     }

    //     CTransaction copyTx = tx;
    //     copyTx.clear_hash();
    //     copyTx.clear_verifysign();
    //     std::string txHash = Getsha256hash(copyTx.SerializeAsString());
    //     MagicSingleton<TFSbenchmark>::GetInstance()->SetTxHashByBlockHash(cblock.hash(), txHash);

    //     uint64_t handleTxHeight =  cblock.height() - 1;
    //     TxHelper::vrfAgentType type = TxHelper::GetVrfAgentType(tx, handleTxHeight);
    //     if(type == TxHelper::vrfAgentType_defalut || type == TxHelper::vrfAgentType_local)
    //     {
    //         continue;
    //     }

    //     std::pair<std::string,Vrf>  vrf;
    //     if(!MagicSingleton<VRF>::GetInstance()->getVrfInfo(txHash, vrf))
    //     {
    //         ERRORLOG("getVrfInfo failed!");
    //         return -3000;
    //     }
    //     Vrf *vrfinfo  = blockmsg.add_vrfinfo();
    //     vrfinfo ->CopyFrom(vrf.second);

    //     if(!MagicSingleton<VRF>::GetInstance()->getTxVrfInfo(txHash, vrf))
    //     {
    //         ERRORLOG("getTxVrfInfo failed!");
    //         return -4000;
    //     }

    //     auto vrfJson = nlohmann::json::parse(vrf.second.data());
	// 	vrfJson["txhash"] = txHash;
    //     vrf.second.set_data(vrfJson.dump());

    //     blockmsg.add_txvrfinfo()->CopyFrom(vrf.second);
    // }
    
    // auto msg = make_shared<BlockMsg>(blockmsg);
	// // ret = DoHandleBlock_V33_1(msg);
    // if(ret != 0)
    // {
    //     ERRORLOG("DoHandleBlock failed The error code is {}",ret);
    //     CBlock cblock;
	//     if (!cblock.ParseFromString(msg->block()))
	//     {
	// 	    ERRORLOG("fail to serialization!!");
	// 	    return -3090;
	//     }
    //     ClearVRF(cblock);
    //     return ret -4000;
    // }

	return 0;
}



const int TransactionCache_V33_1::_kBuildInterval = 3 * 1000;
const time_t TransactionCache_V33_1::_kTxExpireInterval  = 10;
const int TransactionCache_V33_1::_kBuildThreshold = 1000000;


TransactionCache_V33_1::TransactionCache_V33_1()
{
    _buildTimer.AsyncLoop(
        _kBuildInterval, 
        [=](){ _blockBuilder.notify_one(); }
        );
}

int TransactionCache_V33_1::AddCache(const CTransaction& transaction, const TxMsgReq& sendTxMsg)
{
    std::unique_lock<mutex> locker(_cacheMutex);
    if(CheckConflict(transaction))
    {
        DEBUGLOG("DoubleSpentTransactions, txHash:{}", transaction.hash());
        return -1;
    }
    uint64_t height = sendTxMsg.txmsginfo().height() + 1;

    auto find = _cache.find(height); 
    if(find == _cache.end()) 
    {
        _cache[height] = std::list<TransactionEntity_V33_1>{}; 
    }

    time_t add_time = time(NULL);
    _cache.at(height).push_back(TransactionEntity_V33_1(transaction, sendTxMsg, add_time)) ;
    for(auto txEntity: _cache)
    {
        if (txEntity.second.size() >= _kBuildThreshold)
        {
            _blockBuilder.notify_one();
        }
    }
    return 0;
}

bool TransactionCache_V33_1::Process()
{
    _buildThread = std::thread(std::bind(&TransactionCache_V33_1::_ProcessingFunc, this));
    _buildThread.detach();
    return true;
}

bool TransactionCache_V33_1::CheckConflict(const CTransaction& transaction)
{
    return Checker::CheckConflict_V33_1(transaction, _cache);
}
void TransactionCache_V33_1::Stop(){
    _threadRun=false;
}

void TransactionCache_V33_1::_ProcessingFunc()
{
    while (_threadRun)
    {
        std::unique_lock<mutex> locker(_cacheMutex);
        auto SPending = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        _blockBuilder.wait(locker);
        auto EPending = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
        std::vector<cacheIter> emptyHeightCache;
        for(auto cacheEntity = _cache.begin(); cacheEntity != _cache.end(); ++cacheEntity)
        {
            if(cacheEntity == _cache.end())
            {
                break;
            }
            
            MagicSingleton<TFSbenchmark>::GetInstance()->SetBlockPendingTime(EPending);

            std::list<txEntitiesIter> buildTxs = _GetNeededCache(cacheEntity->second);
            std::vector<TransactionEntity_V33_1> buildCaches;
            for(auto iter : buildTxs)
            {
                buildCaches.push_back(*iter);
            }
            auto ret = BuildBlock_V33_1(buildCaches, false);
            if(ret != 0)
            {
                ERRORLOG("{} build block fail", ret);
                if(ret == -103 || ret == -104 || ret == -105 || ret == -2)
                { 
                    continue;
                }
                _TearDown(buildTxs, false, emptyHeightCache, cacheEntity);
                continue;
            }
            _TearDown(buildTxs, true, emptyHeightCache, cacheEntity);
        }
        for(auto cache: emptyHeightCache)
        {
            _cache.erase(cache);
        }
        locker.unlock();
        
    }
    
}

std::list<TransactionCache_V33_1::txEntitiesIter> TransactionCache_V33_1::_GetNeededCache(const std::list<TransactionEntity_V33_1>& txs)
{
    std::list<txEntitiesIter> buildCaches;

    if(txs.empty())
    {
        return buildCaches;
    }

    txEntitiesIter iter = txs.begin();
    txEntitiesIter end = txs.end();

    for(int i = 0; i < _kBuildThreshold && iter != end; ++i, ++iter) 
    {
        buildCaches.push_back(iter);
    }        


    return buildCaches;
}

bool TransactionCache_V33_1::_RemoveProcessedTransaction(const  std::list<txEntitiesIter>& txEntitiesIter, const bool buildSuccess, std::list<TransactionEntity_V33_1>& txEntities)
{

    for(auto iter : txEntitiesIter)
    {
        std::string hash = iter->GetTransaction().hash();
        txEntities.erase(iter);
        std::string message;
        if(buildSuccess)
        {
            message = " successfully packaged";
        }
        else
        {
            message = " packaging fail";
        }
        std::cout << "transaction " << hash << message << std::endl;
    }
    

    for(auto txEntity = txEntities.begin(); txEntity != txEntities.end(); ++txEntity)
    {
        time_t current_time = time(NULL);
        if((current_time - txEntity->GetTimestamp()) > _kTxExpireInterval)
        {
            TRACELOG("transaction {} has expired", txEntity->GetTransaction().hash());
            std::cout << "transaction expired: " << txEntity->GetTransaction().hash() << std::endl;
        }
    }

    if(txEntities.empty())
    {
        return false;
    }            
    return true;
}

void TransactionCache_V33_1::GetCache(std::map<uint64_t, std::list<TransactionEntity_V33_1>>& cache)
{
    cache = _cache;
}

void TransactionCache_V33_1::_TearDown(const  std::list<txEntitiesIter>& txEntitiesIters, const bool buildSuccess, std::vector<cacheIter>& emptyHeightCache , cacheIter cacheEntity)
{
    if(!_RemoveProcessedTransaction(txEntitiesIters, buildSuccess, cacheEntity->second))
    {
        emptyHeightCache.push_back(cacheEntity);         
    }
}

int _HandleSeekContractPreHashReq(const std::shared_ptr<newSeekContractPreHashReq> &msg, const MsgData &msgdata)
{
    newSeekContractPreHashAck ack;
    ack.set_version(msg->version());
    ack.set_msg_id(msg->msg_id());
    ack.set_self_node_id(NetGetSelfNodeId());
    Node node;
	if (!MagicSingleton<PeerNode>::GetInstance()->FindNodeByFd(msgdata.fd, node))
	{
        ERRORLOG("FindNodeByFd fail !!!, seekId:{}", node.base58Address);
		return -1;
	}

    DBReader dbReader;
    if(msg->seekroothash_size() >= 200)
    {
        ERRORLOG("msg->seekroothash_size:({}) >= 200", msg->seekroothash_size());
        return -2;
    }
    for(auto& preHashPair : msg->seekroothash())
    {
        std::string DBContractPreHash;
        if (DBStatus::DB_SUCCESS != dbReader.GetLatestUtxoByContractAddr(preHashPair.contractaddr(), DBContractPreHash))
        {
            ERRORLOG("GetLatestUtxoByContractAddr fail !!!");
            return -3;
        }
        if(DBContractPreHash != preHashPair.roothash())
        {
            DEBUGLOG("DBContractPreHash:({}) != roothash:({}) seekId:{}", DBContractPreHash, preHashPair.roothash(), node.base58Address);
            std::string strPrevBlockHash;
            if(dbReader.GetBlockHashByTransactionHash(DBContractPreHash, strPrevBlockHash) != DBStatus::DB_SUCCESS)
            {
                ERRORLOG("GetBlockHashByTransactionHash failed!");
                return -4;
            }
            std::string blockRaw;
            if(dbReader.GetBlockByBlockHash(strPrevBlockHash, blockRaw) != DBStatus::DB_SUCCESS)
            {
                ERRORLOG("GetBlockByBlockHash failed!");
                return -5;
            }
            auto seekContractBlock = ack.add_seekcontractblock();
            seekContractBlock->set_contractaddr(preHashPair.contractaddr());
            seekContractBlock->set_roothash(strPrevBlockHash);
            seekContractBlock->set_blockraw(blockRaw);
        }
    }

    NetSendMessage<newSeekContractPreHashAck>(node.base58Address, ack, net_com::Compress::kCompress_True, net_com::Encrypt::kEncrypt_False, net_com::Priority::kPriority_High_1);
    return 0;
}
int _HandleSeekContractPreHashAck(const std::shared_ptr<newSeekContractPreHashAck> &msg, const MsgData &msgdata)
{
    GLOBALDATAMGRPTR.AddWaitData(msg->msg_id(), msg->SerializeAsString());
    return 0;
}

int _newSeekContractPreHash(const std::list<std::pair<std::string, std::string>> &contractTxPreHashList)
{
    DEBUGLOG("_newSeekContractPreHash.............");
    uint64_t chainHeight;
    if(!MagicSingleton<BlockHelper>::GetInstance()->ObtainChainHeight(chainHeight))
    {
        DEBUGLOG("ObtainChainHeight fail!!!");
    }
    uint64_t selfNodeHeight = 0;
    std::vector<std::string> pledgeAddr; // stake and invested addr
    {
        DBReader dbReader;
        auto status = dbReader.GetBlockTop(selfNodeHeight);
        if (DBStatus::DB_SUCCESS != status)
        {
            DEBUGLOG("GetBlockTop fail!!!");

        }
        std::vector<std::string> stakeAddr;
        status = dbReader.GetStakeAddress(stakeAddr);
        if (DBStatus::DB_SUCCESS != status && DBStatus::DB_NOT_FOUND != status)
        {
            DEBUGLOG("GetStakeAddress fail!!!");
        }

        for(const auto& addr : stakeAddr)
        {
            if(VerifyBonusAddr(addr) != 0)
            {
                DEBUGLOG("{} doesn't get invested, skip", addr);
                continue;
            }
            pledgeAddr.push_back(addr);
        }
    }
    std::vector<std::string> sendNodeIds;
    if (GetPrehashFindNode(pledgeAddr.size(), chainHeight, pledgeAddr, sendNodeIds) != 0)
    {
        ERRORLOG("get sync node fail");
    }

    if(sendNodeIds.size() == 0)
    {
        DEBUGLOG("sendNodeIds {}",sendNodeIds.size());
        return -2;
    }

    //send_size
    std::string msgId;
    if (!GLOBALDATAMGRPTR.CreateWait(3, sendNodeIds.size() * 0.8, msgId))
    {
        return -3;
    }

    newSeekContractPreHashReq req;
    req.set_version(global::kVersion);
    req.set_msg_id(msgId);

    for(auto &item : contractTxPreHashList)
    {
        preHashPair * _hashPair = req.add_seekroothash();
        _hashPair->set_contractaddr(item.first);
        _hashPair->set_roothash(item.second);
        DEBUGLOG("req contractAddr:{}, contractTxHash:{}", item.first, item.second);
    }
    
    DEBUGLOG("1111111111111");
    for (auto &nodeBase58 : sendNodeIds)
    {
        NetSendMessage<newSeekContractPreHashReq>(nodeBase58, req, net_com::Compress::kCompress_False, net_com::Encrypt::kEncrypt_False, net_com::Priority::kPriority_High_1);
    }
    DEBUGLOG("22222222222222");
    std::vector<std::string> ret_datas;
    if (!GLOBALDATAMGRPTR.WaitData(msgId, ret_datas))
    {
        return -4;
    }
    DEBUGLOG("3333333333333333");
    newSeekContractPreHashAck ack;
    std::map<std::string, std::set<std::string>> blockHashMap;
    std::map<std::string, std::pair<std::string, std::string>> testMap; //TODO::
    for (auto &ret_data : ret_datas)
    {
        ack.Clear();
        if (!ack.ParseFromString(ret_data))
        {
            continue;
        }
        for(auto& iter : ack.seekcontractblock())
        {
            blockHashMap[ack.self_node_id()].insert(iter.blockraw());
            testMap[iter.blockraw()] = {iter.contractaddr(), iter.roothash()};
        }
    }
    DEBUGLOG("4444444444444444444");
    std::unordered_map<std::string , int> countMap;

    for (auto& iter : blockHashMap) 
    {
        for(auto& iter_second : iter.second)
        {
            countMap[iter_second]++;
        }
        
    }

    DEBUGLOG("55555555555555");
    DBReader dbReader;
    std::vector<std::pair<CBlock,std::string>> seekBlocks;
    for (const auto& iter : countMap) 
    {
        double rate = double(iter.second) / double(blockHashMap.size());
        auto test_iter = testMap[iter.first];
        if(rate < 0.66)
        {
            ERRORLOG("rate:({}) < 0.66, contractAddr:{}, contractTxHash:{}", rate, test_iter.first, test_iter.second);
            continue;
        }

        CBlock block;
        if(!block.ParseFromString(iter.first))
        {
            continue;
        }
        string blockStr;
        if(dbReader.GetBlockByBlockHash(block.hash(), blockStr) != DBStatus::DB_SUCCESS)
        {
            seekBlocks.push_back({block, block.hash()});
            DEBUGLOG("rate:({}) < 0.66, contractAddr:{}, contractTxHash:{}, blockHash:{}", rate, test_iter.first, test_iter.second, block.hash());
            MagicSingleton<BlockHelper>::GetInstance()->AddSeekBlock(seekBlocks);
            DEBUGLOG("wwwwwwwwwwwwwwwwww");
        }
    }
    DEBUGLOG("666666666666666");

    uint64_t timeOut = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp() + 2 * 1000000;
    uint64_t currentTime;
    bool flag;
    do
    {
        flag = true;
        for(auto& it : seekBlocks)
        {
            std::string blockRaw;
            if(dbReader.GetBlockByBlockHash(it.second, blockRaw) != DBStatus::DB_SUCCESS)
            {
                flag = false;
                break;
            }
        }
        if(flag)
        {
            DEBUGLOG("find block successfuly ");
            return 0;
        }
        sleep(1);
        currentTime = MagicSingleton<TimeUtil>::GetInstance()->GetUTCTimestamp();
    }while(currentTime < timeOut && !flag);
    return -6;
}