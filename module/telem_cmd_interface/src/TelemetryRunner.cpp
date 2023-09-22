/**
 * @file TelemetryRunner.cpp
 *
 * @copyright Copyright © 2023 United States Government as represented by
 * the National Aeronautics and Space Administration.
 * No copyright is claimed in the United States under Title 17, U.S.Code.
 * All Other Rights Reserved.
 *
 * @section LICENSE
 * Released under the NASA Open Source Agreement (NOSA)
 * See LICENSE.md in the source root directory for more information.
 *
 */

#include <iostream>
#include <unordered_map>

#include <boost/filesystem.hpp>
#include <boost/function.hpp>
#include <boost/make_unique.hpp>
#include <boost/program_options.hpp>
#include <boost/thread.hpp>
#include <boost/tokenizer.hpp>
#include <boost/asio.hpp>

#include "TelemetryRunner.h"
#include "Logger.h"
#include "SignalHandler.h"
#include "TelemetryDefinitions.h"
#include "TelemetryConnection.h"
#include "TelemetryConnectionPoller.h"
#include "TelemetryLogger.h"
#include "Environment.h"
#include "DeadlineTimer.h"
#include "ThreadNamer.h"
#include <queue>
#include <atomic>
#ifdef USE_WEB_INTERFACE
#include "BeastWebsocketServer.h"
#endif

static constexpr hdtn::Logger::SubProcess subprocess = hdtn::Logger::SubProcess::telem;

/**
 * Polling options
 */
static const uint16_t THREAD_INTERVAL_MS = 1000;
static const uint8_t TELEM_NUM_POLL_ATTEMPTS = 3;
static const uint16_t TELEM_TIMEOUT_POLL_MS = 200;
static const uint16_t API_TIMEOUT_POLL_MS = 100;
static const uint16_t API_NUM_POLL_ATTEMPTS = 3;

/**
 * Bitmask codes for tracking receive events
 */
static const unsigned int REC_INGRESS = 0x01;
static const unsigned int REC_EGRESS = 0x02;
static const unsigned int REC_STORAGE = 0x04;

/**
 * TelemetryRunner implementation class
 */
class TelemetryRunner::Impl : private boost::noncopyable {
    public:
        Impl();
        bool Init(const HdtnConfig& hdtnConfig, zmq::context_t *inprocContextPtr, TelemetryRunnerProgramOptions& options);
        void Stop();

    private:
        void ThreadFunc(const HdtnDistributedConfig_ptr& hdtnDistributedConfigPtr, zmq::context_t * inprocContextPtr);
        void OnNewJsonTelemetry(const char* buffer, uint64_t bufferSize);
        void OnNewWebsocketConnectionCallback(WebsocketSessionPublicBase& conn);
        bool OnNewWebsocketDataReceivedCallback(WebsocketSessionPublicBase& conn, std::string& receivedString);
        bool OnApiRequest(std::string&& msgJson, zmq::message_t&& connectionID);
        bool HandleIngressCommand(std::string& movablePayload, zmq::message_t& connectionID);
        bool HandleStorageCommand(std::string& movablePayload, zmq::message_t& connectionID);
        bool HandleRouterCommand(std::string &movablePayload, zmq::message_t& connectionID);
        bool HandleEgressCommand(std::string &movablePayload, zmq::message_t& connectionID);
        bool ProcessHdtnConfigRequest(std::string &movablePayload, zmq::message_t& connectionID);
        void QueueTelemRequests();
       
        std::atomic<bool> m_running;
        std::unique_ptr<boost::thread> m_threadPtr;
#ifdef USE_WEB_INTERFACE
        std::unique_ptr<BeastWebsocketServer> m_websocketServerPtr;
#endif
        std::unique_ptr<TelemetryLogger> m_telemetryLoggerPtr;
        DeadlineTimer m_deadlineTimer;
        HdtnConfig m_hdtnConfig;
        std::shared_ptr<std::string> m_hdtnConfigJsonPtr;

        boost::mutex m_lastSerializedAllOutductCapabilitiesMutex;
        std::shared_ptr<std::string> m_lastJsonSerializedAllOutductCapabilitiesPtr;

        std::unique_ptr<TelemetryConnection> m_ingressConnection;
        std::unique_ptr<TelemetryConnection> m_egressConnection;
        std::unique_ptr<TelemetryConnection> m_storageConnection;
        std::unique_ptr<TelemetryConnection> m_routerConnection;
        std::unique_ptr<TelemetryConnection> m_apiConnection;

        typedef boost::function<bool(std::string& movablePayload, zmq::message_t& connectionID)> ApiCommandFunction_t;
        typedef std::unordered_map<std::string, ApiCommandFunction_t> ApiCommandFunctionMap_t;
        ApiCommandFunctionMap_t m_apiCmdMap;
};

/**
 * TelemetryRunner proxies
 */
TelemetryRunner::TelemetryRunner()
    : m_pimpl(boost::make_unique<TelemetryRunner::Impl>())
{}

bool TelemetryRunner::Init(const HdtnConfig &hdtnConfig, zmq::context_t *inprocContextPtr, TelemetryRunnerProgramOptions &options) {
    return m_pimpl->Init(hdtnConfig, inprocContextPtr, options);
}

void TelemetryRunner::Stop() {
    m_pimpl->Stop();
}

TelemetryRunner::~TelemetryRunner() {
    Stop();
}

/**
 * TelemetryRunner implementation
 */

TelemetryRunner::Impl::Impl() :
    m_running(false),
    m_deadlineTimer(THREAD_INTERVAL_MS)
{
    m_apiCmdMap[PingApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleIngressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetBpSecApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleIngressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[UpdateBpSecApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleIngressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[UploadContactPlanApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleRouterCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetExpiringStorageApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleStorageCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetStorageApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleStorageCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[SetMaxSendRateApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleEgressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetOutductsApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleEgressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetOutductCapabilitiesApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleEgressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetInductsApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::HandleIngressCommand, this, boost::placeholders::_1, boost::placeholders::_2);
    m_apiCmdMap[GetHdtnConfigApiCommand_t::name] = boost::bind(&TelemetryRunner::Impl::ProcessHdtnConfigRequest, this, boost::placeholders::_1, boost::placeholders::_2);
}

bool TelemetryRunner::Impl::Init(const HdtnConfig &hdtnConfig, zmq::context_t *inprocContextPtr, TelemetryRunnerProgramOptions &options) {
    if ((inprocContextPtr == NULL) && (!options.m_hdtnDistributedConfigPtr)) {
        LOG_ERROR(subprocess) << "Error in TelemetryRunner Init: using distributed mode but Hdtn Distributed Config is invalid";
        return false;
    }
    m_hdtnConfig = hdtnConfig;
    { // add hdtn version to config, and preserialize it to json once for all connecting web GUIs
        boost::property_tree::ptree pt = hdtnConfig.GetNewPropertyTree();
        pt.put("hdtnVersionString", hdtn::Logger::GetHdtnVersionAsString());
        m_hdtnConfigJsonPtr = std::make_shared<std::string>(JsonSerializable::PtToJsonString(pt));
    }

#ifdef USE_WEB_INTERFACE
# ifdef BEAST_WEBSOCKET_SERVER_SUPPORT_SSL
    boost::asio::ssl::context sslContext(boost::asio::ssl::context::sslv23_server);
    if (options.m_sslPaths.m_valid) {
        try {
            // tcpclv4 server supports tls 1.2 and 1.3 only
            sslContext.set_options(
                boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 | boost::asio::ssl::context::no_tlsv1_1 | boost::asio::ssl::context::single_dh_use);
            if (options.m_sslPaths.m_certificateChainPemFile.size()) {
                sslContext.use_certificate_chain_file(options.m_sslPaths.m_certificateChainPemFile.string());
            }
            else {
                sslContext.use_certificate_file(options.m_sslPaths.m_certificatePemFile.string(), boost::asio::ssl::context::pem);
            }
            sslContext.use_private_key_file(options.m_sslPaths.m_privateKeyPemFile.string(), boost::asio::ssl::context::pem);
            sslContext.use_tmp_dh_file(options.m_sslPaths.m_diffieHellmanParametersPemFile.string()); //"C:/hdtn_ssl_certificates/dh4096.pem"
        }
        catch (boost::system::system_error &e) {
            LOG_ERROR(subprocess) << "SSL error in TelemetryRunner Init: " << e.what();
            return false;
        }
    }
    m_websocketServerPtr = boost::make_unique<BeastWebsocketServer>(std::move(sslContext), options.m_sslPaths.m_valid);
# else
    m_websocketServerPtr = boost::make_unique<BeastWebsocketServer>();
# endif // #ifdef BEAST_WEBSOCKET_SERVER_SUPPORT_SSL
    m_websocketServerPtr->Init(options.m_guiDocumentRoot, options.m_guiPortNumber,
        boost::bind(&TelemetryRunner::Impl::OnNewWebsocketConnectionCallback, this, boost::placeholders::_1),
        boost::bind(&TelemetryRunner::Impl::OnNewWebsocketDataReceivedCallback, this, boost::placeholders::_1, boost::placeholders::_2));
#endif // USE_WEB_INTERFACE

#ifdef DO_STATS_LOGGING
    m_telemetryLoggerPtr = boost::make_unique<TelemetryLogger>();
#endif

    m_running = true;
    m_threadPtr = boost::make_unique<boost::thread>(
        boost::bind(&TelemetryRunner::Impl::ThreadFunc, this, options.m_hdtnDistributedConfigPtr, inprocContextPtr)); // create and start the worker thread
    return true;
}

void TelemetryRunner::Impl::OnNewWebsocketConnectionCallback(WebsocketSessionPublicBase &conn) {
    // std::cout << "newconn\n";
    conn.AsyncSendTextData(std::shared_ptr<std::string>(m_hdtnConfigJsonPtr));
    {
        boost::mutex::scoped_lock lock(m_lastSerializedAllOutductCapabilitiesMutex);
        if (m_lastJsonSerializedAllOutductCapabilitiesPtr && m_lastJsonSerializedAllOutductCapabilitiesPtr->size()) {
            conn.AsyncSendTextData(std::shared_ptr<std::string>(m_lastJsonSerializedAllOutductCapabilitiesPtr)); // create copy of shared ptr and move the copy in
        }
    }
}

bool TelemetryRunner::Impl::OnNewWebsocketDataReceivedCallback(WebsocketSessionPublicBase &conn, std::string &receivedString) {
    zmq::message_t connectionID;
    connectionID.copy(GUI_REQ_CONN_ID);
    if (!OnApiRequest(std::move(receivedString), std::move(connectionID))) {
        LOG_ERROR(subprocess) << "failed to handle API request from websocket";
    }
    return true; // keep open
}

bool TelemetryRunner::Impl::HandleIngressCommand(std::string &movablePayload, zmq::message_t& connectionID) {
    return m_ingressConnection->EnqueueApiPayload(std::move(movablePayload), std::move(connectionID));
}

bool TelemetryRunner::Impl::HandleRouterCommand(std::string &movablePayload, zmq::message_t& connectionID) {
    return m_routerConnection->EnqueueApiPayload(std::move(movablePayload), std::move(connectionID));
}

bool TelemetryRunner::Impl::HandleStorageCommand(std::string &movablePayload, zmq::message_t& connectionID) {
    return m_storageConnection->EnqueueApiPayload(std::move(movablePayload), std::move(connectionID));
}

bool TelemetryRunner::Impl::HandleEgressCommand(std::string &movablePayload, zmq::message_t& connectionID) {
    return m_egressConnection->EnqueueApiPayload(std::move(movablePayload), std::move(connectionID));
}

bool TelemetryRunner::Impl::OnApiRequest(std::string &&msgJson, zmq::message_t&& connectionID) {
    std::shared_ptr<ApiCommand_t> apiCmdPtr = ApiCommand_t::CreateFromJson(msgJson);
    if (!apiCmdPtr) {
        LOG_ERROR(subprocess) << "error parsing received api json message.. got\n"
            << msgJson;
        return false;
    }
    TelemetryRunner::Impl::ApiCommandFunctionMap_t::iterator it = m_apiCmdMap.find(apiCmdPtr->m_apiCall);
    if (it == m_apiCmdMap.end()) {
        LOG_ERROR(subprocess) << "Unrecognized API command " << apiCmdPtr->m_apiCall;
        return false;
    }
    return it->second(msgJson, connectionID); // note: msgJson will still be moved (boost::function doesn't support r-value references as parameters)
}

bool TelemetryRunner::Impl::ProcessHdtnConfigRequest(std::string &movablePayload, zmq::message_t& connectionID) {
     //moveablePayload parameter (not used)
    // Processes external API request by retrieving HDTN config and sending it back to the requester
   
    zmq::message_t blank;
    zmq::message_t response(m_hdtnConfigJsonPtr->c_str(), m_hdtnConfigJsonPtr->size());
    
    m_apiConnection->SendZmqMessage(std::move(connectionID), true);
    m_apiConnection->SendZmqMessage(std::move(blank), true);

    return m_apiConnection->SendZmqMessage(std::move(response), false);
  
}


static bool ReceivedIngress(unsigned int mask) {
    return (mask & REC_INGRESS);
}

static bool ReceivedEgress(unsigned int mask) {
    return (mask & REC_EGRESS);
}

static bool ReceivedStorage(unsigned int mask) {
    return (mask & REC_STORAGE);
}

static bool ReceivedAllRequired(unsigned int mask) {
    return ReceivedStorage(mask) && ReceivedEgress(mask) && ReceivedIngress(mask);
}

void TelemetryRunner::Impl::ThreadFunc(const HdtnDistributedConfig_ptr &hdtnDistributedConfigPtr, zmq::context_t *inprocContextPtr) {
    ThreadNamer::SetThisThreadName("TelemetryRunner");
    // Create and initialize connections
    try {
        if (inprocContextPtr) {
            m_ingressConnection = boost::make_unique<TelemetryConnection>(
                "inproc://connecting_telem_to_from_bound_ingress",
                inprocContextPtr,
                zmq::socket_type::pair
            );
            m_egressConnection = boost::make_unique<TelemetryConnection>(
                "inproc://connecting_telem_to_from_bound_egress",
                inprocContextPtr,
                zmq::socket_type::pair
            );
            m_storageConnection = boost::make_unique<TelemetryConnection>(
                "inproc://connecting_telem_to_from_bound_storage",
                inprocContextPtr,
                zmq::socket_type::pair
            );
            m_routerConnection = boost::make_unique<TelemetryConnection>(
                "inproc://connecting_telem_to_from_bound_router",
                inprocContextPtr,
                zmq::socket_type::pair
            );
        }
        else {
            const std::string connect_connectingTelemToFromBoundIngressPath(
                std::string("tcp://") +
                hdtnDistributedConfigPtr->m_zmqIngressAddress +
                std::string(":") +
                boost::lexical_cast<std::string>(hdtnDistributedConfigPtr->m_zmqConnectingTelemToFromBoundIngressPortPath));
            const std::string connect_connectingTelemToFromBoundEgressPath(
                std::string("tcp://") +
                hdtnDistributedConfigPtr->m_zmqEgressAddress +
                std::string(":") +
                boost::lexical_cast<std::string>(hdtnDistributedConfigPtr->m_zmqConnectingTelemToFromBoundEgressPortPath));
            const std::string connect_connectingTelemToFromBoundStoragePath(
                std::string("tcp://") +
                hdtnDistributedConfigPtr->m_zmqStorageAddress +
                std::string(":") +
                boost::lexical_cast<std::string>(hdtnDistributedConfigPtr->m_zmqConnectingTelemToFromBoundStoragePortPath));
            const std::string connect_connectingTelemToFromBoundRouterPath(
                std::string("tcp://") +
                hdtnDistributedConfigPtr->m_zmqRouterAddress +
                std::string(":") +
                boost::lexical_cast<std::string>(hdtnDistributedConfigPtr->m_zmqConnectingTelemToFromBoundRouterPortPath));

            m_ingressConnection = boost::make_unique<TelemetryConnection>(connect_connectingTelemToFromBoundIngressPath, nullptr, zmq::socket_type::req);
            m_egressConnection = boost::make_unique<TelemetryConnection>(connect_connectingTelemToFromBoundEgressPath, nullptr, zmq::socket_type::req);
            m_storageConnection = boost::make_unique<TelemetryConnection>(connect_connectingTelemToFromBoundStoragePath, nullptr, zmq::socket_type::req);
            m_routerConnection = boost::make_unique<TelemetryConnection>(connect_connectingTelemToFromBoundRouterPath, nullptr, zmq::socket_type::req);
        }

        const std::string connect_connectingTelemToApi(
            std::string("tcp://*:") +
            boost::lexical_cast<std::string>(m_hdtnConfig.m_zmqBoundTelemApiPortPath));
        m_apiConnection = boost::make_unique<TelemetryConnection>(
            connect_connectingTelemToApi,
            nullptr,
            zmq::socket_type::router,
            true);
    }
    catch (const std::exception &e) {
        LOG_ERROR(subprocess) << e.what();
        return;
    }

    // Create poller and add each connection
    TelemetryConnectionPoller poller;
    poller.AddConnection(*m_ingressConnection);
    poller.AddConnection(*m_egressConnection);
    poller.AddConnection(*m_storageConnection);
    poller.AddConnection(*m_routerConnection);

    TelemetryConnectionPoller apiPoller;
    apiPoller.AddConnection(*m_apiConnection);

    // Start loop to begin polling
    
    while (m_running.load(std::memory_order_acquire)) {
        if (!m_deadlineTimer.SleepUntilNextInterval()) {
            break;
        }

        // First, poll for API requests
        // Keep polling until there are no more messages or the number of poll attempts are exceeded
        bool success = false;
        uint8_t count = 0;
        do {
            success = apiPoller.PollConnections(API_TIMEOUT_POLL_MS);
            if (success) {
                // Router sockets send three message parts:
                // 1. The connection identity
                // 2. The message envelope (don't care)
                // 3. The message body
                zmq::message_t connectionID = m_apiConnection->ReadMessage();
                m_apiConnection->ReadMessage();
                zmq::message_t msgJson = m_apiConnection->ReadMessage();
                OnApiRequest(std::move(msgJson.to_string()), std::move(connectionID));
            }
            ++count;
        } while (success && count < API_NUM_POLL_ATTEMPTS);

        // Queue requests for normal telemetry (for logging + GUI)
        QueueTelemRequests();

        // Send pending requests to all hdtn modules
        m_storageConnection->SendRequests();
        m_egressConnection->SendRequests();
        m_ingressConnection->SendRequests();
        m_routerConnection->SendRequests();

        // Poll for responses from all modules
        unsigned int receiveEventsMask = 0;
        AllInductTelemetry_t inductTelem;
        AllOutductTelemetry_t outductTelem;
        StorageTelemetry_t storageTelem;
        for (unsigned int attempt = 0; attempt < TELEM_NUM_POLL_ATTEMPTS; ++attempt) {
            if (ReceivedAllRequired(receiveEventsMask)) {
                break;
            }

            if (!poller.PollConnections(TELEM_TIMEOUT_POLL_MS)) {
                continue;
            }

            if (poller.HasNewMessage(*m_ingressConnection)) {
                receiveEventsMask |= REC_INGRESS;
                bool more;
                do {
                    zmq::message_t connectionID = m_ingressConnection->ReadMessage();
                    std::string apiCall = m_ingressConnection->ReadMessage().to_string();
                    zmq::message_t responseMsg = m_ingressConnection->ReadMessage();
                    more = responseMsg.more();

                    // Handle the response depending on who sent it
                    if (connectionID == TELEM_REQ_CONN_ID) {
                        OnNewJsonTelemetry((const char *)responseMsg.data(), responseMsg.size());
                        if (apiCall == GetInductsApiCommand_t::name) {
                            if (!inductTelem.SetValuesFromJsonCharArray((const char *)responseMsg.data(), responseMsg.size())) {
                                LOG_ERROR(subprocess) << "cannot deserialize AllInductTelemetry_t";
                            }
                        }
                    } else if (connectionID == GUI_REQ_CONN_ID) {
                        // Request came from GUI; No action needed
                    } else {
                        // Request came from external API. Respond to the appropriate connection.
                        zmq::message_t blank;
                        m_apiConnection->SendZmqMessage(std::move(connectionID), true);
                        m_apiConnection->SendZmqMessage(std::move(blank), true);
                        m_apiConnection->SendZmqMessage(std::move(responseMsg), false);
                    }
                } while (more);
            }
            if (poller.HasNewMessage(*m_egressConnection)) {
                receiveEventsMask |= REC_EGRESS;

                bool more;
                do {
                    zmq::message_t connectionID = m_egressConnection->ReadMessage();
                    std::string apiCall = m_egressConnection->ReadMessage().to_string();
                    zmq::message_t responseMsg = m_egressConnection->ReadMessage();
                    more = responseMsg.more();    

                    // Handle the response depending on who sent it
                    if (connectionID == TELEM_REQ_CONN_ID) {
                        if (apiCall == GetOutductCapabilitiesApiCommand_t::name) {
                            ApiResp_t response;
                            if (response.SetValuesFromJson(responseMsg.to_string()) && response.m_success == false) {
                                // There was no outduct capability data. Do nothing.
                            } else {
                                boost::mutex::scoped_lock lock(m_lastSerializedAllOutductCapabilitiesMutex);
                                m_lastJsonSerializedAllOutductCapabilitiesPtr = std::make_shared<std::string>(responseMsg.to_string());
                                OnNewJsonTelemetry((const char *)responseMsg.data(), responseMsg.size());
                            }
                        }
                        else if (apiCall == GetOutductsApiCommand_t::name) {
                            if (!outductTelem.SetValuesFromJsonCharArray((const char *)responseMsg.data(), responseMsg.size())) {
                                LOG_ERROR(subprocess) << "cannot deserialize AllOutductTelemetry_t";
                            }
                            OnNewJsonTelemetry((const char *)responseMsg.data(), responseMsg.size());
                        }
                    } else if (connectionID == GUI_REQ_CONN_ID) {
                        // Request came from GUI; No action needed
                    } else {
                        // Request came from external API. Respond to the appropriate connection.
                        zmq::message_t blank;
                        m_apiConnection->SendZmqMessage(std::move(connectionID), true);
                        m_apiConnection->SendZmqMessage(std::move(blank), true);
                        m_apiConnection->SendZmqMessage(std::move(responseMsg), false);
                    }
                } while (more);
            }
            if (poller.HasNewMessage(*m_storageConnection)) {
                receiveEventsMask |= REC_STORAGE;
                bool more;
                do {
                    zmq::message_t connectionID = m_storageConnection->ReadMessage();
                    std::string apiCall = m_storageConnection->ReadMessage().to_string();
                    zmq::message_t responseMsg = m_storageConnection->ReadMessage();
                    more = responseMsg.more();    

                    // Handle the response depending on who sent it
                    if (connectionID == TELEM_REQ_CONN_ID) {
                        OnNewJsonTelemetry((const char *)responseMsg.data(), responseMsg.size());
                        if (apiCall == GetStorageApiCommand_t::name) {
                            if (!storageTelem.SetValuesFromJsonCharArray((const char *)responseMsg.data(), responseMsg.size())) {
                                LOG_ERROR(subprocess) << "cannot deserialize StorageTelemetry_t";
                            }
                        }
                    } else if (connectionID == GUI_REQ_CONN_ID) {
                        // Request came from GUI; No action needed
                    } else {
                        // Request came from external API. Respond to the appropraite connection.
                        zmq::message_t blank;
                        m_apiConnection->SendZmqMessage(std::move(connectionID), true);
                        m_apiConnection->SendZmqMessage(std::move(blank), true);
                        m_apiConnection->SendZmqMessage(std::move(responseMsg), false);
                    }
                } while (more);
            }
            if (poller.HasNewMessage(*m_routerConnection)) {
                bool more;
                do {
                    zmq::message_t connectionID = m_routerConnection->ReadMessage();
                    std::string apiCall = m_routerConnection->ReadMessage().to_string();
                    zmq::message_t responseMsg = m_routerConnection->ReadMessage();
                    more = responseMsg.more();    

                    // Handle the response depending on who sent it
                   if (connectionID == GUI_REQ_CONN_ID) {
                        // Request came from GUI; No action needed
                    } else {
                        LOG_INFO(subprocess) << "Sending to API";
                        // Request came from external API. Respond to the appropraite connection.
                        zmq::message_t blank;
                        m_apiConnection->SendZmqMessage(std::move(connectionID), true);
                        m_apiConnection->SendZmqMessage(std::move(blank), true);
                        m_apiConnection->SendZmqMessage(std::move(responseMsg), false);
                    }
                } while (more);
            }
        }
        if (ReceivedAllRequired(receiveEventsMask)) {
            if (m_telemetryLoggerPtr) {
                m_telemetryLoggerPtr->LogTelemetry(inductTelem, outductTelem, storageTelem);
            }
        }
        else {
            LOG_WARNING(subprocess) << "did not get telemetry from all modules. missing:" << (ReceivedEgress(receiveEventsMask) ? "" : " egress") << (ReceivedIngress(receiveEventsMask) ? "" : " ingress") << (ReceivedStorage(receiveEventsMask) ? "" : " storage");
        }
    }
    LOG_DEBUG(subprocess) << "ThreadFunc exiting";
}

void TelemetryRunner::Impl::QueueTelemRequests() {
    std::string request = GetStorageApiCommand_t().ToJson();
    zmq::message_t connectionId;
    connectionId.copy(TELEM_REQ_CONN_ID);
    m_storageConnection->EnqueueApiPayload(std::move(request), std::move(connectionId));

    request = GetOutductCapabilitiesApiCommand_t().ToJson();
    connectionId.copy(TELEM_REQ_CONN_ID);
    m_egressConnection->EnqueueApiPayload(std::move(request), std::move(connectionId));

    request = GetOutductsApiCommand_t().ToJson();
    connectionId.copy(TELEM_REQ_CONN_ID);
    m_egressConnection->EnqueueApiPayload(std::move(request), std::move(connectionId));

    request = GetInductsApiCommand_t().ToJson();
    connectionId.copy(TELEM_REQ_CONN_ID);
    m_ingressConnection->EnqueueApiPayload(std::move(request), std::move(connectionId));
}

void TelemetryRunner::Impl::OnNewJsonTelemetry(const char *buffer, uint64_t bufferSize) {
#ifdef USE_WEB_INTERFACE
    if (m_websocketServerPtr) {
        std::shared_ptr<std::string> strPtr = std::make_shared<std::string>(buffer, bufferSize);
        m_websocketServerPtr->SendTextDataToActiveWebsockets(strPtr);
    }
#endif
}

void TelemetryRunner::Impl::Stop() {
    m_running = false;
    m_deadlineTimer.Disable();
    m_deadlineTimer.Cancel();
    if (m_threadPtr) {
        try {
            m_threadPtr->join();
        }
        catch (const boost::thread_resource_error &) {
            LOG_ERROR(subprocess) << "error stopping TelemetryRunner thread";
        }
        m_threadPtr.reset(); // delete it
    }
#ifdef USE_WEB_INTERFACE
    // stop websocket after thread
    if (m_websocketServerPtr) {
        m_websocketServerPtr->Stop();
        m_websocketServerPtr.reset();
    }
#endif
}
