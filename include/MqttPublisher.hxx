/*
 * MqttPublisher.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_MQTT_PUBLISHER_HXX
#define AIMON_MQTT_PUBLISHER_HXX

#include <string>
#include <mutex>
#include <set>
#include "Models.hxx"
#include "ConfigManager.hxx"

struct mosquitto;

namespace aimon {

class MqttPublisher {
public:
    explicit MqttPublisher(const MqttConfig& config);
    ~MqttPublisher();

    bool start();
    void stop();

    void publishDiscovery();
    void publishState(const AggregateStatus& status);

private:
    bool publishMessage(const std::string& topic, const std::string& payload, bool retain = true);
    void publishSensorDiscovery(const std::string& sensorId,
                                const std::string& sensorName,
                                const std::string& stateTopic,
                                const std::string& unit = "",
                                const std::string& icon = "",
                                const std::string& jsonAttributesTopic = "");

    MqttConfig _config;
    struct mosquitto* _mosq = nullptr;
    bool _connected = false;
    std::mutex _mutex;
    std::set<std::string> _discoveredModels;
};

} // namespace aimon

#endif // AIMON_MQTT_PUBLISHER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
