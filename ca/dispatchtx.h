#ifndef __DISPATCHER_H_
#define __DISPATCHER_H_
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include "./transaction.h"
#include "./global.h"
#include "transaction_cache.h"
#include "interface.h"
#include "./utils/console.h"

class ContractDispatcher{

    public:
        ContractDispatcher() = default;
        ~ContractDispatcher() = default;

        void AddContractInfo(const std::string& contractHash, const std::vector<std::string>& dependentContracts);
        void AddContractMsgReq(const std::string& contractHash, const ContractTxMsgReq &msg);
        void RemoveContractInfo(const std::string& contractName);
        void ClearContractInfoCache();
        void Start();
        void Stop();
        void Process();
        bool HasDuplicate(const std::vector<std::string>& v1, const std::vector<std::string>& v2);
        void setValue(const uint64_t& newValue);
        
    private:
        constexpr static int _contractWaitingTime = 3 * 1000000;

        struct msgInfo
        {
            std::vector<TxMsgReq> txMsgReq;
            std::set<std::string> nodelist;
            Vrf info; 
        };

        void _DispatcherProcessingFunc();

        std::vector<std::vector<TxMsgReq>> GetDependentData();
        std::vector<std::vector<TxMsgReq>> GroupDependentData(const std::vector<std::vector<TxMsgReq>> & txMsgVec);
        int DistributionContractTx(std::multimap<std::string, msgInfo>& distribution);
        int SendTxInfoToPackager(const std::string &packager, const Vrf &info, std::vector<TxMsgReq> &txsmsg,const std::set<std::string> nodelist);

    private:

        std::thread _dispatcherThread;
        std::mutex _contractInfoCacheMutex;
        std::mutex _contractMsgMutex;
        std::mutex _contractHandleMutex;
        std::mutex _mtx;

        bool isFirst = false;
        uint64_t timeValue;

        std::unordered_map<std::string, std::vector<std::string>> _contractDependentCache; 
        std::unordered_map<std::string, TxMsgReq> _contractMsgReqCache; 
};

#endif