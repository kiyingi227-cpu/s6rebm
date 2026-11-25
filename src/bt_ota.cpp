#include "bt_ota.h"
#include "board_config.h"

#include <Arduino.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_system.h"

using namespace BoardConfig;
using namespace OnyxOta;

// ============================================================================
// Embedded Firmware Descriptor (immutable in .rodata, verified by app & ESP32)
// ============================================================================
static const FwDescriptor kFirmwareDescriptor __attribute__((used)) = {
    "ONYX_FW_DESC_V1",
    static_cast<uint16_t>(ONYX_PRODUCT_ID),
    static_cast<uint8_t>(ONYX_HW_REV),
    static_cast<uint8_t>(ONYX_FW_MAJOR),
    static_cast<uint8_t>(ONYX_FW_MINOR),
    static_cast<uint8_t>(ONYX_FW_PATCH),
    0,
    "HK Onyx Studio 6",
    __DATE__,
    __TIME__,
};

const FwDescriptor &BtOta::firmwareDescriptor() {
  return kFirmwareDescriptor;
}

BtOta *BtOta::instance_ = nullptr;

namespace {

constexpr uint16_t GATTS_APP_ID = 0x59;

enum GattAttrIdx : uint8_t {
  IDX_SVC = 0,
  IDX_CHAR_INFO,
  IDX_CHAR_INFO_VAL,
  IDX_CHAR_INFO_CCCD,
  IDX_CHAR_CTRL,
  IDX_CHAR_CTRL_VAL,
  IDX_CHAR_CTRL_CCCD,
  IDX_CHAR_DATA,
  IDX_CHAR_DATA_VAL,
  IDX_NB
};

constexpr uint16_t PRIMARY_SERVICE_UUID   = ESP_GATT_UUID_PRI_SERVICE;
constexpr uint16_t CHAR_DECLARATION_UUID  = ESP_GATT_UUID_CHAR_DECLARE;
constexpr uint16_t CHAR_CLIENT_CFG_UUID   = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

constexpr uint8_t PROP_READ_NOTIFY        = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
constexpr uint8_t PROP_WRITE_NOTIFY       = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
constexpr uint8_t PROP_WRITE_NR           = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

static uint8_t sInfoCccd[2] = {0x00, 0x00};
static uint8_t sCtrlCccd[2] = {0x00, 0x00};
static uint8_t sDummyVal[4] = {0x00, 0x00, 0x00, 0x00};

static const esp_gatts_attr_db_t kGattDb[IDX_NB] = {
    // Primary Service
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&PRIMARY_SERVICE_UUID)),
         ESP_GATT_PERM_READ, sizeof(UUID_SERVICE_LE), sizeof(UUID_SERVICE_LE),
         const_cast<uint8_t *>(UUID_SERVICE_LE)}
    },

    // Characteristic 1: DEVICE_INFO (Read | Notify) — live snapshot on read
    [IDX_CHAR_INFO] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&CHAR_DECLARATION_UUID)),
         ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t),
         const_cast<uint8_t *>(&PROP_READ_NOTIFY)}
    },
    [IDX_CHAR_INFO_VAL] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, const_cast<uint8_t *>(UUID_CHAR_INFO_LE),
         ESP_GATT_PERM_READ, sizeof(DeviceInfoPayload), sizeof(sDummyVal), sDummyVal}
    },
    [IDX_CHAR_INFO_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&CHAR_CLIENT_CFG_UUID)),
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(sInfoCccd), sizeof(sInfoCccd), sInfoCccd}
    },

    // Characteristic 2: OTA_CONTROL (Write | Notify)
    [IDX_CHAR_CTRL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&CHAR_DECLARATION_UUID)),
         ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t),
         const_cast<uint8_t *>(&PROP_WRITE_NOTIFY)}
    },
    [IDX_CHAR_CTRL_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, const_cast<uint8_t *>(UUID_CHAR_CTRL_LE),
         ESP_GATT_PERM_WRITE, 256, sizeof(sDummyVal), sDummyVal}
    },
    [IDX_CHAR_CTRL_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&CHAR_CLIENT_CFG_UUID)),
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(sCtrlCccd), sizeof(sCtrlCccd), sCtrlCccd}
    },

    // Characteristic 3: OTA_DATA (Write | Write Without Response)
    [IDX_CHAR_DATA] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&CHAR_DECLARATION_UUID)),
         ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t),
         const_cast<uint8_t *>(&PROP_WRITE_NR)}
    },
    [IDX_CHAR_DATA_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, const_cast<uint8_t *>(UUID_CHAR_DATA_LE),
         ESP_GATT_PERM_WRITE, 512, sizeof(sDummyVal), sDummyVal}
    },
};

static esp_ble_adv_params_t sBleAdvParams = {
    .adv_int_min       = 0x00A0, // 100 ms
    .adv_int_max       = 0x0140, // 200 ms
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr         = {0},
    .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Raw BLE Advertisement (31 bytes exact):
//   [3B Flags (LE Gen Disc, Dual-Mode BR/EDR supported)]
//   [18B Complete 128-bit Service UUID]
//   [10B Manufacturer Specific Data: Company 0x594F + "OS" + modelLo + fwMajor/Minor/Patch]
static uint8_t sBleAdvRaw[31] = {
    0x02, 0x01, 0x02,
    0x11, 0x07,
    UUID_SERVICE_LE[0],  UUID_SERVICE_LE[1],  UUID_SERVICE_LE[2],  UUID_SERVICE_LE[3],
    UUID_SERVICE_LE[4],  UUID_SERVICE_LE[5],  UUID_SERVICE_LE[6],  UUID_SERVICE_LE[7],
    UUID_SERVICE_LE[8],  UUID_SERVICE_LE[9],  UUID_SERVICE_LE[10], UUID_SERVICE_LE[11],
    UUID_SERVICE_LE[12], UUID_SERVICE_LE[13], UUID_SERVICE_LE[14], UUID_SERVICE_LE[15],
    0x09, 0xFF,
    static_cast<uint8_t>(MANUFACTURER_COMPANY_ID & 0xFF),
    static_cast<uint8_t>((MANUFACTURER_COMPANY_ID >> 8) & 0xFF),
    MFG_MAGIC_0,
    MFG_MAGIC_1,
    static_cast<uint8_t>(ONYX_PRODUCT_ID & 0xFF),
    static_cast<uint8_t>(ONYX_FW_MAJOR),
    static_cast<uint8_t>(ONYX_FW_MINOR),
    static_cast<uint8_t>(ONYX_FW_PATCH),
};

// Classic EIR Manufacturer Specific Data (same brand payload)
static uint8_t sClassicEirMfg[8] = {
    static_cast<uint8_t>(MANUFACTURER_COMPANY_ID & 0xFF),
    static_cast<uint8_t>((MANUFACTURER_COMPANY_ID >> 8) & 0xFF),
    MFG_MAGIC_0,
    MFG_MAGIC_1,
    static_cast<uint8_t>(ONYX_PRODUCT_ID & 0xFF),
    static_cast<uint8_t>(ONYX_FW_MAJOR),
    static_cast<uint8_t>(ONYX_FW_MINOR),
    static_cast<uint8_t>(ONYX_FW_PATCH),
};

static uint8_t estimateBatteryPercent(float vBat) {
  if (vBat < BAT_PRESENT_MIN_V) return 100; // Bench/no-battery mode
  if (vBat <= 3.20f) return 0;
  if (vBat >= 4.18f) return 100;
  float pct = (vBat - 3.20f) * (100.0f / (4.18f - 3.20f));
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return static_cast<uint8_t>(pct + 0.5f);
}

static uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// ============================================================================
// Bluedroid Callbacks (execute on Core 0 BTC task — keep fast & non-blocking!)
// ============================================================================
struct BtOtaCallbacks {
  static void spp(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    BtOta *self = BtOta::instance_;
    if (!self) return;

    switch (event) {
      case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVICE_NAME);
        break;

      case ESP_SPP_START_EVT:
        self->sppReady_ = (param->start.status == ESP_SPP_SUCCESS);
        break;

      case ESP_SPP_SRV_OPEN_EVT:
        self->sppHandle_    = param->srv_open.handle;
        self->sppConnected_ = true;
        self->sppCongested_ = false;
        self->sppParser_.reset();
        break;

      case ESP_SPP_CLOSE_EVT:
        self->sppConnected_ = false;
        self->sppCongested_ = false;
        self->sppHandle_    = 0;
        self->sppParser_.reset();
        if (self->activeOtaLink_ == BtOta::LinkType::CLASSIC_SPP &&
            self->otaState_ == STATE_RECEIVING) {
          self->enqueueEvent(BtOta::LinkType::CLASSIC_SPP, CMD_OTA_ABORT, 1, nullptr, 0);
        }
        break;

      case ESP_SPP_CONG_EVT:
        self->sppCongested_ = param->cong.cong;
        break;

      case ESP_SPP_DATA_IND_EVT:
        if (param->data_ind.data && param->data_ind.len > 0) {
          self->feedStreamBytes(BtOta::LinkType::CLASSIC_SPP,
                                self->sppParser_,
                                param->data_ind.data,
                                param->data_ind.len);
        }
        break;

      default:
        break;
    }
  }

  static void bleGap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    BtOta *self = BtOta::instance_;
    if (!self) return;

    switch (event) {
      case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        self->bleAdvCfgMask_ |= 0x01;
        if (self->bleAdvCfgMask_ == 0x03) {
          esp_ble_gap_start_advertising(&sBleAdvParams);
        }
        break;

      case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        self->bleAdvCfgMask_ |= 0x02;
        if (self->bleAdvCfgMask_ == 0x03) {
          esp_ble_gap_start_advertising(&sBleAdvParams);
        }
        break;

      case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
          self->bleReady_ = true;
        }
        break;

      default:
        break;
    }
  }

  static void bleGatts(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf, esp_ble_gatts_cb_param_t *param) {
    BtOta *self = BtOta::instance_;
    if (!self) return;

    switch (event) {
      case ESP_GATTS_REG_EVT: {
        self->bleGattsIf_ = gattsIf;
        esp_ble_gap_set_device_name(BT_DEVICE_NAME);

        // Configure BLE raw advertising data (31B: Flags + 128-bit Service UUID + Brand Mfg Data)
        esp_ble_gap_config_adv_data_raw(sBleAdvRaw, sizeof(sBleAdvRaw));

        // Build Scan Response dynamically from BT_DEVICE_NAME
        uint8_t scanRsp[31] = {};
        size_t nameLen = strlen(BT_DEVICE_NAME);
        if (nameLen > 24) nameLen = 24;
        size_t pos = 0;
        scanRsp[pos++] = static_cast<uint8_t>(nameLen + 1);
        scanRsp[pos++] = 0x09; // Complete Local Name
        memcpy(&scanRsp[pos], BT_DEVICE_NAME, nameLen);
        pos += nameLen;
        scanRsp[pos++] = 0x02;
        scanRsp[pos++] = 0x0A; // TX Power Level
        scanRsp[pos++] = 0x00;
        esp_ble_gap_config_scan_rsp_data_raw(scanRsp, pos);

        esp_ble_gatts_create_attr_tab(kGattDb, gattsIf, IDX_NB, 0);
        break;
      }

      case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK && param->add_attr_tab.num_handle == IDX_NB) {
          memcpy(self->gattHandles_, param->add_attr_tab.handles, sizeof(self->gattHandles_));
          esp_ble_gatts_start_service(self->gattHandles_[IDX_SVC]);
        }
        break;

      case ESP_GATTS_CONNECT_EVT: {
        self->bleConnId_     = param->connect.conn_id;
        self->bleConnected_  = true;
        self->bleMtu_        = 23;
        self->bleInfoNotify_ = false;
        self->bleCtrlNotify_ = false;

        // Request fast connection interval (7.5..15 ms) for responsive BLE OTA throughput
        esp_ble_conn_update_params_t connParams = {};
        memcpy(connParams.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        connParams.min_int = 0x06; // 7.5 ms
        connParams.max_int = 0x0C; // 15 ms
        connParams.latency = 0;
        connParams.timeout = 400;  // 4.0 s
        esp_ble_gap_update_conn_params(&connParams);
        break;
      }

      case ESP_GATTS_DISCONNECT_EVT:
        self->bleConnected_  = false;
        self->bleInfoNotify_ = false;
        self->bleCtrlNotify_ = false;
        if (self->activeOtaLink_ == BtOta::LinkType::BLE &&
            self->otaState_ == STATE_RECEIVING) {
          self->enqueueEvent(BtOta::LinkType::BLE, CMD_OTA_ABORT, 1, nullptr, 0);
        }
        esp_ble_gap_start_advertising(&sBleAdvParams);
        break;

      case ESP_GATTS_MTU_EVT:
        self->bleMtu_ = param->mtu.mtu;
        break;

      case ESP_GATTS_READ_EVT:
        if (param->read.handle == self->gattHandles_[IDX_CHAR_INFO_VAL]) {
          DeviceInfoPayload info = {};
          self->fillDeviceInfo(info, BtOta::LinkType::BLE);
          esp_gatt_rsp_t rsp = {};
          rsp.attr_value.handle = param->read.handle;
          rsp.attr_value.len    = sizeof(DeviceInfoPayload);
          memcpy(rsp.attr_value.value, &info, sizeof(DeviceInfoPayload));
          esp_ble_gatts_send_response(gattsIf, param->read.conn_id,
                                      param->read.trans_id, ESP_GATT_OK, &rsp);
        }
        break;

      case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep && param->write.value && param->write.len > 0) {
          uint16_t h = param->write.handle;
          if (h == self->gattHandles_[IDX_CHAR_INFO_CCCD] && param->write.len >= 2) {
            self->bleInfoNotify_ = (param->write.value[0] & 0x01) != 0;
          } else if (h == self->gattHandles_[IDX_CHAR_CTRL_CCCD] && param->write.len >= 2) {
            self->bleCtrlNotify_ = (param->write.value[0] & 0x01) != 0;
          } else if (h == self->gattHandles_[IDX_CHAR_CTRL_VAL]) {
            self->parseSingleFrame(BtOta::LinkType::BLE, param->write.value, param->write.len);
          } else if (h == self->gattHandles_[IDX_CHAR_DATA_VAL]) {
            // Fast raw BLE OTA data path: [uint32 offset][chunk bytes]
            if (param->write.len > 4 && param->write.len <= MAX_FRAME_PAYLOAD) {
              self->enqueueEvent(BtOta::LinkType::BLE, CMD_OTA_DATA, 0,
                                 param->write.value, param->write.len);
            }
          }
        }
        break;

      default:
        break;
    }
  }
};

// ============================================================================
// Lifecycle & Initialization
// ============================================================================
bool BtOta::begin(AudioOutput &audio, BatteryMonitor &battery, BtAudio &btAudio) {
  audio_    = &audio;
  battery_  = &battery;
  btAudio_  = &btAudio;
  instance_ = this;

  bool okEir = setupClassicEir();
  bool okSpp = setupClassicSpp();
  bool okBle = setupBleGatt();

  return okEir && okSpp && okBle;
}

bool BtOta::setupClassicEir() {
  esp_bt_eir_data_t eir = {};
  eir.fec_required        = true;
  eir.include_txpower     = true;
  eir.include_uuid        = true;
  eir.flag                = 0;
  eir.manufacturer_len    = sizeof(sClassicEirMfg);
  eir.p_manufacturer_data = sClassicEirMfg;
  return esp_bt_gap_config_eir_data(&eir) == ESP_OK;
}

bool BtOta::setupClassicSpp() {
  if (esp_spp_register_callback(BtOtaCallbacks::spp) != ESP_OK) return false;
  if (esp_spp_init(ESP_SPP_MODE_CB) != ESP_OK) return false;
  return true;
}

bool BtOta::setupBleGatt() {
  esp_ble_gatt_set_local_mtu(517);
  if (esp_ble_gap_register_callback(BtOtaCallbacks::bleGap) != ESP_OK) return false;
  if (esp_ble_gatts_register_callback(BtOtaCallbacks::bleGatts) != ESP_OK) return false;
  if (esp_ble_gatts_app_register(GATTS_APP_ID) != ESP_OK) return false;
  return true;
}

void BtOta::confirmRunningFirmware() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) return;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
    rollbackConfirmed_ = true;
  } else {
    rollbackConfirmed_ = true;
  }
}

bool BtOta::isOtaActive() const {
  return otaState_ == STATE_RECEIVING ||
         otaState_ == STATE_VERIFYING ||
         otaState_ == STATE_COMMITTED ||
         otaState_ == STATE_REBOOTING;
}

bool BtOta::rebootRequested() const {
  return rebootDue_ && (static_cast<int32_t>(millis() - rebootAtMs_) >= 0);
}

// ============================================================================
// Queue & Framing Helpers
// ============================================================================
bool BtOta::enqueueEvent(LinkType link, uint8_t msgType, uint8_t flags,
                         const uint8_t *payload, uint16_t len) {
  if (len > MAX_FRAME_PAYLOAD) return false;
  bool ok = false;
  portENTER_CRITICAL(&rxMux_);
  uint8_t next = static_cast<uint8_t>((rxHead_ + 1) % RX_QUEUE_DEPTH);
  if (next != rxTail_) {
    RxEvent &slot = rxQueue_[rxHead_];
    slot.link    = link;
    slot.msgType = msgType;
    slot.flags   = flags;
    slot.length  = len;
    if (payload && len > 0) {
      memcpy(slot.payload, payload, len);
    }
    rxHead_ = next;
    ok = true;
  }
  portEXIT_CRITICAL(&rxMux_);
  return ok;
}

bool BtOta::dequeueEvent(RxEvent &out) {
  bool ok = false;
  portENTER_CRITICAL(&rxMux_);
  if (rxTail_ != rxHead_) {
    out = rxQueue_[rxTail_];
    rxTail_ = static_cast<uint8_t>((rxTail_ + 1) % RX_QUEUE_DEPTH);
    ok = true;
  }
  portEXIT_CRITICAL(&rxMux_);
  return ok;
}

void BtOta::feedStreamBytes(LinkType link, StreamParser &p, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = data[i];
    switch (p.state) {
      case ParseState::SYNC_0:
        if (b == FRAME_SYNC_0) p.state = ParseState::SYNC_1;
        break;

      case ParseState::SYNC_1:
        if (b == FRAME_SYNC_1) {
          p.hdrIdx = 0;
          p.state  = ParseState::HEADER;
        } else if (b != FRAME_SYNC_0) {
          p.state = ParseState::SYNC_0;
        }
        break;

      case ParseState::HEADER:
        p.hdrBuf[p.hdrIdx++] = b;
        if (p.hdrIdx == 4) {
          p.msgType = p.hdrBuf[0];
          p.flags   = p.hdrBuf[1];
          p.length  = static_cast<uint16_t>(p.hdrBuf[2]) |
                      (static_cast<uint16_t>(p.hdrBuf[3]) << 8);
          p.pos     = 0;
          if (p.length > MAX_FRAME_PAYLOAD) {
            p.reset();
          } else if (p.length == 0) {
            p.state = ParseState::CRC_LO;
          } else {
            p.state = ParseState::PAYLOAD;
          }
        }
        break;

      case ParseState::PAYLOAD:
        p.payload[p.pos++] = b;
        if (p.pos >= p.length) {
          p.state = ParseState::CRC_LO;
        }
        break;

      case ParseState::CRC_LO:
        p.rxCrc = b;
        p.state = ParseState::CRC_HI;
        break;

      case ParseState::CRC_HI: {
        p.rxCrc |= static_cast<uint16_t>(b) << 8;
        // Compute CRC over [hdrBuf(4) + payload(length)]
        uint16_t crc = 0xFFFF;
        auto feedCrc = [&](const uint8_t *buf, size_t n) {
          for (size_t k = 0; k < n; ++k) {
            crc ^= static_cast<uint16_t>(buf[k]) << 8;
            for (int bit = 0; bit < 8; ++bit) {
              crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                   : static_cast<uint16_t>(crc << 1);
            }
          }
        };
        feedCrc(p.hdrBuf, 4);
        if (p.length > 0) feedCrc(p.payload, p.length);

        if (crc == p.rxCrc) {
          enqueueEvent(link, p.msgType, p.flags, p.payload, p.length);
        }
        p.reset();
        break;
      }
    }
  }
}

bool BtOta::parseSingleFrame(LinkType link, const uint8_t *data, size_t len) {
  if (len < 8 || data[0] != FRAME_SYNC_0 || data[1] != FRAME_SYNC_1) return false;
  uint8_t  msgType = data[2];
  uint8_t  flags   = data[3];
  uint16_t payLen  = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
  if (payLen > MAX_FRAME_PAYLOAD || len != static_cast<size_t>(payLen + 8)) return false;

  uint16_t rxCrc   = static_cast<uint16_t>(data[6 + payLen]) |
                     (static_cast<uint16_t>(data[7 + payLen]) << 8);
  uint16_t calcCrc = crc16Ccitt(data + 2, payLen + 4);
  if (rxCrc != calcCrc) return false;

  return enqueueEvent(link, msgType, flags, data + 6, payLen);
}

// ============================================================================
// Core 1 Polling & Command Dispatch
// ============================================================================
void BtOta::poll() {
  // Drain all queued OTA commands/chunks cleanly on Core 1
  RxEvent ev;
  int processed = 0;
  while (processed < 16 && dequeueEvent(ev)) {
    handleEvent(ev);
    ++processed;
  }

  uint32_t now = millis();

  // Safety timeout: if an OTA session stalls for 30 seconds without packets, clean up
  if (otaState_ == STATE_RECEIVING && (now - lastActivityMs_) > 30000) {
    abortActiveOtaInternal();
  }

  // Push live telemetry notification over BLE every 2 seconds while idle & subscribed
  if (bleConnected_ && bleInfoNotify_ && !isOtaActive() && (now - lastTelemetryMs_) >= 2000) {
    lastTelemetryMs_ = now;
    DeviceInfoPayload info = {};
    fillDeviceInfo(info, LinkType::BLE);
    esp_ble_gatts_send_indicate(bleGattsIf_, bleConnId_, gattHandles_[IDX_CHAR_INFO_VAL],
                                sizeof(info), reinterpret_cast<uint8_t *>(&info), false);
  }
}

void BtOta::handleEvent(const RxEvent &ev) {
  lastActivityMs_ = millis();
  switch (ev.msgType) {
    case CMD_HELLO:      handleHello(ev.link); break;
    case CMD_OTA_BEGIN:  handleOtaBegin(ev.link, ev.payload, ev.length); break;
    case CMD_OTA_DATA:   handleOtaData(ev.link, ev.payload, ev.length); break;
    case CMD_OTA_COMMIT: handleOtaCommit(ev.link); break;
    case CMD_OTA_REBOOT: handleOtaReboot(ev.link); break;
    case CMD_OTA_ABORT:  handleOtaAbort(ev.link, ev.flags == 0); break;
    default:
      sendError(ev.link, ev.msgType, ERR_BAD_FRAME, bytesReceived_, "Unknown command");
      break;
  }
}

void BtOta::fillDeviceInfo(DeviceInfoPayload &info, LinkType forLink) const {
  memset(&info, 0, sizeof(info));
  memcpy(info.magic, "ONYX", 4);
  info.protocolVersion = PROTOCOL_VERSION;
  info.hwRev           = kFirmwareDescriptor.hwRev;
  info.productId       = kFirmwareDescriptor.productId;
  info.fwMajor         = kFirmwareDescriptor.fwMajor;
  info.fwMinor         = kFirmwareDescriptor.fwMinor;
  info.fwPatch         = kFirmwareDescriptor.fwPatch;

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)      info.runningSlot = 0;
    else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) info.runningSlot = 1;
    else                                                          info.runningSlot = 0xFF;
  } else {
    info.runningSlot = 0xFF;
  }

  const esp_partition_t *nextPart = esp_ota_get_next_update_partition(nullptr);
  info.maxOtaSize = nextPart ? nextPart->size : 0;

  if (battery_) {
    float vBat = battery_->batteryVoltage();
    info.batteryMv  = static_cast<uint16_t>(vBat * 1000.0f + 0.5f);
    info.batteryPct = estimateBatteryPercent(vBat);
    if (battery_->isDcPresent())  info.statusFlags |= STATUS_DC_PRESENT;
    if (battery_->isCharging())   info.statusFlags |= STATUS_CHARGING;
    if (battery_->isLowBattery()) info.statusFlags |= STATUS_LOW_BATTERY;
  }
  if (btAudio_ && btAudio_->isConnected()) info.statusFlags |= STATUS_BT_AUDIO_CONN;
  if (audio_ && audio_->isAuxActive())     info.statusFlags |= STATUS_AUX_ACTIVE;
  if (sppReady_)                           info.statusFlags |= STATUS_SPP_AVAILABLE;
  if (bleReady_)                           info.statusFlags |= STATUS_BLE_AVAILABLE;
  if (rollbackConfirmed_)                  info.statusFlags |= STATUS_ROLLBACK_VALID;

  info.otaState = static_cast<uint8_t>(otaState_);

  if (forLink == LinkType::BLE) {
    uint16_t usable = (bleMtu_ > 23) ? static_cast<uint16_t>(bleMtu_ - 7) : 16;
    if (usable > 496) usable = 496;
    usable = static_cast<uint16_t>(usable & ~0x3u); // 4-byte align
    if (usable < 16) usable = 16;
    info.negotiatedChunk = usable;
    info.windowPackets   = 6;
  } else {
    info.negotiatedChunk = 512;
    info.windowPackets   = 8;
  }

  const uint8_t *bda = esp_bt_dev_get_address();
  if (bda) memcpy(info.classicMac, bda, 6);

  strncpy(info.modelName, kFirmwareDescriptor.modelName, sizeof(info.modelName) - 1);
  strncpy(info.buildDate, kFirmwareDescriptor.buildDate, sizeof(info.buildDate) - 1);
  strncpy(info.buildTime, kFirmwareDescriptor.buildTime, sizeof(info.buildTime) - 1);
}

void BtOta::handleHello(LinkType link) {
  DeviceInfoPayload info = {};
  fillDeviceInfo(info, link);
  sendFrame(link, RSP_DEVICE_INFO, &info, sizeof(info));
}

void BtOta::handleOtaBegin(LinkType link, const uint8_t *payload, uint16_t len) {
  if (len < sizeof(OtaBeginPayload)) {
    sendError(link, CMD_OTA_BEGIN, ERR_BAD_FRAME, 0, "Invalid BEGIN payload");
    return;
  }
  if (otaState_ == STATE_RECEIVING && activeOtaLink_ != link) {
    sendError(link, CMD_OTA_BEGIN, ERR_BUSY_OTHER_LINK, bytesReceived_, "OTA busy on other link");
    return;
  }

  OtaBeginPayload req = {};
  memcpy(&req, payload, sizeof(req));

  // Verify product ID matches this speaker
  if (req.productId != kFirmwareDescriptor.productId) {
    sendError(link, CMD_OTA_BEGIN, ERR_MODEL_MISMATCH, 0, "Firmware model mismatch");
    return;
  }

  // Battery guard: require AC adapter OR battery >= 3.40 V (ignore bench mode < 1.50 V)
  if (battery_ && !battery_->isDcPresent()) {
    float vBat = battery_->batteryVoltage();
    if (vBat >= BAT_PRESENT_MIN_V && vBat < 3.40f) {
      sendError(link, CMD_OTA_BEGIN, ERR_LOW_BATTERY, 0, "Battery too low for OTA");
      return;
    }
  }

  // Abort any stale previous session on same link
  if (otaHandleOpen_) {
    abortActiveOtaInternal();
  }

  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (!part) {
    sendError(link, CMD_OTA_BEGIN, ERR_OTA_BEGIN_FAILED, 0, "No OTA partition available");
    return;
  }
  if (req.imageSize < 4096 || req.imageSize > part->size) {
    sendError(link, CMD_OTA_BEGIN, ERR_IMAGE_TOO_LARGE, 0, "Image exceeds OTA partition");
    return;
  }

  // Mute audio and pause Core 1 DSP task before touching SPI flash
  if (audio_) {
    audio_->setOtaMode(true);
  }

  esp_err_t err = esp_ota_begin(part, req.imageSize, &otaHandle_);
  if (err != ESP_OK) {
    if (audio_) audio_->setOtaMode(false);
    sendError(link, CMD_OTA_BEGIN, ERR_OTA_BEGIN_FAILED, 0, "esp_ota_begin failed");
    return;
  }

  mbedtls_sha256_init(&shaCtx_);
  mbedtls_sha256_starts_ret(&shaCtx_, 0);
  shaActive_ = true;

  updatePartition_ = part;
  otaHandleOpen_   = true;
  activeOtaLink_   = link;
  otaState_        = STATE_RECEIVING;
  expectedSize_    = req.imageSize;
  bytesReceived_   = 0;
  lastAckedOffset_ = 0;
  progressPct_     = 0;
  memcpy(expectedSha256_, req.sha256, 32);

  if (link == LinkType::BLE) {
    uint16_t usable = (bleMtu_ > 23) ? static_cast<uint16_t>(bleMtu_ - 7) : 16;
    if (usable > 496) usable = 496;
    usable = static_cast<uint16_t>(usable & ~0x3u);
    if (usable < 16) usable = 16;
    activeChunkSize_  = usable;
    activeWindowPkts_ = 6;
  } else {
    activeChunkSize_  = 512;
    activeWindowPkts_ = 8;
  }
  windowBytes_ = static_cast<uint32_t>(activeChunkSize_) * activeWindowPkts_;

  sendAck(link, CMD_OTA_BEGIN, ERR_NONE, activeChunkSize_, 0, activeWindowPkts_);
  sendProgress(link);
}

void BtOta::handleOtaData(LinkType link, const uint8_t *payload, uint16_t len) {
  if (otaState_ != STATE_RECEIVING || activeOtaLink_ != link || !otaHandleOpen_) {
    sendError(link, CMD_OTA_DATA, ERR_BAD_STATE, bytesReceived_, "OTA not active");
    return;
  }
  if (len <= 4) {
    sendError(link, CMD_OTA_DATA, ERR_BAD_FRAME, bytesReceived_, "Empty data chunk");
    return;
  }

  uint32_t offset = readLe32(payload);
  const uint8_t *chunk = payload + 4;
  uint16_t chunkLen = static_cast<uint16_t>(len - 4);

  if (offset != bytesReceived_) {
    uint32_t now = millis();
    if (now - lastMismatchErrMs_ >= 80) {
      lastMismatchErrMs_ = now;
      sendError(link, CMD_OTA_DATA, ERR_OFFSET_MISMATCH, bytesReceived_, "Offset mismatch");
    }
    return;
  }

  if (bytesReceived_ + chunkLen > expectedSize_) {
    sendError(link, CMD_OTA_DATA, ERR_IMAGE_TOO_LARGE, bytesReceived_, "Chunk past image end");
    abortActiveOtaInternal();
    return;
  }

  // First chunk sanity check: ESP32 image magic byte must be 0xE9
  if (bytesReceived_ == 0 && chunk[0] != 0xE9) {
    sendError(link, CMD_OTA_DATA, ERR_INVALID_IMAGE, 0, "Not a valid ESP32 binary");
    abortActiveOtaInternal();
    return;
  }

  esp_err_t err = esp_ota_write(otaHandle_, chunk, chunkLen);
  if (err != ESP_OK) {
    sendError(link, CMD_OTA_DATA, ERR_FLASH_WRITE, bytesReceived_, "Flash write failed");
    abortActiveOtaInternal();
    return;
  }

  mbedtls_sha256_update_ret(&shaCtx_, chunk, chunkLen);
  bytesReceived_ += chunkLen;
  progressPct_ = static_cast<uint8_t>((static_cast<uint64_t>(bytesReceived_) * 100ULL) / expectedSize_);

  if ((bytesReceived_ - lastAckedOffset_) >= windowBytes_ || bytesReceived_ == expectedSize_) {
    lastAckedOffset_ = bytesReceived_;
    sendAck(link, CMD_OTA_DATA, ERR_NONE, activeChunkSize_, bytesReceived_, activeWindowPkts_);
    sendProgress(link);
  }
}

bool BtOta::verifyWrittenDescriptor(const esp_partition_t *part, uint32_t imageSize) const {
  if (!part || imageSize < sizeof(FwDescriptor)) return false;

  // Scan the written partition for "ONYX_FW_DESC_V1\0" in 1024-byte overlapping windows
  constexpr size_t READ_BUF = 1024;
  constexpr size_t OVERLAP  = sizeof(FwDescriptor);
  uint8_t buf[READ_BUF + OVERLAP];

  uint32_t offset = 0;
  while (offset < imageSize) {
    uint32_t toRead = imageSize - offset;
    if (toRead > sizeof(buf)) toRead = sizeof(buf);
    if (esp_partition_read(part, offset, buf, toRead) != ESP_OK) return false;

    for (uint32_t i = 0; i + sizeof(FwDescriptor) <= toRead; ++i) {
      if (buf[i] == 'O' && memcmp(buf + i, FW_DESC_MAGIC, 16) == 0) {
        FwDescriptor desc = {};
        memcpy(&desc, buf + i, sizeof(desc));
        if (desc.productId == kFirmwareDescriptor.productId) {
          return true;
        }
      }
    }

    if (toRead <= OVERLAP) break;
    offset += READ_BUF;
  }
  return false;
}

void BtOta::handleOtaCommit(LinkType link) {
  if (otaState_ != STATE_RECEIVING || activeOtaLink_ != link || !otaHandleOpen_) {
    sendError(link, CMD_OTA_COMMIT, ERR_BAD_STATE, bytesReceived_, "No active OTA to commit");
    return;
  }
  if (bytesReceived_ != expectedSize_) {
    sendError(link, CMD_OTA_COMMIT, ERR_OFFSET_MISMATCH, bytesReceived_, "Incomplete image");
    return;
  }

  otaState_ = STATE_VERIFYING;
  sendProgress(link);

  uint8_t calcSha[32] = {};
  mbedtls_sha256_finish_ret(&shaCtx_, calcSha);
  mbedtls_sha256_free(&shaCtx_);
  shaActive_ = false;

  if (memcmp(calcSha, expectedSha256_, 32) != 0) {
    sendError(link, CMD_OTA_COMMIT, ERR_SHA256_MISMATCH, bytesReceived_, "SHA-256 verification failed");
    abortActiveOtaInternal();
    return;
  }

  // Verify the written binary actually belongs to this speaker product family
  if (!verifyWrittenDescriptor(updatePartition_, expectedSize_)) {
    sendError(link, CMD_OTA_COMMIT, ERR_MODEL_MISMATCH, bytesReceived_, "Missing or wrong Onyx descriptor");
    abortActiveOtaInternal();
    return;
  }

  esp_err_t err = esp_ota_end(otaHandle_);
  otaHandleOpen_ = false;
  if (err != ESP_OK) {
    sendError(link, CMD_OTA_COMMIT, ERR_OTA_END_FAILED, bytesReceived_, "esp_ota_end validation failed");
    abortActiveOtaInternal();
    return;
  }

  err = esp_ota_set_boot_partition(updatePartition_);
  if (err != ESP_OK) {
    sendError(link, CMD_OTA_COMMIT, ERR_OTA_END_FAILED, bytesReceived_, "Failed to set boot partition");
    abortActiveOtaInternal();
    return;
  }

  otaState_    = STATE_COMMITTED;
  progressPct_ = 100;
  sendAck(link, CMD_OTA_COMMIT, ERR_NONE, 0, bytesReceived_, 1);
  sendProgress(link);
}

void BtOta::handleOtaReboot(LinkType link) {
  if (otaState_ != STATE_COMMITTED) {
    sendError(link, CMD_OTA_REBOOT, ERR_BAD_STATE, bytesReceived_, "Commit OTA before reboot");
    return;
  }

  otaState_   = STATE_REBOOTING;
  rebootDue_  = true;
  rebootAtMs_ = millis() + 400; // Allow BT ACK/progress packet to flush cleanly
  sendAck(link, CMD_OTA_REBOOT, ERR_NONE, 0, bytesReceived_, 0);
  sendProgress(link);
}

void BtOta::handleOtaAbort(LinkType link, bool sendReply) {
  abortActiveOtaInternal();
  if (sendReply) {
    sendAck(link, CMD_OTA_ABORT, ERR_NONE, 0, 0, 0);
  }
}

void BtOta::abortActiveOtaInternal() {
  if (otaHandleOpen_) {
    esp_ota_abort(otaHandle_);
    otaHandleOpen_ = false;
  }
  if (shaActive_) {
    mbedtls_sha256_free(&shaCtx_);
    shaActive_ = false;
  }
  otaState_        = STATE_IDLE;
  activeOtaLink_   = LinkType::NONE;
  updatePartition_ = nullptr;
  expectedSize_    = 0;
  bytesReceived_   = 0;
  lastAckedOffset_ = 0;
  progressPct_     = 0;

  if (audio_) {
    audio_->setOtaMode(false);
  }
}

// ============================================================================
// Wire TX Helpers (Classic SPP & BLE GATT Control Notification)
// ============================================================================
bool BtOta::sendFrame(LinkType link, uint8_t msgType, const void *payload, uint16_t len) {
  if (len > 220) return false;

  uint8_t frame[256];
  frame[0] = FRAME_SYNC_0;
  frame[1] = FRAME_SYNC_1;
  frame[2] = msgType;
  frame[3] = 0x00;
  frame[4] = static_cast<uint8_t>(len & 0xFF);
  frame[5] = static_cast<uint8_t>((len >> 8) & 0xFF);
  if (payload && len > 0) {
    memcpy(frame + 6, payload, len);
  }
  uint16_t crc = crc16Ccitt(frame + 2, len + 4);
  frame[6 + len] = static_cast<uint8_t>(crc & 0xFF);
  frame[7 + len] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  uint16_t totalLen = static_cast<uint16_t>(len + 8);

  if (link == LinkType::CLASSIC_SPP) {
    if (!sppConnected_ || sppHandle_ == 0) return false;
    uint32_t t0 = millis();
    while (sppCongested_ && (millis() - t0) < 300) {
      delay(2);
    }
    return esp_spp_write(sppHandle_, totalLen, frame) == ESP_OK;
  }

  if (link == LinkType::BLE) {
    if (!bleConnected_ || !bleCtrlNotify_) return false;
    return esp_ble_gatts_send_indicate(bleGattsIf_, bleConnId_, gattHandles_[IDX_CHAR_CTRL_VAL],
                                       totalLen, frame, false) == ESP_OK;
  }
  return false;
}

void BtOta::sendAck(LinkType link, uint8_t origCmd, uint8_t statusCode,
                    uint16_t chunkSize, uint32_t nextOffset, uint32_t detail) {
  AckPayload p = {};
  p.origCmd    = origCmd;
  p.statusCode = statusCode;
  p.chunkSize  = chunkSize;
  p.nextOffset = nextOffset;
  p.detail     = detail;
  sendFrame(link, RSP_ACK, &p, sizeof(p));
}

void BtOta::sendProgress(LinkType link) {
  ProgressPayload p = {};
  p.bytesReceived = bytesReceived_;
  p.totalBytes    = expectedSize_;
  p.otaState      = static_cast<uint8_t>(otaState_);
  p.percent       = progressPct_;
  sendFrame(link, EVT_PROGRESS, &p, sizeof(p));
}

void BtOta::sendError(LinkType link, uint8_t origCmd, uint8_t errCode,
                      uint32_t expectedOffset, const char *msg) {
  uint8_t buf[96] = {};
  ErrorPayload hdr = {};
  hdr.origCmd        = origCmd;
  hdr.errCode        = errCode;
  hdr.expectedOffset = expectedOffset;
  memcpy(buf, &hdr, sizeof(hdr));
  size_t msgLen = msg ? strlen(msg) : 0;
  if (msgLen > sizeof(buf) - sizeof(hdr) - 1) msgLen = sizeof(buf) - sizeof(hdr) - 1;
  if (msgLen > 0) memcpy(buf + sizeof(hdr), msg, msgLen);
  sendFrame(link, RSP_ERROR, buf, static_cast<uint16_t>(sizeof(hdr) + msgLen));
}
