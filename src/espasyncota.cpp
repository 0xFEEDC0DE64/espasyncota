#include "espasyncota.h"

// system includes
#include <cassert>

// esp-idf includes
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>

// 3rdparty lib includes
#include <fmt/core.h>

// local includes
#include "cleanuphelper.h"
#include "esphttpdutils.h"
#include "tickchrono.h"

using namespace std::chrono_literals;

namespace {
constexpr const char * const TAG = "ASYNC_OTA";

constexpr int TASK_RUNNING_BIT = BIT0;
constexpr int START_REQUEST_BIT = BIT1;
constexpr int REQUEST_RUNNING_BIT = BIT2;
constexpr int REQUEST_FINISHED_BIT = BIT3;
constexpr int REQUEST_SUCCEEDED_BIT = BIT4;
constexpr int END_TASK_BIT = BIT5;
constexpr int TASK_ENDED_BIT = BIT6;
constexpr int ABORT_REQUEST_BIT = BIT7;
} // namespace

EspAsyncOta::EspAsyncOta(const char *taskName, uint32_t stackSize, espcpputils::CoreAffinity coreAffinity) :
    m_taskName{taskName},
    m_stackSize{stackSize},
    m_coreAffinity{coreAffinity}
{
    assert(m_eventGroup.handle);
}

EspAsyncOta::~EspAsyncOta()
{
    endTask();
}

tl::expected<void, std::string> EspAsyncOta::startTask()
{
    if (m_taskHandle)
    {
        constexpr auto msg = "ota task handle is not null";
        ESP_LOGW(TAG, "%s", msg);
        return tl::make_unexpected(msg);
    }

    if (const auto bits = m_eventGroup.getBits(); bits & TASK_RUNNING_BIT)
    {
        constexpr auto msg = "ota task already running";
        ESP_LOGW(TAG, "%s", msg);
        return tl::make_unexpected(msg);
    }

    m_eventGroup.clearBits(TASK_RUNNING_BIT | START_REQUEST_BIT | REQUEST_RUNNING_BIT | REQUEST_FINISHED_BIT | REQUEST_SUCCEEDED_BIT | END_TASK_BIT | TASK_ENDED_BIT | ABORT_REQUEST_BIT);

    const auto result = espcpputils::createTask(otaTask, m_taskName, m_stackSize, this, 10, &m_taskHandle, m_coreAffinity);
    if (result != pdPASS)
    {
        auto msg = fmt::format("failed creating ota task {}", result);
        ESP_LOGE(TAG, "%.*s", msg.size(), msg.data());
        return tl::make_unexpected(std::move(msg));
    }

    if (!m_taskHandle)
    {
        constexpr auto msg = "ota task handle is null";
        ESP_LOGW(TAG, "%s", msg);
        return tl::make_unexpected(msg);
    }

    ESP_LOGD(TAG, "created ota task %s", m_taskName);

    if (const auto bits = m_eventGroup.waitBits(TASK_RUNNING_BIT, false, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_RUNNING_BIT)
        return {};

    ESP_LOGW(TAG, "ota task %s TASK_RUNNING_BIT bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = m_eventGroup.waitBits(TASK_RUNNING_BIT, false, false, portMAX_DELAY);
            bits & TASK_RUNNING_BIT)
            break;

    return {};
}

tl::expected<void, std::string> EspAsyncOta::endTask()
{
    if (const auto bits = m_eventGroup.getBits();
        !(bits & TASK_RUNNING_BIT))
        return {};
    else if (bits & END_TASK_BIT)
    {
        constexpr auto msg = "Another end request is already pending";
        ESP_LOGE(TAG, "%s", msg);
        return tl::make_unexpected(msg);
    }

    m_eventGroup.setBits(END_TASK_BIT);

    if (const auto bits = m_eventGroup.waitBits(TASK_ENDED_BIT, true, false, std::chrono::ceil<espcpputils::ticks>(1s).count());
        bits & TASK_ENDED_BIT)
    {
        ESP_LOGD(TAG, "ota task %s ended", m_taskName);
        return {};
    }

    ESP_LOGW(TAG, "ota task %s TASK_ENDED_BIT bit not yet set...", m_taskName);

    while (true)
        if (const auto bits = m_eventGroup.waitBits(TASK_ENDED_BIT, true, false, portMAX_DELAY);
            bits & TASK_ENDED_BIT)
            break;

    ESP_LOGD(TAG, "ota task %s ended", m_taskName);

    return {};
}

OtaCloudUpdateStatus EspAsyncOta::status() const
{
    if (const auto bits = m_eventGroup.getBits(); !(bits & TASK_RUNNING_BIT))
    {
        return OtaCloudUpdateStatus::Idle;
    }
    else if (bits & (START_REQUEST_BIT | REQUEST_RUNNING_BIT))
    {
        return OtaCloudUpdateStatus::Updating;
    }
    else if (bits & REQUEST_FINISHED_BIT)
    {
        if (bits & REQUEST_SUCCEEDED_BIT)
            return OtaCloudUpdateStatus::Succeeded;
        else
            return OtaCloudUpdateStatus::Failed;
    }

    return OtaCloudUpdateStatus::Idle;
}

tl::expected<void, std::string> EspAsyncOta::trigger(std::string_view url, std::string_view cert_pem,
                                                    std::string_view client_key, std::string_view client_cert)
{
    if (!m_taskHandle)
    {
        if (auto result = startTask(); !result)
            return tl::make_unexpected(std::move(result).error());
    }

    if (const auto bits = m_eventGroup.getBits(); !(bits & TASK_RUNNING_BIT))
        return tl::make_unexpected("ota cloud task not running");
    else if (bits & (START_REQUEST_BIT | REQUEST_RUNNING_BIT))
        return tl::make_unexpected("ota cloud already running");
    else if (bits & REQUEST_FINISHED_BIT)
        return tl::make_unexpected("ota cloud not fully finished, try again");
    else
        assert(!(bits & REQUEST_SUCCEEDED_BIT));

    if (url.empty())
        return tl::make_unexpected("empty firmware url");

    if (const auto result = esphttpdutils::urlverify(url); !result)
        return tl::make_unexpected(fmt::format("could not verify firmware url: {}", result.error()));

    m_url = std::string{url};
    m_cert_pem = cert_pem;
    m_client_key = client_key;
    m_client_cert = client_cert;

    m_eventGroup.setBits(START_REQUEST_BIT);
    ESP_LOGI(TAG, "ota cloud update triggered");

    return {};
}

tl::expected<void, std::string> EspAsyncOta::abort()
{
    if (const auto bits = m_eventGroup.getBits(); !(bits & (START_REQUEST_BIT | REQUEST_RUNNING_BIT)))
        return tl::make_unexpected("no ota job is running!");
    else if (bits & ABORT_REQUEST_BIT)
        return tl::make_unexpected("an abort has already been requested!");

    m_eventGroup.setBits(ABORT_REQUEST_BIT);
    ESP_LOGI(TAG, "ota cloud update abort requested");

    return {};
}

void EspAsyncOta::update()
{
    //if (!m_taskHandle)
    //{
    //    if (const auto result = startTask(); !result)
    //    {
    //        ESP_LOGE(TAG, "starting OTA task failed: %.*s", result.error().size(), result.error().data());
    //        return;
    //    }
    //}

    if (const auto bits = m_eventGroup.getBits(); bits & (START_REQUEST_BIT | REQUEST_RUNNING_BIT))
    {
        if (!m_lastInfo || espchrono::ago(*m_lastInfo) >= 1s)
        {
            m_lastInfo = espchrono::millis_clock::now();
            if (m_totalSize)
                ESP_LOGI(TAG, "OTA Progress %i of %i (%.2f%%)", m_progress, *m_totalSize, 100.f*m_progress / *m_totalSize);
            else
                ESP_LOGI(TAG, "OTA Progress %i of unknown", m_progress);
        }
    }
    else if (bits & REQUEST_FINISHED_BIT)
    {
        if (m_finishedTs)
        {
            if (espchrono::ago(*m_finishedTs) > 5s)
            {
                m_finishedTs = std::nullopt;

                if (bits & REQUEST_SUCCEEDED_BIT)
                    esp_restart();

                m_eventGroup.clearBits(REQUEST_FINISHED_BIT|REQUEST_SUCCEEDED_BIT);

                m_appDesc = std::nullopt;
            }
        }
        else
        {
            m_finishedTs = espchrono::millis_clock::now();
            if (m_totalSize)
                ESP_LOGI(TAG, "OTA Finished %i of %i (%.2f%%)", m_progress, *m_totalSize, 100.f*m_progress / *m_totalSize);
            else
                ESP_LOGI(TAG, "OTA Finished %i of unknown", m_progress);
        }
    }
}

/*static*/ void EspAsyncOta::otaTask(void *arg)
{
    auto _this = reinterpret_cast<EspAsyncOta*>(arg);

    assert(_this);

    _this->otaTask();
}

void EspAsyncOta::otaTask()
{
    auto helper = cpputils::makeCleanupHelper([&](){
        m_eventGroup.clearBits(TASK_RUNNING_BIT);
        m_taskHandle = NULL;
        vTaskDelete(NULL);
    });

    m_eventGroup.setBits(TASK_RUNNING_BIT);

    while (true)
    {
        {
            const auto bits = m_eventGroup.waitBits(START_REQUEST_BIT, true, true, portMAX_DELAY);
            if (!(bits & START_REQUEST_BIT))
                continue;
        }

        {
            const auto bits = m_eventGroup.getBits();
            assert(!(bits & START_REQUEST_BIT));
            assert(!(bits & REQUEST_RUNNING_BIT));
            assert(!(bits & REQUEST_FINISHED_BIT));
            assert(!(bits & REQUEST_SUCCEEDED_BIT));
        }

        m_progress = 0;

        m_eventGroup.setBits(REQUEST_RUNNING_BIT);

        auto helper2 = cpputils::makeCleanupHelper([&](){
            m_eventGroup.clearBits(REQUEST_RUNNING_BIT | ABORT_REQUEST_BIT);
            m_eventGroup.setBits(REQUEST_FINISHED_BIT);
        });

        esp_http_client_config_t config{};
        config.url = m_url.c_str();
        if (!m_cert_pem.empty())
        {
            config.cert_pem = m_cert_pem.data();
            config.cert_len = m_cert_pem.size();
        }
        config.skip_cert_common_name_check = false;

        if (!m_client_key.empty())
        {
            config.client_key_pem = m_client_key.data();
            config.client_key_len = m_client_key.size();
        }

        if (!m_client_cert.empty())
        {
            config.client_cert_pem = m_client_cert.data();
            config.client_cert_len = m_client_cert.size();
        }

        esp_https_ota_config_t ota_config = {
            .http_config = &config,
        };

        esp_https_ota_handle_t https_ota_handle{};

        ESP_LOGI(TAG, "esp_https_ota_begin()... (%s)", m_url.c_str());

        {
            const auto result = esp_https_ota_begin(&ota_config, &https_ota_handle);
            ESP_LOG_LEVEL_LOCAL((result == ESP_OK ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "esp_https_ota_begin() returned: %s", esp_err_to_name(result));
            if (result != ESP_OK)
            {
                m_message = fmt::format("{}() failed with {} (at {})", "esp_https_ota_begin",
                                        esp_err_to_name(result), std::chrono::floor<std::chrono::milliseconds>(espchrono::millis_clock::now().time_since_epoch()).count());
                continue;
            }
        }

        if (https_ota_handle == NULL)
        {
            ESP_LOGE(TAG, "ota handle invalid");
            m_message = fmt::format("ota handle invalid (at {})",
                                    std::chrono::floor<std::chrono::milliseconds>(espchrono::millis_clock::now().time_since_epoch()).count());
            continue;
        }

        {
            ESP_LOGI(TAG, "esp_https_ota_get_img_desc()...");
            esp_app_desc_t new_app_info;
            const auto result = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
            ESP_LOG_LEVEL_LOCAL((result == ESP_OK ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "esp_https_ota_get_img_desc() returned: %s", esp_err_to_name(result));
            if (result == ESP_OK)
                m_appDesc = new_app_info;
            else
                m_appDesc = std::nullopt;
        }

        ESP_LOGI(TAG, "esp_https_ota_get_image_size()...");
        {
            const auto size = esp_https_ota_get_image_size(https_ota_handle);
            ESP_LOG_LEVEL_LOCAL((size > 0 ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "esp_https_ota_get_image_size() returned: %i", size);
            if (size > 0)
                m_totalSize = size;
        }

        ESP_LOGI(TAG, "esp_https_ota_perform()...");
        bool aborted{};
        esp_err_t ota_perform_err;
        {
            espchrono::millis_clock::time_point lastYield = espchrono::millis_clock::now();
            while (true)
            {
                ota_perform_err = esp_https_ota_perform(https_ota_handle);
                if (ota_perform_err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
                    break;

                m_progress = esp_https_ota_get_image_len_read(https_ota_handle);
                if (espchrono::ago(lastYield) >= 1s)
                {
                    lastYield = espchrono::millis_clock::now();
                    vPortYield();
                }

                if (m_eventGroup.clearBits(ABORT_REQUEST_BIT) & ABORT_REQUEST_BIT)
                {
                    ESP_LOGW(TAG, "abort request received");
                    aborted = true;
                    m_message = "Requested abort";
                    ota_perform_err = ESP_FAIL;
                    break;
                }
            }
        }
        ESP_LOG_LEVEL_LOCAL((ota_perform_err == ESP_OK ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "esp_https_ota_perform() returned: %s", esp_err_to_name(ota_perform_err));

        ESP_LOGI(TAG, "esp_https_ota_finish()...");
        const auto ota_finish_err = esp_https_ota_finish(https_ota_handle);
        ESP_LOG_LEVEL_LOCAL((ota_finish_err == ESP_OK ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "esp_https_ota_finish() returned: %s", esp_err_to_name(ota_finish_err));


        if (!aborted)
        {
            if (ota_perform_err != ESP_OK)
                m_message = fmt::format("{}() failed with {} (at {})",
                                        "esp_https_ota_perform",
                                        esp_err_to_name(ota_perform_err),
                                        std::chrono::floor<std::chrono::milliseconds>(espchrono::millis_clock::now().time_since_epoch()).count());
            else if (ota_finish_err != ESP_OK)
                m_message = fmt::format("{}() failed with {} (at {})",
                                        "esp_https_ota_finish",
                                        esp_err_to_name(ota_finish_err),
                                        std::chrono::floor<std::chrono::milliseconds>(espchrono::millis_clock::now().time_since_epoch()).count());
            else
                m_message.clear();
        }

        if (ota_perform_err == ESP_OK &&
            ota_finish_err == ESP_OK)
            m_eventGroup.setBits(REQUEST_SUCCEEDED_BIT);
    }
}
