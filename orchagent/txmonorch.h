#ifndef SWSS_TXMONORCH_H
#define SWSS_TXMONORCH_H

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"
#include "table.h"
#include "selectabletimer.h"
#include "select.h"
#include "timer.h"

/*tx state definition*/
enum txMonState{
    TXMON_PORT_STATE_OK = 0,
    TXMON_PORT_STATE_NOT_OK,
    TXMON_PORT_STATE_INVALID,
    TXMON_PORT_STATE_UNKNOWN,
    TXMON_PORT_STATE_MAX
};

/*tuple to represent single port monitoring info - oid string, port state, counter*/
typedef std::tuple<sai_object_id_t, txMonState, uint64_t, uint32_t> txMonPortInfo;
/*Map to save info on all ports: port(string) --> info*/
typedef std::map<std::string, txMonPortInfo> txMonInfoMap;

#define tmpiOidStr  std::get<0>
#define tmpiState   std::get<1>
#define tmpiCounter std::get<2>
#define tmpiThresh  std::get<3>

class TxMonOrch : public Orch
{
public:
    TxMonOrch(TableConnector confDbConnector,
              TableConnector stateDbConnector, TableConnector appDbConnector);
    virtual ~TxMonOrch(void);


private:
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
    void TxMonHandleTimerIntervalUpdate(const vector<FieldValueTuple>& data, bool isSet);
    bool TxMonHandleThresholdUpdate(const string &alias,
                                    const vector<FieldValueTuple>& data,
                                    bool isSet);
    void timerPopScanTxMap(void);
    bool timerPopHandleSinglePort(const string& portAlias,
                                  txMonPortInfo& portInfo);
    void timerPopUpdateStateDb(const string& portAlias,
                               enum txMonState portState);

    /*declare connectores to all redis DBs*/
    Table m_appDbTxMonTable;
    Table m_stateDbTxMonTable;

    //needed in order to get stat counters
    shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    shared_ptr<swss::Table> m_countersTable = nullptr;

    /*map DB to manage counters info per port*/
    txMonInfoMap m_txMonInfoMap;


    /*global timer interval value*/
    uint32_t m_txMonTimerInterval;

    /*timer object*/
    SelectableTimer* m_txMonTimer = nullptr;
};

#endif /* SWSS_TXMONORCH_H */
