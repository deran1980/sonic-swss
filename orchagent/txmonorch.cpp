#include "converter.h"
#include "timer.h"
#include "port.h"
#include "select.h"
#include "portsorch.h"
#include "orch.h"
#include "sai_serialize.h"
#include <array>

#include "txmonorch.h"

using namespace std;
using namespace swss;

extern PortsOrch*     gPortsOrch;

static const array<string, TXMON_PORT_STATE_MAX> stateNames = {"OK", "NOT_OK", "INVALID", "UNKNOWN"};
static const string COUNTER_NAME = "SAI_PORT_STAT_IF_OUT_ERRORS";
#define TXMON_TIMER_INTERVAL_KEY    "TXMON_TIMER_INTERVAL"


TxMonOrch::TxMonOrch(TableConnector confDbConnector,
                     TableConnector stateDbConnector, TableConnector appDbConnector):
    Orch(confDbConnector.first, confDbConnector.second),
    m_appDbTxMonTable(appDbConnector.first, appDbConnector.second),
    m_stateDbTxMonTable(stateDbConnector.first, stateDbConnector.second),
    m_countersDb(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_countersTable(new Table(m_countersDb.get(), "COUNTERS")),
    m_txMonTimer(new SelectableTimer(timespec { .tv_sec = 0, .tv_nsec = 0 })),
    m_txMonTimerInterval(0),
    m_txMonInfoMap()
{
    SWSS_LOG_ENTER();

    auto executor = new ExecutableTimer(m_txMonTimer, this, "TXMON_PERIOD_TIMER");
    Orch::addExecutor(executor);

    SWSS_LOG_NOTICE("TxMonOrch initialized with tables %s %s %s\n",
                    appDbConnector.second.c_str(),
                    stateDbConnector.second.c_str(),
                    confDbConnector.second.c_str());
}

TxMonOrch::~TxMonOrch(void)
{
    SWSS_LOG_ENTER();
}

void TxMonOrch::TxMonHandleTimerIntervalUpdate(const vector<FieldValueTuple>& data,
                                               bool isSet)
{
    SWSS_LOG_ENTER();

    try
    {
        if (isSet) {
            for (auto idx : data)
            {
                m_txMonTimerInterval = stoi(fvValue(idx));

                auto interval = timespec { .tv_sec = m_txMonTimerInterval, .tv_nsec = 0 };
                m_txMonTimer->setInterval(interval);
                m_txMonTimer->reset();
                SWSS_LOG_NOTICE("Changing timer interval value to %s seconds", fvValue(idx).c_str());
            }
        } else {
            m_txMonTimerInterval = 0;
            m_txMonTimer->stop();
            SWSS_LOG_NOTICE("TXMON timer interval stop");
        }
    }
    catch (...) {
        SWSS_LOG_WARN("Input timer interval value is invalid.");
    }
}

bool TxMonOrch::TxMonHandleThresholdUpdate(const string &portAlias,
                                           const vector<FieldValueTuple>& data,
                                           bool isSet)
{
    SWSS_LOG_ENTER();

    Port port;

    try
    {
        if (isSet)
        {
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);

                if (field == "thresh_val")
                {
                    /*check if port exist in local cache - if not create*/
                    txMonPortInfo &portInfo = m_txMonInfoMap[portAlias]; //this implicitly create entry if not exist...
                    if (tmpiOidStr(portInfo) == 0) //not exist?
                    {
                        if (gPortsOrch->getPort(portAlias, port))
                        {
                            tmpiOidStr(portInfo) = port.m_port_id;
                        } else {
                            //TODO what to do in case of  failure...
                        }
                        tmpiState(portInfo) = TXMON_PORT_STATE_UNKNOWN;
                    }

                    tmpiThresh(portInfo) = to_uint<uint32_t>(value);

                    vector<FieldValueTuple> fvt;

                    /*save in application DB a reflection of all ports states*/
                    fvt.emplace_back("threshold",to_string(tmpiThresh(portInfo)));
                    fvt.emplace_back("counter_val",to_string(tmpiCounter(portInfo)));
                    fvt.emplace_back("state",to_string(tmpiState(portInfo)));

                    m_appDbTxMonTable.set(portAlias, fvt);
                    timerPopUpdateStateDb(portAlias, TXMON_PORT_STATE_UNKNOWN);

                    m_appDbTxMonTable.flush();
                    m_stateDbTxMonTable.flush();
                } else {
                    SWSS_LOG_WARN("Unknown threshold field type");
                    return false;
                }
            }
        } else {
            //remove entry from all relevant DBs
            m_txMonInfoMap.erase(portAlias);
            m_appDbTxMonTable.del(portAlias);
            m_stateDbTxMonTable.del(portAlias);

            SWSS_LOG_INFO("TXMON threshold cleared for port %s\n", portAlias.c_str());
        }
    }
    catch (...)
    {
        SWSS_LOG_WARN("Port %s input threshold is invalid.", portAlias.c_str());
    }

    return true;
}

void TxMonOrch::timerPopUpdateStateDb(const string& portAlias,
                                      enum txMonState portState)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fvt;

    fvt.emplace_back("port_state", stateNames[portState]);
    m_stateDbTxMonTable.set(portAlias, fvt);
}



bool TxMonOrch::timerPopHandleSinglePort(const string& portAlias,
                                         txMonPortInfo& portInfo)
{
    SWSS_LOG_ENTER();

    string cntStrVal;
    string oidStr = sai_serialize_object_id(tmpiOidStr(portInfo));

    SWSS_LOG_INFO("TX_MON: Read port %s DB counter value %lu\n", portAlias.c_str(),
                  tmpiCounter(portInfo));

    if (!m_countersTable->hget(oidStr, COUNTER_NAME, cntStrVal))
    {
        SWSS_LOG_ERROR("Error reading counters table for port %s", portAlias.c_str());
        /*update state DB*/
        timerPopUpdateStateDb(portAlias, TXMON_PORT_STATE_INVALID);
        return false;
    }

    SWSS_LOG_INFO("TX_MON: Read port %s SAI counter value %s\n", portAlias.c_str(),
                  cntStrVal.c_str());

    /*check if counters pass threshold*/
    if (stoul(cntStrVal) - tmpiCounter(portInfo) > tmpiThresh(portInfo))
    {
        if(tmpiState(portInfo) != TXMON_PORT_STATE_NOT_OK)
        {
            tmpiState(portInfo)  = TXMON_PORT_STATE_NOT_OK;
            /*update state DB*/
            timerPopUpdateStateDb(portAlias, TXMON_PORT_STATE_NOT_OK);
        }
    } else {
        /*update state to OK*/
        if(tmpiState(portInfo) != TXMON_PORT_STATE_OK)
        {
            tmpiState(portInfo)  = TXMON_PORT_STATE_OK;
            /*update state DB*/
            timerPopUpdateStateDb(portAlias, TXMON_PORT_STATE_OK);
        }
    }

    /*update new counter value */
    tmpiCounter(portInfo) = stoul(cntStrVal);

    return true;
}

void TxMonOrch::timerPopScanTxMap()
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fvt;

    for (auto &entry : m_txMonInfoMap)
    {
        /*read current port counters from DB*/
        if (!timerPopHandleSinglePort(entry.first, entry.second)) {
            continue;
        }

        /*save in application DB a reflection of all ports states*/
        fvt.emplace_back("threshold",to_string(tmpiThresh(entry.second)));
        fvt.emplace_back("counter_val",to_string(tmpiCounter(entry.second)));
        fvt.emplace_back("state",to_string(tmpiState(entry.second)));

        m_appDbTxMonTable.set(entry.first, fvt);
    }

    m_appDbTxMonTable.flush();
    m_stateDbTxMonTable.flush();
}


void TxMonOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    bool status = true;

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    /*here we handle changes in config table. Possible changes:
     * 1. set global time period in seconds
     *      key: TXMON_KEY_PERIOD
     *      op: SET\DEL
     *      value: Seconds
     * 2. set packet threshold per interface:
     *      Key: Port Alias
     *      op: SET\DEL
     *      Value: Packets count */

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        vector<FieldValueTuple> fvt = kfvFieldsValues(t);

        SWSS_LOG_INFO("TX_MON: Configuration key %s op %s\n",
                      key.c_str(),
                      op.c_str());

        /*check if setting global period*/
        if (key == TXMON_TIMER_INTERVAL_KEY)
        {
            if (op == SET_COMMAND)
            {
                TxMonHandleTimerIntervalUpdate(fvt, true);
            } else {
                TxMonHandleTimerIntervalUpdate(fvt, false);
            }
        } else {
            /*set threshold per port*/
            if (op == SET_COMMAND)
            {
                status = TxMonHandleThresholdUpdate(key, fvt, true);
            } else {
                status = TxMonHandleThresholdUpdate(key, fvt, false);
            }
        }

        if (!status) {
            SWSS_LOG_ERROR("Configuration failed for key %s.", key.c_str());
        }
        consumer.m_toSync.erase(it++);
    }
}


void TxMonOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    /* timer pop:
     * read all counters of all relevant ports and compare to previous read.
     * in case counter diff is bigger then threshold - set state of port to not OK.
     * otherwise set to OK.*/
    timerPopScanTxMap();
}
