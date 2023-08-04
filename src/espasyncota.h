#pragma once

// system includes
#include <optional>
#include <string>
#include <expected>

// esp-idf includes
#include <esp_app_desc.h>

// local includes
#include "taskutils.h"
#include "wrappers/event_group.h"
#include "espchrono.h"
#include "cpptypesafeenum.h"

#define OtaCloudUpdateStatusValues(x) \
    x(Idle) \
    x(Updating) \
    x(Failed) \
    x(Succeeded) \
    x(NotReady) \
    x(Verifying)
DECLARE_TYPESAFE_ENUM(OtaCloudUpdateStatus, : uint8_t, OtaCloudUpdateStatusValues)

class EspAsyncOta
{
public:
    EspAsyncOta(const char *taskName="asyncOtaTask", uint32_t stackSize=4096, espcpputils::CoreAffinity coreAffinity=espcpputils::CoreAffinity::Core1);
    ~EspAsyncOta();

    std::expected<void, std::string> startTask();
    std::expected<void, std::string> endTask();

    int progress() const { return m_progress; }
    std::optional<int> totalSize() const { return m_totalSize; }
    void setTotalSize(int totalSize) { m_totalSize = totalSize; }
    const std::string &message() const { return m_message; }
    const std::optional<esp_app_desc_t> &appDesc() const { return m_appDesc; }
    OtaCloudUpdateStatus status() const;
    std::expected<void, std::string> trigger(std::string_view url, std::string_view cert_pem, bool use_global_ca,
                                             std::string_view client_key, std::string_view client_cert);
    std::expected<void, std::string> abort();

    void update();

private:
    static void otaTask(void *arg);
    void otaTask();

    const char * const m_taskName;
    const uint32_t m_stackSize;
    const espcpputils::CoreAffinity m_coreAffinity;

    int m_progress{};
    std::optional<int> m_totalSize;
    std::string m_message;
    std::optional<esp_app_desc_t> m_appDesc;

    espcpputils::event_group m_eventGroup;
    TaskHandle_t m_taskHandle{};

    std::optional<espchrono::millis_clock::time_point> m_finishedTs;
    std::optional<espchrono::millis_clock::time_point> m_lastInfo;

    std::string m_url;
    std::string_view m_cert_pem;
    bool m_use_global_ca;
    std::string_view m_client_key;
    std::string_view m_client_cert;
};

