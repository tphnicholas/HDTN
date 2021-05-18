#ifndef LTP_TIMER_MANAGER_H
#define LTP_TIMER_MANAGER_H 1

#include <boost/bimap.hpp>
#include <boost/asio.hpp>
#include <boost/function.hpp>

//Single threaded class designed to run and be called from ioService thread only


template <typename idType>
class LtpTimerManager {
private:
    LtpTimerManager();
public:
    typedef boost::function<void(idType serialNumber, std::vector<uint8_t> & userData)> LtpTimerExpiredCallback_t;
    LtpTimerManager(boost::asio::io_service & ioService, const boost::posix_time::time_duration & oneWayLightTime, const boost::posix_time::time_duration & oneWayMarginTime, const LtpTimerExpiredCallback_t & callback);
    ~LtpTimerManager();
    void Reset();
       
    bool StartTimer(const idType serialNumber, std::vector<uint8_t> userData = std::vector<uint8_t>());
    bool DeleteTimer(const idType serialNumber);
    bool Empty() const;
    //std::vector<uint8_t> & GetUserDataRef(const uint64_t serialNumber);
private:
    void OnTimerExpired(const boost::system::error_code& e);
private:
    boost::asio::deadline_timer m_deadlineTimer;
    const boost::posix_time::time_duration M_ONE_WAY_LIGHT_TIME;
    const boost::posix_time::time_duration M_ONE_WAY_MARGIN_TIME;
    const boost::posix_time::time_duration M_TRANSMISSION_TO_ACK_RECEIVED_TIME;
    const LtpTimerExpiredCallback_t m_ltpTimerExpiredCallbackFunction;
    boost::bimap<idType, boost::posix_time::ptime> m_bimapCheckpointSerialNumberToExpiry;
    std::map<idType, std::vector<uint8_t> > m_mapSerialNumberToUserData;
    idType m_activeSerialNumberBeingTimed;
    bool m_isTimerActive;
};

#endif // LTP_TIMER_MANAGER_H

