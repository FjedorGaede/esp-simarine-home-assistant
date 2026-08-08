#include "config.hpp"
#include "esp_system.h"
#include "mqtt_client.hpp"
#include "mqtt_logger.hpp"
#include "wifi_connector.hpp"
#include "wifi_utils.hpp"

#include "spymarine/spymarine.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <chrono>

namespace {
constexpr auto TAG = "spymarine";

void send_home_assistant_device_discovery(const spymarine::hub& hub,
                                          mqtt_client& client) {
  ESP_LOGI(TAG, "Sending Home Assistant device discovery messages");

  for (const auto& device : hub.devices() | std::views::filter(device_filter)) {
    const auto message =
        spymarine::make_home_assistant_device_discovery_message(device, hub);
    const auto published = client.publish(
        message.topic.c_str(), message.payload, mqtt_qos::at_least_once, false);
    if (published) {
      ESP_LOGI(TAG, "Device discovery message sent for device %d",
               spymarine::get_device_id(device));
    } else {
      ESP_LOGE(TAG, "Couldn't send device discovery message");
    }
  }
}

void publish_sensor_values(const spymarine::hub& hub, mqtt_client& client) {
  ESP_LOGI(TAG, "Sending Home Assistant sensor messags");

  for (const auto& device : hub.devices() | std::views::filter(device_filter)) {
    const auto message =
        spymarine::make_home_assistant_state_message(device, hub, state_config);
    client.publish(message.topic.c_str(), message.payload,
                   mqtt_qos::at_most_once, false);
  }
}

void process_sensor_values(spymarine::hub& hub, mqtt_client& client) {
  ESP_LOGI(TAG, "Start processing sensor values");

  std::atomic<bool> reinitialize = false;
  client.subscribe("homeassistant/status", mqtt_qos::at_least_once,
                   [&](std::string_view data) {
                     if (data == "online") {
                       reinitialize = true;
                     }
                   });

  using clock = std::chrono::steady_clock;
  auto last_update_time = clock::now();

  while (true) {
    if (reinitialize) {
      send_mqtt_logger_device_discovery();
      send_home_assistant_device_discovery(hub, client);
      hub.read_sensor_values();
      publish_sensor_values(hub, client);
      reinitialize = false;
    }

    hub.read_sensor_values();

    if ((clock::now() - last_update_time) >= sensor_update_interval) {
      publish_sensor_values(hub, client);
      hub.start_new_average_window();
      last_update_time = clock::now();
    }
  }
}

void log_system_info(const spymarine::hub& hub) {
  ESP_LOGI(TAG, "System Information");
  ESP_LOGI(TAG, "  Serial Number: %lu",
           static_cast<unsigned long>(hub.system().serial_number));
  ESP_LOGI(TAG, "  Firmware Version: %d.%d", hub.system().fw_version.major,
           hub.system().fw_version.minor);
}

bool start(mqtt_client& client) {
  ESP_LOGI(TAG, "Discovering Simarine system... ");
  const auto ip = spymarine::discover();
  if (!ip) {
    ESP_LOGE(TAG, "failed: %s", spymarine::error_message(ip.error()).c_str());
    return false;
  }
  ESP_LOGI(TAG, "done");

  ESP_LOGI(TAG, "Connecting... ");
  auto hub = spymarine::connect(*ip).and_then(spymarine::initialize_hub);
  if (!hub) {
    ESP_LOGE(TAG, "failed: %s", spymarine::error_message(hub.error()).c_str());
    return false;
  }
  ESP_LOGI(TAG, "done");

  if (hub->devices().empty()) {
    ESP_LOGE(TAG, "No devices found");
    return false;
  }

  log_system_info(*hub);
  process_sensor_values(*hub, client);
  return true;
}

} // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_connector connector{wifi_ssid, wifi_password};
  {
    auto wifi_connected_promise = connector.make_connected_promise();
    connector.start();
    wifi_connected_promise.wait();
  }

  mqtt_client mqtt_client{make_mqtt_config()};
  {
    auto mqtt_client_connected_promise = mqtt_client.make_connected_promise();
    mqtt_client.start();
    mqtt_client_connected_promise.wait();
  }

  setup_mqtt_logger(mqtt_client);
  send_mqtt_logger_device_discovery();

  if (!start(mqtt_client)) {
    ESP_LOGE(TAG, "Failed to initialize, restarting in a moment...");
    std::this_thread::sleep_for(wifi_retry_interval);
    esp_restart();
  }
}
